#include "exchangecore/gateway.hpp"

#include "exchangecore/bounded_queue.hpp"
#include "exchangecore/matching_engine.hpp"
#include "exchangecore/protocol.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <unistd.h>

namespace exchangecore {
namespace {
namespace wire = protocol;
using Clock = std::chrono::steady_clock;

class Socket {
public:
    explicit Socket(int fd = -1) noexcept : fd_(fd) {}
    ~Socket() { reset(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) { reset(); fd_ = std::exchange(other.fd_, -1); }
        return *this;
    }
    [[nodiscard]] int get() const noexcept { return fd_; }
    void reset() noexcept {
        // Never retry close(EINTR): the descriptor may already have been freed.
        if (fd_ >= 0) { (void)::close(fd_); fd_ = -1; }
    }
private:
    int fd_;
};

[[noreturn]] void socket_error(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

void nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        socket_error("fcntl nonblocking");
    const int descriptor_flags = ::fcntl(fd, F_GETFD, 0);
    if (descriptor_flags < 0 || ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0)
        socket_error("fcntl close-on-exec");
#if defined(SO_NOSIGPIPE)
    const int enabled = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0)
        socket_error("setsockopt SO_NOSIGPIPE");
#endif
}

constexpr int send_flags() noexcept {
#if defined(MSG_NOSIGNAL)
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

void wake(int fd) noexcept {
    const char byte = 'w';
    while (::send(fd, &byte, 1, send_flags()) < 0 && errno == EINTR) {}
    // EAGAIN means an earlier byte already makes the poll endpoint readable.
}

struct QueuedRequest {
    std::uint64_t connection{};
    std::uint64_t sequence{};
    wire::Request request{};
};

struct Completion {
    std::uint64_t connection{};
    std::uint64_t sequence{};
    std::uint64_t request_id{};
    wire::Command command{wire::Command::Ping};
    Result result{};
    std::optional<Price> bid;
    std::optional<Price> ask;
    std::size_t active{};
    Quantity quantity{};
    std::string_view error;
};

struct ResponseSlot {
    std::uint64_t request_id{};
    std::uint64_t sequence{};
    std::optional<Completion> completion;
    std::size_t trade_index{};
    unsigned phase{}; // 0 header, 1 trade stream, 2 END
};

struct Connection {
    Socket socket;
    std::string input;
    std::string output;
    std::size_t output_offset{};
    std::deque<ResponseSlot> pending;
    std::uint64_t last_request_id{};
    bool read_closed{};
    bool close_after_output{};

    Connection(Socket fd, const GatewayConfig& config) : socket(std::move(fd)) {
        input.reserve(config.max_input + 1);
        output.reserve(config.max_output);
    }
};

void validate(const GatewayConfig& config) {
    if (config.max_clients == 0 || config.max_clients > 4096 ||
        config.queue_capacity == 0 || config.queue_capacity > 65'536 ||
        config.completion_capacity == 0 || config.completion_capacity > 65'536 ||
        config.pending_per_client == 0 || config.pending_per_client > 1024 ||
        config.max_input == 0 || config.max_input > wire::max_frame_bytes ||
        config.max_output < wire::max_response_line_bytes || config.max_output > 1'048'576 ||
        config.max_orders == 0 || config.max_orders > 10'000'000 ||
        config.shutdown_grace.count() < 0 || config.shutdown_grace.count() > 60'000 ||
        config.send_buffer < 0 || config.write_chunk == 0 || config.write_chunk > 65'536) {
        throw std::invalid_argument("gateway configuration outside supported bounds (see --help)");
    }
}

std::uint64_t peak_rss_kib() noexcept {
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0) return 0;
    const auto rss = static_cast<std::uint64_t>(usage.ru_maxrss);
#if defined(__APPLE__)
    return rss / 1024;
#else
    return rss;
#endif
}

void local_error(Connection& connection, std::uint64_t request_id, std::string_view code,
                 GatewayStats& stats) {
    Completion completion;
    completion.request_id = request_id;
    completion.error = code;
    ResponseSlot slot;
    slot.request_id = request_id;
    slot.completion = std::move(completion);
    connection.pending.push_back(std::move(slot));
    stats.pending_high_water = std::max(stats.pending_high_water, connection.pending.size());
}

// Advances only after the line has been appended, so a full output buffer keeps
// the exact cursor for the next partial send. No complete response is serialized.
void render(Connection& connection, const GatewayConfig& config, GatewayStats& stats) {
    if (connection.output_offset != 0) {
        connection.output.erase(0, connection.output_offset);
        connection.output_offset = 0;
    }
    while (!connection.pending.empty() && connection.pending.front().completion) {
        auto& slot = connection.pending.front();
        const auto& completion = *slot.completion;
        std::string line;
        unsigned next_phase = slot.phase;
        bool finished = false;
        if (slot.phase == 0) {
            if (!completion.error.empty())
                line = wire::format_error(slot.request_id, slot.sequence, completion.error);
            else if (completion.command == wire::Command::Best)
                line = wire::format_best(slot.request_id, slot.sequence, completion.bid, completion.ask,
                                         completion.active, completion.quantity);
            else if (completion.command == wire::Command::Ping)
                line = wire::format_pong(slot.request_id, slot.sequence);
            else
                line = wire::format_result(slot.request_id, slot.sequence, completion.result);
            next_phase = completion.error.empty() && !completion.result.trades.empty() ? 1U : 2U;
        } else if (slot.phase == 1) {
            line = wire::format_trade(slot.request_id, slot.sequence,
                                     completion.result.trades[slot.trade_index]);
            next_phase = slot.trade_index + 1 == completion.result.trades.size() ? 2U : 1U;
        } else {
            line = wire::format_end(slot.request_id, slot.sequence);
            finished = true;
        }
        if (line.size() > config.max_output - connection.output.size()) break;
        connection.output.append(line);
        stats.output_high_water = std::max(stats.output_high_water, connection.output.size());
        if (slot.phase == 1) ++slot.trade_index;
        slot.phase = next_phase;
        if (finished) connection.pending.pop_front();
    }
}

bool writable(Connection& connection, const GatewayConfig& config, GatewayStats& stats) {
    // Fairness: cap one connection's writes per poll cycle. All syscalls remain
    // nonblocking; readiness can become stale without blocking another client.
    std::size_t sent_this_cycle = 0;
    while (connection.output_offset < connection.output.size() && sent_this_cycle < 65'536) {
        const auto requested = std::min(config.write_chunk, connection.output.size() - connection.output_offset);
        const auto count = ::send(connection.socket.get(),
                                  connection.output.data() + connection.output_offset,
                                  requested, send_flags());
        ++stats.write_calls;
        if (count > 0) {
            connection.output_offset += static_cast<std::size_t>(count);
            sent_this_cycle += static_cast<std::size_t>(count);
            if (static_cast<std::size_t>(count) < requested) ++stats.short_writes;
            if (connection.output_offset < connection.output.size()) ++stats.partial_writes;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            ++stats.would_block;
            return true;
        } else {
            return false;
        }
    }
    if (connection.output_offset == connection.output.size()) {
        connection.output.clear();
        connection.output_offset = 0;
    }
    return true;
}

} // namespace

GatewayStats run_gateway(const GatewayConfig& config, std::ostream& status,
                         const std::atomic<bool>& stop_requested) {
    validate(config);
    const std::string host = config.host == "localhost" ? "127.0.0.1" : config.host;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config.port);
    if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1)
        throw std::invalid_argument("host must be a numeric IPv4 address or localhost");
    Socket listener(::socket(AF_INET, SOCK_STREAM, 0));
    if (listener.get() < 0) socket_error("socket");
    nonblocking(listener.get());
    const int enabled = 1;
    if (::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0)
        socket_error("setsockopt SO_REUSEADDR");
    if (::bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0)
        socket_error("bind");
    if (::listen(listener.get(), static_cast<int>(config.max_clients)) < 0) socket_error("listen");
    socklen_t address_length = sizeof(address);
    if (::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &address_length) < 0)
        socket_error("getsockname");

    int pair[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) socket_error("socketpair");
    Socket wake_read(pair[0]);
    Socket wake_write(pair[1]);
    nonblocking(wake_read.get());
    nonblocking(wake_write.get());
    BoundedQueue<QueuedRequest> requests(config.queue_capacity);
    BoundedQueue<Completion> completions(config.completion_capacity);
    std::atomic<bool> worker_done{false};
    std::atomic<std::uint64_t> processed{0};
    std::atomic<bool> invariant_failed{false};
    GatewayStats stats;
    stats.startup_rss_kib = peak_rss_kib();
    std::jthread worker([&, engine = std::make_unique<MatchingEngine>(EngineConfig{0, config.max_orders})] {
        QueuedRequest request;
        while (requests.wait_pop(request)) {
            Completion completion;
            completion.connection = request.connection;
            completion.sequence = request.sequence;
            completion.request_id = request.request.request_id;
            completion.command = request.request.command;
            try {
                if (completion.command == wire::Command::Best) {
                    completion.bid = engine->best_bid();
                    completion.ask = engine->best_ask();
                    completion.active = engine->active_order_count();
                    completion.quantity = engine->resting_quantity();
                } else if (completion.command != wire::Command::Ping) {
                    completion.result = engine->process(request.request.event);
                }
            } catch (...) {
                // Expected validation failures are Results. An exceptional
                // processing failure is explicit and does not claim execution.
                completion.error = "INTERNAL";
            }
            processed.fetch_add(1, std::memory_order_relaxed);
            (void)completions.wait_push(std::move(completion));
            wake(wake_write.get());
        }
        try { engine->check_invariants(); }
        catch (...) { invariant_failed.store(true, std::memory_order_relaxed); }
        worker_done.store(true, std::memory_order_release);
        wake(wake_write.get());
    });

    std::map<std::uint64_t, Connection> clients;
    std::uint64_t next_connection = 1;
    std::uint64_t next_sequence = 1;
    bool shutting_down = false;
    std::optional<Clock::time_point> response_deadline;

    const auto close_client = [&](auto iterator) {
        for (const auto& slot : iterator->second.pending)
            if (slot.sequence != 0 && slot.completion) ++stats.discarded;
        ++stats.closed;
        return clients.erase(iterator);
    };

    const auto admit = [&](std::uint64_t token, Connection& connection) {
        while (connection.pending.size() < config.pending_per_client &&
               connection.output.size() - connection.output_offset < config.max_output &&
               !connection.close_after_output) {
            const auto newline = connection.input.find('\n');
            if (newline == std::string::npos) {
                if (connection.input.size() > config.max_input) {
                    local_error(connection, 0, "LINE_TOO_LARGE", stats);
                    ++stats.parse_errors;
                    connection.input.clear();
                    connection.close_after_output = true;
                    connection.read_closed = true;
                }
                break;
            }
            const auto parsed = wire::parse_request(std::string_view(connection.input).substr(0, newline));
            connection.input.erase(0, newline + 1);
            if (!parsed) {
                local_error(connection, parsed.request.request_id, wire::to_string(parsed.error), stats);
                ++stats.parse_errors;
                continue;
            }
            if (parsed.request.request_id <= connection.last_request_id) {
                local_error(connection, parsed.request.request_id, "DUPLICATE_REQUEST", stats);
                ++stats.parse_errors;
                continue;
            }
            connection.last_request_id = parsed.request.request_id;
            if (next_sequence == std::numeric_limits<std::uint64_t>::max()) {
                local_error(connection, parsed.request.request_id, "SEQUENCE_EXHAUSTED", stats);
                continue;
            }
            // Allocate response tracking before queue admission. A success can
            // never lose its route through a later deque allocation failure.
            ResponseSlot slot;
            slot.request_id = parsed.request.request_id;
            slot.sequence = next_sequence;
            connection.pending.push_back(std::move(slot));
            stats.pending_high_water = std::max(stats.pending_high_water, connection.pending.size());
            QueuedRequest request{token, next_sequence, parsed.request};
            if (requests.try_push(std::move(request))) {
                ++next_sequence;
                ++stats.accepted;
            } else {
                connection.pending.pop_back();
                local_error(connection, parsed.request.request_id, "OVERLOADED", stats);
                ++stats.overloaded;
            }
        }
    };

    try {
        status << "LISTEN " << host << ' ' << ntohs(address.sin_port) << '\n' << std::flush;
        if (!status) throw std::runtime_error("cannot write gateway startup status");
        while (true) {
            if (stop_requested.load(std::memory_order_relaxed) && !shutting_down) {
                shutting_down = true;
                listener.reset();
                requests.close();
                for (auto& [token, connection] : clients) {
                    static_cast<void>(token);
                    connection.read_closed = true;
                    connection.input.clear();
                }
            }
            // Drain completions independently of socket writability: a slow
            // client must not pin the matching writer on a full completion FIFO.
            Completion completion;
            while (completions.try_pop(completion)) {
                const auto found = clients.find(completion.connection);
                bool routed = false;
                if (found != clients.end()) {
                    for (auto& slot : found->second.pending) {
                        if (slot.sequence == completion.sequence) {
                            slot.completion = std::move(completion);
                            routed = true;
                            break;
                        }
                    }
                }
                if (!routed) ++stats.discarded;
            }
            if (shutting_down && worker_done.load(std::memory_order_acquire) && !response_deadline)
                response_deadline = Clock::now() + config.shutdown_grace;

            for (auto client = clients.begin(); client != clients.end();) {
                auto& connection = client->second;
                render(connection, config, stats);
                if (!shutting_down) admit(client->first, connection);
                render(connection, config, stats);
                if ((connection.read_closed || connection.close_after_output || shutting_down) &&
                    connection.pending.empty() && connection.output.empty()) {
                    client = close_client(client);
                } else {
                    ++client;
                }
            }
            if (shutting_down && response_deadline &&
                (clients.empty() || Clock::now() >= *response_deadline)) break;

            std::vector<pollfd> descriptors;
            std::vector<std::uint64_t> tokens;
            descriptors.reserve(clients.size() + 2);
            tokens.reserve(clients.size());
            descriptors.push_back({wake_read.get(), POLLIN, 0});
            descriptors.push_back({listener.get(), static_cast<short>(shutting_down ? 0 : POLLIN), 0});
            for (const auto& [token, connection] : clients) {
                short events = 0;
                if (!connection.read_closed && !shutting_down &&
                    connection.pending.size() < config.pending_per_client &&
                    connection.output.size() - connection.output_offset < config.max_output)
                    events |= POLLIN;
                if (connection.output_offset < connection.output.size()) events |= POLLOUT;
                // poll reports HUP even with events=0. A temporarily paused
                // connection waits for the completion wakeup instead of spinning.
                descriptors.push_back({events == 0 ? -1 : connection.socket.get(), events, 0});
                tokens.push_back(token);
            }
            const int ready = ::poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()), 50);
            if (ready < 0) {
                if (errno == EINTR) continue;
                socket_error("poll");
            }
            if ((descriptors[0].revents & POLLIN) != 0) {
                std::array<char, 256> bytes{};
                while (::recv(wake_read.get(), bytes.data(), bytes.size(), 0) > 0) {}
            }
            if (!shutting_down && (descriptors[1].revents & POLLIN) != 0) {
                // Limit accept work per cycle so a connection flood cannot
                // starve responses, signals or existing sockets.
                for (std::size_t attempt = 0; attempt < 64; ++attempt) {
                    Socket accepted(::accept(listener.get(), nullptr, nullptr));
                    if (accepted.get() < 0) {
                        if (errno == EINTR) continue;
                        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED) break;
                        socket_error("accept");
                    }
                    if (clients.size() >= config.max_clients ||
                        next_connection == std::numeric_limits<std::uint64_t>::max()) continue;
                    try {
                        nonblocking(accepted.get());
                        if (::setsockopt(accepted.get(), IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) < 0)
                            socket_error("setsockopt TCP_NODELAY");
                        if (config.send_buffer != 0 &&
                            ::setsockopt(accepted.get(), SOL_SOCKET, SO_SNDBUF,
                                         &config.send_buffer, sizeof(config.send_buffer)) < 0)
                            socket_error("setsockopt SO_SNDBUF");
                    } catch (const std::system_error&) {
                        // A peer may reset between accept and setsockopt. In
                        // particular macOS SO_NOSIGPIPE can then report EINVAL.
                        // This descriptor is local; other clients stay alive.
                        ++stats.setup_failures;
                        continue;
                    }
                    clients.try_emplace(next_connection++, std::move(accepted), config);
                    ++stats.connections;
                }
            }
            for (std::size_t i = 0; i < tokens.size(); ++i) {
                const auto found = clients.find(tokens[i]);
                if (found == clients.end()) continue;
                auto& connection = found->second;
                const short revents = descriptors[i + 2].revents;
                bool alive = (revents & (POLLERR | POLLNVAL)) == 0;
                if (alive && (revents & (POLLOUT | POLLHUP)) != 0 &&
                    connection.output_offset < connection.output.size())
                    alive = writable(connection, config, stats);
                if (alive && !connection.read_closed && !shutting_down &&
                    (revents & (POLLIN | POLLHUP)) != 0) {
                    std::array<char, 4096> bytes{};
                    std::size_t received_this_cycle = 0;
                    while (connection.pending.size() < config.pending_per_client &&
                           connection.output.size() - connection.output_offset < config.max_output &&
                           !connection.read_closed && received_this_cycle < 65'536) {
                        const auto room = config.max_input + 1 - connection.input.size();
                        const auto count = ::recv(connection.socket.get(), bytes.data(),
                                                  std::min(bytes.size(), room), 0);
                        if (count > 0) {
                            connection.input.append(bytes.data(), static_cast<std::size_t>(count));
                            stats.input_high_water = std::max(stats.input_high_water, connection.input.size());
                            received_this_cycle += static_cast<std::size_t>(count);
                            admit(found->first, connection);
                            render(connection, config, stats);
                        } else if (count == 0) {
                            connection.read_closed = true;
                            // FIN still permits writing queued responses. An
                            // unterminated final frame was never admitted.
                            if (!connection.input.empty()) {
                                const auto partial = wire::parse_request(connection.input);
                                local_error(connection, partial.request.request_id, "INCOMPLETE_FRAME", stats);
                                ++stats.parse_errors;
                            }
                            connection.input.clear();
                        } else if (errno == EINTR) {
                            continue;
                        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        } else {
                            alive = false;
                            break;
                        }
                    }
                }
                if (!alive) close_client(found);
            }
        }
    } catch (...) {
        // Unblock both queue waits before joining. The writer still drains all
        // admitted requests, discarding completions when IO cannot continue.
        requests.close();
        completions.close();
        worker.join();
        throw;
    }
    requests.close();
    worker.join();
    // The final producer push may race the last IO drain immediately before
    // worker_done becomes visible (especially with zero response grace).
    Completion leftover;
    while (completions.try_pop(leftover)) ++stats.discarded;
    while (!clients.empty()) close_client(clients.begin());
    stats.processed = processed.load(std::memory_order_relaxed);
    stats.queue_high_water = requests.high_watermark();
    stats.completion_high_water = completions.high_watermark();
    stats.peak_rss_kib = peak_rss_kib();
    if (invariant_failed.load(std::memory_order_relaxed))
        throw std::runtime_error("matching invariants failed during gateway shutdown");
    return stats;
}

} // namespace exchangecore
