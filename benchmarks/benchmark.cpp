#include "exchangecore/benchmark.hpp"
#include "exchangecore/matching_engine.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <exception>
#include <iomanip>
#include <limits>
#include <ostream>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#include <sys/utsname.h>
#endif

namespace exchangecore {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::array<std::string_view, 6> workload_names{
    "resting", "crossing", "cancellation", "deep", "touch", "mixed"};
constexpr std::size_t maximum_events = 5'000'000;

struct ReplaySummary {
    std::uint64_t checksum{0xcbf29ce484222325ULL};
    std::uint64_t timing_digest{};
    std::uint64_t accepted{};
    std::uint64_t rejected{};
    std::uint64_t trades{};
    std::uint64_t canceled{};
    std::uint64_t filled{};
};

void fold(std::uint64_t& hash, std::uint64_t value) noexcept {
    // Unsigned overflow is deliberate, defined modular arithmetic.
    hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
}

void record(ReplaySummary& summary, const Result& result) noexcept {
    summary.accepted += static_cast<std::uint64_t>(result.accepted);
    summary.rejected += static_cast<std::uint64_t>(!result.accepted);
    summary.trades += result.trades.size();
    summary.canceled += static_cast<std::uint64_t>(result.canceled != 0);
    summary.filled += result.filled;
    summary.timing_digest += result.filled + result.remaining + result.canceled +
                             result.trades.size() + static_cast<std::uint64_t>(result.accepted);
    fold(summary.checksum, static_cast<std::uint64_t>(result.accepted));
    fold(summary.checksum, static_cast<std::uint64_t>(result.reason));
    fold(summary.checksum, result.order_id);
    fold(summary.checksum, result.filled);
    fold(summary.checksum, result.remaining);
    fold(summary.checksum, result.canceled);
    fold(summary.checksum, result.trades.size());
    for (const auto& trade : result.trades) {
        fold(summary.checksum, trade.maker_id);
        fold(summary.checksum, trade.taker_id);
        fold(summary.checksum, static_cast<std::uint64_t>(trade.price));
        fold(summary.checksum, trade.quantity);
    }
}

ReplaySummary validate_replay(MatchingEngine& engine, const std::vector<Event>& events) {
    ReplaySummary summary;
    for (const auto& event : events) {
        record(summary, engine.process(event));
    }
    engine.check_invariants();
    return summary;
}

std::uint64_t state_checksum(const MatchingEngine& engine) {
    auto hash = std::uint64_t{0xcbf29ce484222325ULL};
    const auto state = engine.snapshot();
    for (const auto* side : {&state.bids, &state.asks}) {
        fold(hash, side->size());
        for (const auto& order : *side) {
            fold(hash, order.id);
            fold(hash, static_cast<std::uint64_t>(order.side));
            fold(hash, static_cast<std::uint64_t>(order.price));
            fold(hash, order.quantity);
            fold(hash, order.sequence);
        }
    }
    return hash;
}

// Throughput runs deliberately avoid per-event clocks and full trade hashing.
// A small result reduction keeps every result observable. Detailed result and
// state hashes are verified in separate, untimed replays.
std::uint64_t replay_timed_path(MatchingEngine& engine, const std::vector<Event>& events) {
    std::uint64_t digest{};
    for (const auto& event : events) {
        const auto result = engine.process(event);
        digest += result.filled + result.remaining + result.canceled +
                  result.trades.size() + static_cast<std::uint64_t>(result.accepted);
    }
    return digest;
}

void validate_activity(std::string_view name, const ReplaySummary& summary,
                       const MatchingEngine& engine, std::size_t count) {
    if (summary.rejected != 0 || summary.accepted != count) {
        throw std::runtime_error("benchmark generated an unexpectedly rejected event");
    }
    if (name == "resting" || name == "deep") {
        if (summary.trades != 0 || summary.canceled != 0 || engine.active_order_count() != count) {
            throw std::runtime_error("resting workload activity mismatch");
        }
    } else if (name == "crossing" || name == "touch") {
        if (summary.trades < count / 4) {
            throw std::runtime_error("crossing workload did not produce expected executions");
        }
    } else if (name == "cancellation") {
        if (summary.canceled < count / 2 - 1 || summary.trades != 0 ||
            engine.active_order_count() > 2) {
            throw std::runtime_error("cancellation workload activity mismatch");
        }
    } else if (name == "mixed") {
        if (summary.trades < count / 5 || summary.canceled < count / 10 ||
            engine.active_order_count() > 3) {
            throw std::runtime_error("mixed workload activity mismatch");
        }
    }
    if (name == "deep" &&
        engine.depth(Side::Buy).size() + engine.depth(Side::Sell).size() != count) {
        throw std::runtime_error("deep workload did not create one distinct price level per event");
    }
}

double percentile(const std::vector<double>& sorted, double proportion) {
    const auto rank = static_cast<std::size_t>(
        std::ceil(proportion * static_cast<double>(sorted.size())));
    return sorted[std::max(std::size_t{1}, rank) - 1];
}

std::uint64_t peak_rss_kib() noexcept {
#if defined(__unix__) || defined(__APPLE__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0) {
        return 0;
    }
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(usage.ru_maxrss) / 1024U;
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#endif
#else
    return 0;
#endif
}

std::string platform_name() {
#if defined(__unix__) || defined(__APPLE__)
    utsname value{};
    if (uname(&value) == 0) {
        return std::string(value.sysname) + " " + value.release + " " + value.machine;
    }
#endif
    return "unknown";
}

std::string compiler_name() {
#if defined(__clang__)
    return std::string("Clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("GCC ") + __VERSION__;
#else
    return "unknown";
#endif
}

std::string build_mode() {
#if defined(NDEBUG)
    return "Release (NDEBUG)";
#else
    return "Debug (invariants may dominate; do not use for performance claims)";
#endif
}

void csv_string(std::ostream& output, std::string_view value) {
    output << '"';
    for (char character : value) {
        if (character == '"') {
            output << '"';
        }
        output << character;
    }
    output << '"';
}

void usage(std::ostream& output) {
    output << "Benchmark options:\n"
              "  --events N            Events per workload (64..5000000; default 100000)\n"
              "  --trials N            Throughput trials (1..100; default 5)\n"
              "  --warmup N            Untimed warmup replays (0..100; default 1)\n"
              "  --samples N           Sampled events, separate replay (clamped to events; default 10000)\n"
              "  --latency-samples N   Alias for --samples\n"
              "  --seed N              Unsigned deterministic seed (default 60448)\n"
              "  --reserve N           Reserve engine hash indexes (0=baseline; default 0)\n"
              "  --workload NAME       all, resting, crossing, cancellation, deep, touch, mixed\n"
              "  --csv                 Machine-readable CSV, one row per workload\n"
              "  --help                Show this help\n";
}

std::uint64_t parse_unsigned(std::string_view text) {
    std::uint64_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        throw std::invalid_argument("expected an unsigned decimal integer: " + std::string(text));
    }
    return value;
}

std::size_t parse_size(std::string_view text) {
    const auto value = parse_unsigned(text);
    if (value > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument("integer exceeds platform size_t range");
    }
    return static_cast<std::size_t>(value);
}

}  // namespace

std::vector<Event> make_benchmark_workload(std::string_view name, std::size_t count,
                                          std::uint64_t seed) {
    if (std::find(workload_names.begin(), workload_names.end(), name) == workload_names.end()) {
        throw std::invalid_argument("unknown benchmark workload: " + std::string(name));
    }
    if (count > maximum_events) {
        throw std::invalid_argument("workload exceeds 5000000 event safety limit");
    }
    std::mt19937_64 random(seed);
    std::vector<Event> events;
    events.reserve(count);
    OrderId next_id = 1;
    auto add_limit = [&](Side side, Quantity quantity, Price price) {
        const auto id = next_id++;
        events.push_back(Event::limit(id, side, quantity, price));
        return id;
    };
    auto add_market = [&](Side side, Quantity quantity) {
        events.push_back(Event::market(next_id++, side, quantity));
    };
    auto add_cancel = [&](OrderId id) { events.push_back(Event::cancel(id)); };
    // Every complete block ends with an empty book. A truncated final block is
    // valid and intentionally contributes its final resting state to the hash.
    while (events.size() < count) {
        const auto remaining = [&]() { return events.size() < count; };
        const auto quantity = Quantity{2} * (1 + random() % 50);
        if (name == "resting") {
            const bool buy = (random() & 1U) != 0;
            const auto offset = static_cast<Price>(random() % 64);
            add_limit(buy ? Side::Buy : Side::Sell, quantity,
                      buy ? 23'900 - offset : 24'100 + offset);
        } else if (name == "deep") {
            const auto offset = static_cast<Price>(events.size() / 2 + 1);
            const bool buy = events.size() % 2 == 0;
            add_limit(buy ? Side::Buy : Side::Sell, quantity,
                      buy ? 1'000'000'000 - offset : 1'000'000'000 + offset);
        } else if (name == "crossing") {
            add_limit(Side::Sell, quantity, 23'999);
            if (remaining()) add_limit(Side::Sell, quantity, 24'000);
            if (remaining()) add_limit(Side::Sell, quantity, 24'001);
            if (remaining()) add_limit(Side::Buy, 3 * quantity, 24'001);
            if (remaining()) add_limit(Side::Buy, quantity, 23'999);
            if (remaining()) add_market(Side::Sell, quantity);
        } else if (name == "cancellation") {
            const auto buy = add_limit(Side::Buy, quantity, 23'999);
            OrderId sell{};
            if (remaining()) sell = add_limit(Side::Sell, quantity, 24'001);
            if (remaining()) add_cancel(buy);
            if (remaining()) add_cancel(sell);
        } else if (name == "touch") {
            add_limit(Side::Buy, quantity, 23'999);
            if (remaining()) add_limit(Side::Sell, quantity, 24'001);
            if (remaining()) add_limit(Side::Buy, quantity, 24'001);
            if (remaining()) add_market(Side::Sell, quantity);
            OrderId cancel_id{};
            if (remaining()) cancel_id = add_limit(Side::Buy, quantity, 23'999);
            if (remaining()) add_cancel(cancel_id);
        } else {
            const auto center = Price{24'000} + static_cast<Price>(random() % 21) - 10;
            add_limit(Side::Buy, quantity, center - 1);
            if (remaining()) add_limit(Side::Sell, quantity, center + 1);
            OrderId deep_buy{};
            if (remaining()) deep_buy = add_limit(Side::Buy, quantity, center - 2);
            if (remaining()) add_market(Side::Buy, quantity / 2);
            if (remaining()) add_limit(Side::Sell, quantity, center - 1);
            if (remaining()) add_cancel(deep_buy);
            if (remaining()) add_market(Side::Buy, quantity / 2);
            if (remaining()) add_market(Side::Sell, quantity);
            OrderId ask{};
            if (remaining()) ask = add_limit(Side::Sell, quantity, center + 2);
            if (remaining()) add_cancel(ask);
        }
    }
    return events;
}

int run_benchmarks(const BenchmarkOptions& options, std::ostream& output) {
    if (options.events < 64 || options.events > maximum_events || options.trials == 0 ||
        options.trials > 100 || options.warmup > 100 || options.latency_samples == 0 ||
        options.reserve_orders > maximum_events) {
        throw std::invalid_argument("benchmark options outside supported bounds (see --help)");
    }
    if ((options.allocation_probe.start == nullptr) != (options.allocation_probe.stop == nullptr)) {
        throw std::invalid_argument("allocation probe requires both start and stop callbacks");
    }
    if (options.workload != "all" &&
        std::find(workload_names.begin(), workload_names.end(), options.workload) == workload_names.end()) {
        throw std::invalid_argument("unknown benchmark workload: " + options.workload);
    }
    const bool allocations_available = options.allocation_probe.start != nullptr;
    const auto platform = platform_name();
    const auto compiler = compiler_name();
    const auto mode = build_mode();
    const auto samples = std::min(options.events, options.latency_samples);
    const EngineConfig config{options.reserve_orders, std::max(options.events, options.reserve_orders)};
    const auto original_flags = output.flags();
    const auto original_precision = output.precision();
    output << std::fixed << std::setprecision(3);
    if (options.csv) {
        output << "workload,seed,events,trials,warmup,reserve_orders,latency_samples,"
                  "throughput_min_ops_s,throughput_median_ops_s,throughput_max_ops_s,"
                  "throughput_cv_percent,latency_avg_ns,latency_p50_ns,latency_p95_ns,latency_p99_ns,"
                  "accepted,rejected,trades,cancellations,filled_quantity,active_orders,"
                  "replay_checksum,state_checksum,process_peak_rss_kib,allocations_available,"
                  "init_allocation_calls,init_requested_bytes,process_allocation_calls,process_requested_bytes,"
                  "hardware_threads,clock_period_num,clock_period_den,platform,compiler,build\n";
    } else {
        output << "ExchangeCore synthetic engine benchmark\nPlatform: " << platform
               << "\nCompiler: " << compiler << "\nBuild: " << mode
               << "\nHardware threads: " << std::thread::hardware_concurrency()
               << "; steady_clock period: " << Clock::period::num << '/' << Clock::period::den
               << " seconds\nSeed: " << options.seed << "; events/workload: " << options.events
               << "; trials: " << options.trials << "; warmups: " << options.warmup
               << "; reserve: " << options.reserve_orders << "\n"
                  "Throughput excludes generation, construction, validation and formatting; includes result destruction\n"
                  "and a small scalar digest. Latencies use separate sampled per-operation clocks and are instrumented.\n"
                  "RSS is the entire process high-water mark, not per-workload retained memory.\n"
                  "Allocation calls/requested bytes use a separate pass (gross requests, not live bytes).\n";
    }
    bool matched_name = false;
    for (const auto name : workload_names) {
        if (options.workload != "all" && options.workload != name) continue;
        matched_name = true;
        const auto events = make_benchmark_workload(name, options.events, options.seed);
        ReplaySummary expected;
        std::uint64_t expected_state{};
        std::size_t active_orders{};
        {
            MatchingEngine engine(config);
            expected = validate_replay(engine, events);
            validate_activity(name, expected, engine, options.events);
            expected_state = state_checksum(engine);
            active_orders = engine.active_order_count();
        }
        // A second full output hash verifies deterministic replay independently
        // of the scalar reduction used in throughput runs.
        for (std::size_t pass = 0; pass < options.warmup + 1; ++pass) {
            MatchingEngine engine(config);
            const auto actual = validate_replay(engine, events);
            if (actual.checksum != expected.checksum || state_checksum(engine) != expected_state) {
                throw std::runtime_error("nondeterministic benchmark replay");
            }
        }
        std::vector<double> throughput;
        throughput.reserve(options.trials);
        for (std::size_t trial = 0; trial < options.trials; ++trial) {
            MatchingEngine engine(config);
            const auto begin = Clock::now();
            const auto digest = replay_timed_path(engine, events);
            const auto end = Clock::now();
            const auto seconds = std::chrono::duration<double>(end - begin).count();
            if (seconds <= 0 || digest != expected.timing_digest) {
                throw std::runtime_error("invalid throughput duration or replay digest mismatch");
            }
            engine.check_invariants();
            if (state_checksum(engine) != expected_state) {
                throw std::runtime_error("throughput final state mismatch");
            }
            throughput.push_back(static_cast<double>(events.size()) / seconds);
        }
        std::vector<double> latency;
        latency.reserve(samples);
        std::vector<std::size_t> sample_indices;
        sample_indices.reserve(samples);
        std::mt19937_64 sample_random(options.seed ^ 0x4c4154454e4359ULL);
        for (std::size_t sample = 0; sample < samples; ++sample) {
            const auto first = static_cast<std::size_t>(
                static_cast<std::uint64_t>(sample) * events.size() / samples);
            const auto last = static_cast<std::size_t>(
                static_cast<std::uint64_t>(sample + 1) * events.size() / samples);
            // One seeded position per equal-size interval avoids repeatedly
            // sampling the same event type in periodic block workloads.
            sample_indices.push_back(first + static_cast<std::size_t>(sample_random() % (last - first)));
        }
        {
            MatchingEngine engine(config);
            std::size_t sample_number = 0;
            std::uint64_t digest{};
            for (std::size_t i = 0; i < events.size(); ++i) {
                Result result;
                if (sample_number < samples && i == sample_indices[sample_number]) {
                    const auto begin = Clock::now();
                    result = engine.process(events[i]);
                    const auto end = Clock::now();
                    latency.push_back(std::chrono::duration<double, std::nano>(end - begin).count());
                    ++sample_number;
                } else {
                    result = engine.process(events[i]);
                }
                digest += result.filled + result.remaining + result.canceled +
                          result.trades.size() + static_cast<std::uint64_t>(result.accepted);
            }
            engine.check_invariants();
            if (digest != expected.timing_digest || state_checksum(engine) != expected_state ||
                latency.size() != samples) {
                throw std::runtime_error("latency replay mismatch");
            }
        }
        AllocationStats init_allocations;
        AllocationStats process_allocations;
        if (allocations_available) {
            // Stop callbacks must be called even if an allocation fails.
            options.allocation_probe.start();
            try {
                MatchingEngine engine(config);
                init_allocations = options.allocation_probe.stop();
                options.allocation_probe.start();
                const auto digest = replay_timed_path(engine, events);
                process_allocations = options.allocation_probe.stop();
                if (digest != expected.timing_digest || state_checksum(engine) != expected_state) {
                    throw std::runtime_error("allocation replay mismatch");
                }
                engine.check_invariants();
            } catch (...) {
                (void)options.allocation_probe.stop();
                throw;
            }
        }
        std::sort(throughput.begin(), throughput.end());
        std::sort(latency.begin(), latency.end());
        double rate_sum{};
        double latency_sum{};
        for (const auto value : throughput) rate_sum += value;
        for (const auto value : latency) latency_sum += value;
        const auto rate_mean = rate_sum / static_cast<double>(throughput.size());
        double variance{};
        for (const auto value : throughput) variance += (value - rate_mean) * (value - rate_mean);
        const auto cv = 100 * std::sqrt(variance / static_cast<double>(throughput.size())) / rate_mean;
        const auto median_rate = throughput.size() % 2 == 0
            ? (throughput[throughput.size() / 2 - 1] + throughput[throughput.size() / 2]) / 2
            : throughput[throughput.size() / 2];
        const auto average_latency = latency_sum / static_cast<double>(latency.size());
        const auto p50 = percentile(latency, 0.50);
        const auto p95 = percentile(latency, 0.95);
        const auto p99 = percentile(latency, 0.99);
        const auto rss = peak_rss_kib();
        if (options.csv) {
            output << name << ',' << options.seed << ',' << options.events << ',' << options.trials
                   << ',' << options.warmup << ',' << options.reserve_orders << ',' << samples
                   << ',' << throughput.front() << ',' << median_rate << ',' << throughput.back()
                   << ',' << cv << ',' << average_latency << ',' << p50 << ',' << p95 << ',' << p99
                   << ',' << expected.accepted << ',' << expected.rejected << ',' << expected.trades
                   << ',' << expected.canceled << ',' << expected.filled << ',' << active_orders
                   << ',' << expected.checksum << ',' << expected_state << ',' << rss
                   << ',' << static_cast<unsigned>(allocations_available)
                   << ',' << init_allocations.calls << ',' << init_allocations.requested_bytes
                   << ',' << process_allocations.calls << ',' << process_allocations.requested_bytes
                   << ',' << std::thread::hardware_concurrency()
                   << ',' << Clock::period::num << ',' << Clock::period::den << ',';
            csv_string(output, platform);
            output << ',';
            csv_string(output, compiler);
            output << ',';
            csv_string(output, mode);
            output << '\n';
        } else {
            output << "\n" << name << ": ops/s min=" << throughput.front() << " median=" << median_rate
                   << " max=" << throughput.back() << " CV=" << cv << "%\n"
                   << "  sampled latency ns (" << samples << "): avg=" << average_latency
                   << " p50=" << p50 << " p95=" << p95 << " p99=" << p99 << '\n'
                   << "  accepted=" << expected.accepted << " rejected=" << expected.rejected
                   << " trades=" << expected.trades << " cancellations=" << expected.canceled
                   << " filled=" << expected.filled << " active=" << active_orders
                   << " checksum=" << expected.checksum << " state=" << expected_state
                   << " process_peak_rss_kib=" << rss << '\n';
            if (allocations_available) {
                output << "  allocation calls/requested bytes: init=" << init_allocations.calls
                       << '/' << init_allocations.requested_bytes << " processing="
                       << process_allocations.calls << '/' << process_allocations.requested_bytes << '\n';
            } else {
                output << "  allocation instrumentation unavailable (use exchangecore_bench)\n";
            }
        }
    }
    output.flags(original_flags);
    output.precision(original_precision);
    if (!matched_name) {
        throw std::invalid_argument("unknown benchmark workload: " + options.workload);
    }
    return 0;
}

int run_benchmark_cli(int argc, const char* const* argv, std::ostream& output,
                      std::ostream& errors, AllocationProbe probe) {
    try {
        BenchmarkOptions options;
        options.allocation_probe = probe;
        for (int i = 0; i < argc; ++i) {
            const std::string_view argument(argv[i]);
            if (argument == "--help" || argument == "-h") {
                usage(output);
                return 0;
            }
            if (argument == "--csv") {
                options.csv = true;
                continue;
            }
            if (i + 1 >= argc) {
                throw std::invalid_argument("missing value for " + std::string(argument));
            }
            const std::string_view value(argv[++i]);
            if (argument == "--events") options.events = parse_size(value);
            else if (argument == "--trials") options.trials = parse_size(value);
            else if (argument == "--warmup") options.warmup = parse_size(value);
            else if (argument == "--latency-samples" || argument == "--samples") {
                options.latency_samples = parse_size(value);
            }
            else if (argument == "--seed") options.seed = parse_unsigned(value);
            else if (argument == "--reserve") options.reserve_orders = parse_size(value);
            else if (argument == "--workload") options.workload = value;
            else throw std::invalid_argument("unknown benchmark option: " + std::string(argument));
        }
        return run_benchmarks(options, output);
    } catch (const std::invalid_argument& error) {
        errors << "benchmark: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        errors << "benchmark: " << error.what() << '\n';
        return 1;
    }
}

}  // namespace exchangecore
