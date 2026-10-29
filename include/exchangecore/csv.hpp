#pragma once

#include "exchangecore/event.hpp"
#include <cstddef>
#include <istream>
#include <string>
#include <vector>

namespace exchangecore {
struct ParseError {
    std::size_t line;
    std::string message;
};
struct ParsedEvent {
    std::size_t line;
    Event event;
};
struct ParseResult {
    std::vector<ParsedEvent> events;
    std::vector<ParseError> errors;
};
// Strict unquoted CSV. Empty/comment-only streams contain zero events.
// Bounded to 4096 bytes/line, 1M events, 100 diagnostics.
[[nodiscard]] ParseResult parse_csv(std::istream& input);
} // namespace exchangecore
