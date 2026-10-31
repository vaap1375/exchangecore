#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>

namespace exchangecore {

struct GatewayConfig {
    std::string host{"127.0.0.1"};
    std::uint16_t port{9000};
    std::size_t max_clients{128};
    std::size_t queue_capacity{1024};
    std::size_t completion_capacity{1024};
    std::size_t pending_per_client{16};
    std::size_t max_input{4096};
    std::size_t max_output{65'536};
    std::size_t max_orders{1'000'000};
    std::chrono::milliseconds shutdown_grace{2000};
    int send_buffer{0};
    std::size_t write_chunk{65'536};
};

struct GatewayStats {
    std::uint64_t accepted{};
    std::uint64_t processed{};
    std::uint64_t overloaded{};
    std::uint64_t connections{};
    std::uint64_t closed{};
    std::uint64_t discarded{};
    std::uint64_t parse_errors{};
    std::uint64_t write_calls{};
    std::uint64_t partial_writes{};
    std::uint64_t short_writes{};
    std::uint64_t would_block{};
    std::uint64_t setup_failures{};
    std::uint64_t startup_rss_kib{};
    std::uint64_t peak_rss_kib{};
    std::size_t queue_high_water{};
    std::size_t completion_high_water{};
    std::size_t output_high_water{};
    std::size_t input_high_water{};
    std::size_t pending_high_water{};
};

// Blocking IO owner; internally starts one exclusive matching writer. Emits a
// flushed "LISTEN host actual_port" line after startup. A signal-safe stop flag
// closes admission and drains every queued request before bounded response grace.
// IPv4 POSIX implementation. Throws on configuration/startup/runtime failures.
[[nodiscard]] GatewayStats run_gateway(const GatewayConfig& config, std::ostream& status,
                                      const std::atomic<bool>& stop_requested);

}  // namespace exchangecore
