#pragma once

#include "exchangecore/event.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

namespace exchangecore {

struct AllocationStats {
    std::uint64_t calls{};
    std::uint64_t requested_bytes{};
};

// Optional executable-owned allocation instrumentation. The library never
// replaces global allocation functions. Measurements use a separate replay.
struct AllocationProbe {
    void (*start)() noexcept{};
    AllocationStats (*stop)() noexcept{};
};

struct BenchmarkOptions {
    std::size_t events{100'000};
    std::size_t trials{5};
    std::size_t warmup{1};
    std::size_t latency_samples{10'000};
    std::uint64_t seed{60'448};
    std::size_t reserve_orders{0};
    std::string workload{"all"};
    bool csv{false};
    AllocationProbe allocation_probe{};
};

// Workload construction is independent of engine timing and portable across
// standard libraries: raw mt19937_64 outputs use explicit modulo reduction.
[[nodiscard]] std::vector<Event> make_benchmark_workload(
    std::string_view name, std::size_t events, std::uint64_t seed);

// Returns zero on success. Invalid options or a replay/invariant disagreement
// throw an exception; run_benchmark_cli translates these to a diagnostic.
int run_benchmarks(const BenchmarkOptions& options, std::ostream& output);

// argv contains only options (no executable or "benchmark" command token).
int run_benchmark_cli(int argc, const char* const* argv, std::ostream& output,
                      std::ostream& errors, AllocationProbe probe = {});

}  // namespace exchangecore
