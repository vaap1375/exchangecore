#pragma once

#include "exchangecore/event.hpp"
#include "exchangecore/trade.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace exchangecore::protocol {

// Frame length excludes its terminating LF and includes an optional final CR.
inline constexpr std::size_t max_frame_bytes = 4096;
// Every formatter returns one line, including LF, shorter than this bound.
inline constexpr std::size_t max_response_line_bytes = 256;

enum class Command : std::uint8_t { Limit, Market, Cancel, Best, Ping };

struct Request {
    std::uint64_t request_id{};
    Command command{Command::Ping};
    Event event{};

    bool operator==(const Request&) const = default;
};

enum class ParseError : std::uint8_t {
    None,
    TooLarge,
    InvalidCharacter,
    InvalidVersion,
    InvalidRequestId,
    UnknownCommand,
    FieldCount,
    InvalidOrderId,
    InvalidSide,
    InvalidQuantity,
    InvalidPrice
};

struct ParseResult {
    Request request{};
    ParseError error{ParseError::None};

    [[nodiscard]] explicit operator bool() const noexcept { return error == ParseError::None; }
};

// Allocation-free parser for a single frame excluding LF. A terminal CR is
// accepted; all other control/non-ASCII bytes reject. Space and tab delimit
// fields. A valid second token is retained for correlation on malformed input;
// otherwise request_id is zero. The I/O owner enforces monotonic request IDs.
[[nodiscard]] ParseResult parse_request(std::string_view line) noexcept;
[[nodiscard]] std::string_view to_string(ParseError error) noexcept;
[[nodiscard]] std::string_view to_string(RejectReason reason) noexcept;

// Each function produces exactly one bounded line ending in LF. Header/trades
// and END are separate so a large result can be streamed with bounded buffers.
[[nodiscard]] std::string format_result(std::uint64_t request_id, std::uint64_t sequence,
                                       const Result& result);
[[nodiscard]] std::string format_trade(std::uint64_t request_id, std::uint64_t sequence,
                                      const Trade& trade);
[[nodiscard]] std::string format_best(std::uint64_t request_id, std::uint64_t sequence,
                                     std::optional<Price> bid, std::optional<Price> ask,
                                     std::size_t active_count, Quantity total_quantity);
[[nodiscard]] std::string format_pong(std::uint64_t request_id, std::uint64_t sequence);
[[nodiscard]] std::string format_error(std::uint64_t request_id, std::uint64_t sequence,
                                      ParseError error);
// Codes are fixed protocol tokens: 1..64 uppercase ASCII letters/digits/_ .
// Invalid codes throw invalid_argument; user text must never be interpolated.
[[nodiscard]] std::string format_error(std::uint64_t request_id, std::uint64_t sequence,
                                      std::string_view code);
[[nodiscard]] std::string format_end(std::uint64_t request_id, std::uint64_t sequence);

} // namespace exchangecore::protocol
