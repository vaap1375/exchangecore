#include "exchangecore/csv.hpp"

#include <array>
#include <charconv>
#include <limits>
#include <optional>
#include <string_view>

namespace exchangecore {
namespace {
constexpr std::size_t max_line = 4096;
constexpr std::size_t max_events = 1'000'000;
constexpr std::size_t max_errors = 100;

std::string_view trim(std::string_view s) {
    constexpr std::string_view space = " \t\r\n";
    const auto first = s.find_first_not_of(space);
    if (first == std::string_view::npos) return {};
    return s.substr(first, s.find_last_not_of(space) - first + 1);
}

template<class T> bool positive(std::string_view s, T& value) {
    if (s.empty()) return false;
    const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), value);
    return error == std::errc{} && end == s.data() + s.size() && value > 0;
}

std::optional<std::array<std::string_view, 5>> fields(std::string_view s) {
    std::array<std::string_view, 5> parts{};
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto comma = s.find(',');
        if (i == 4) {
            if (comma != std::string_view::npos) return std::nullopt;
            parts[i] = trim(s);
        } else {
            if (comma == std::string_view::npos) return std::nullopt;
            parts[i] = trim(s.substr(0, comma));
            s.remove_prefix(comma + 1);
        }
    }
    return parts;
}
} // namespace

ParseResult parse_csv(std::istream& input) {
    ParseResult result;
    std::size_t line_number = 0;
    std::optional<std::size_t> read_failure_line;
    bool header = false;
    std::string line;
    line.reserve(max_line);
    // Read bounded lines even for adversarial inputs. Discard excess characters.
    while (true) {
        line.clear();
        bool any = false;
        bool too_long = false;
        char ch = 0;
        while (input.get(ch)) {
            any = true;
            if (ch == '\n') break;
            if (line.size() < max_line) line.push_back(ch);
            else too_long = true;
        }
        if ((input.bad() || (input.fail() && !input.eof())) && !read_failure_line)
            read_failure_line = line_number + 1;
        if (!any) break;
        ++line_number;
        auto error = [&](std::string message) {
            result.errors.push_back({line_number, std::move(message)});
        };
        if (too_long) {
            error("line exceeds 4096 bytes");
        } else {
            const auto text = trim(line);
            if (text.empty() || text.front() == '#') continue;
            const auto split = fields(text);
            if (!split) {
                error("expected exactly 5 unquoted CSV fields");
            } else if (!header) {
                const std::array<std::string_view, 5> expected{"type", "id", "side", "quantity", "price"};
                if (*split != expected) {
                    error("expected header: type,id,side,quantity,price");
                    break;
                }
                header = true;
            } else if (result.events.size() == max_events) {
                error("event limit exceeded (1000000)");
                break;
            } else {
                const auto& f = *split;
                OrderId id{};
                Quantity qty{};
                Price price{};
                if (!positive(f[1], id)) {
                    error("id must be a positive uint64 integer");
                } else if (f[0] == "CANCEL") {
                    if (!f[2].empty() || !f[3].empty() || !f[4].empty())
                        error("CANCEL requires empty side, quantity and price");
                    else result.events.push_back({line_number, Event::cancel(id)});
                } else if (f[0] != "LIMIT" && f[0] != "MARKET") {
                    error("unknown event type (expected LIMIT, MARKET or CANCEL)");
                } else if (f[2] != "BUY" && f[2] != "SELL") {
                    error("side must be BUY or SELL");
                } else if (!positive(f[3], qty)) {
                    error("quantity must be a positive uint64 integer");
                } else if (f[0] == "LIMIT" && !positive(f[4], price)) {
                    error("limit price must be positive int64 integer ticks");
                } else if (f[0] == "MARKET" && !f[4].empty()) {
                    error("MARKET requires an empty price");
                } else {
                    const Side side = f[2] == "BUY" ? Side::Buy : Side::Sell;
                    result.events.push_back({line_number, f[0] == "LIMIT"
                        ? Event::limit(id, side, qty, price) : Event::market(id, side, qty)});
                }
            }
        }
        if (result.errors.size() >= max_errors) break;
    }
    if (read_failure_line && result.errors.size() < max_errors)
        result.errors.push_back({*read_failure_line, "input read failure"});
    return result;
}
} // namespace exchangecore
