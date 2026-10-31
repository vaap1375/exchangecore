#include "exchangecore/gateway.hpp"

#include <charconv>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> stop_requested{false};
void stop(int) { stop_requested.store(true, std::memory_order_relaxed); }

std::uint64_t number(std::string_view text) {
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size())
        throw std::invalid_argument("invalid unsigned integer: " + std::string(text));
    return value;
}

constexpr std::string_view help = R"(ExchangeCore optional POSIX TCP gateway
Usage: exchangecore_gateway [--host IPv4] [--port N] [options]
  --host IPv4                 Default 127.0.0.1 (localhost also accepted)
  --port N                    0..65535; zero selects an ephemeral port (default 9000)
  --max-clients N             1..4096 (default 128)
  --queue-capacity N          Request FIFO 1..65536 (default 1024)
  --completion-capacity N     Completion FIFO 1..65536 (default 1024)
  --pending-per-client N      1..1024 (default 16)
  --max-input N               1..4096 bytes excluding LF (default 4096)
  --max-output N              256..1048576 buffered bytes/client (default 65536)
  --max-orders N              1..10000000 lifetime IDs (default 1000000)
  --shutdown-grace-ms N       0..60000 after matching drains (default 2000)
  --send-buffer N             Optional kernel SO_SNDBUF hint for tests (default 0)
  --socket-send-buffer N      Alias for --send-buffer
  --write-chunk N             1..65536 bytes per send (default 65536)
SIGINT/SIGTERM close admission, process every admitted request, then drain replies
within response grace. Protocol: EC1 request_id COMMAND ... followed by LF.
)";
} // namespace

int main(int argc, char** argv) {
    try {
        exchangecore::GatewayConfig config;
        for (int i = 1; i < argc; ++i) {
            const std::string_view option(argv[i]);
            if (option == "--help" || option == "-h") { std::cout << help; return 0; }
            if (i + 1 == argc) throw std::invalid_argument("missing value for " + std::string(option));
            const std::string_view value(argv[++i]);
            if (option == "--host") { config.host = value; continue; }
            const auto parsed = number(value);
            if (parsed > std::numeric_limits<std::size_t>::max())
                throw std::invalid_argument("integer exceeds platform size_t");
            const auto n = static_cast<std::size_t>(parsed);
            if (option == "--port") {
                if (n > 65535) throw std::invalid_argument("port exceeds 65535");
                config.port = static_cast<std::uint16_t>(n);
            } else if (option == "--max-clients") config.max_clients = n;
            else if (option == "--queue-capacity") config.queue_capacity = n;
            else if (option == "--completion-capacity") config.completion_capacity = n;
            else if (option == "--pending-per-client") config.pending_per_client = n;
            else if (option == "--max-input") config.max_input = n;
            else if (option == "--max-output") config.max_output = n;
            else if (option == "--max-orders") config.max_orders = n;
            else if (option == "--shutdown-grace-ms") {
                if (n > 60000) throw std::invalid_argument("shutdown grace exceeds 60000ms");
                config.shutdown_grace = std::chrono::milliseconds(n);
            } else if (option == "--send-buffer" || option == "--socket-send-buffer") {
                if (n > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                    throw std::invalid_argument("send buffer exceeds int range");
                config.send_buffer = static_cast<int>(n);
            } else if (option == "--write-chunk") {
                config.write_chunk = n;
            } else throw std::invalid_argument("unknown gateway option: " + std::string(option));
        }
        struct sigaction action{};
        action.sa_handler = stop;
        sigemptyset(&action.sa_mask);
        if (::sigaction(SIGINT, &action, nullptr) < 0 || ::sigaction(SIGTERM, &action, nullptr) < 0)
            throw std::runtime_error("cannot install shutdown signal handlers");
        const auto stats = exchangecore::run_gateway(config, std::cout, stop_requested);
        std::cout << "STATS accepted=" << stats.accepted << " processed=" << stats.processed
                  << " overloaded=" << stats.overloaded << " queue_high_water=" << stats.queue_high_water
                  << " completion_high_water=" << stats.completion_high_water
                  << " connections=" << stats.connections << " closed=" << stats.closed
                  << " discarded=" << stats.discarded << " parse_errors=" << stats.parse_errors
                  << " write_calls=" << stats.write_calls << " partial_writes=" << stats.partial_writes
                  << " short_writes=" << stats.short_writes << " would_block=" << stats.would_block
                  << " output_high_water=" << stats.output_high_water
                  << " input_high_water=" << stats.input_high_water
                  << " pending_high_water=" << stats.pending_high_water
                  << " setup_failures=" << stats.setup_failures
                  << " startup_rss_kib=" << stats.startup_rss_kib
                  << " peak_rss_kib=" << stats.peak_rss_kib << '\n';
        return stats.accepted == stats.processed ? 0 : 1;
    } catch (const std::invalid_argument& error) {
        std::cerr << "gateway: " << error.what() << '\n'; return 2;
    } catch (const std::exception& error) {
        std::cerr << "gateway: " << error.what() << '\n'; return 1;
    }
}
