#include "exchangecore/matching_engine.hpp"
#include "exchangecore/protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace exchangecore;
using namespace exchangecore::protocol;

struct TestCase { std::string_view name; void (*run)(); };
std::vector<TestCase>& cases() { static std::vector<TestCase> result; return result; }
struct Register { Register(std::string_view name, void (*run)()) { cases().push_back({name, run}); } };
void check(bool condition, const char* expression, int line) {
    if (!condition) throw std::runtime_error("line " + std::to_string(line) + ": " + expression);
}
#define JOIN_IMPL(a, b) a##b
#define JOIN(a, b) JOIN_IMPL(a, b)
#define TEST(name) \
    void JOIN(test_, __LINE__)(); \
    const Register JOIN(register_, __LINE__)(name, JOIN(test_, __LINE__)); \
    void JOIN(test_, __LINE__)()
#define CHECK(...) check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

Request parsed(std::string_view line) {
    const auto result = parse_request(line);
    CHECK(result);
    return result.request;
}
void rejected(std::string_view line, ParseError error, std::uint64_t request_id = 0) {
    const auto result = parse_request(line);
    CHECK(!result);
    CHECK(result.error == error);
    CHECK(result.request.request_id == request_id);
}
void bounded(std::string_view line) {
    CHECK(!line.empty());
    CHECK(line.back() == '\n');
    CHECK(line.size() < max_response_line_bytes);
    CHECK(line.substr(0, line.size() - 1).find_first_of("\r\n") == std::string_view::npos);
}

TEST("protocol parses buy and sell limits into exact engine events") {
    CHECK(parsed("EC1 7 LIMIT 91 BUY 5 24001") == Request{7, Command::Limit, Event::limit(91, Side::Buy, 5, 24001)});
    CHECK(parsed("EC1 8 LIMIT 92 SELL 6 24002") == Request{8, Command::Limit, Event::limit(92, Side::Sell, 6, 24002)});
}

TEST("protocol parses buy and sell markets without introducing a price") {
    CHECK(parsed("EC1 7 MARKET 91 BUY 5") == Request{7, Command::Market, Event::market(91, Side::Buy, 5)});
    CHECK(parsed("EC1 8 MARKET 92 SELL 6") == Request{8, Command::Market, Event::market(92, Side::Sell, 6)});
}

TEST("protocol cancellation preserves both correlation and order IDs") {
    CHECK(parsed("EC1 17 CANCEL 9") == Request{17, Command::Cancel, Event::cancel(9)});
}

TEST("protocol best and ping require no order payload") {
    const auto best = parsed("EC1 18 BEST"), ping = parsed("EC1 19 PING");
    CHECK(best.request_id == 18); CHECK(best.command == Command::Best);
    CHECK(ping.request_id == 19); CHECK(ping.command == Command::Ping);
}

TEST("protocol allows surrounding spaces, tabs and one terminal CR") {
    CHECK(parsed(" \t EC1\t7  LIMIT\t91\tBUY  5\t24001 \t\r") ==
          Request{7, Command::Limit, Event::limit(91, Side::Buy, 5, 24001)});
    CHECK(parsed("EC1 1 PING\r").command == Command::Ping);
}

TEST("protocol integer parsing preserves full uint64 and positive int64 bounds") {
    const auto request = parsed("EC1 18446744073709551615 LIMIT 18446744073709551615 SELL 18446744073709551615 9223372036854775807");
    CHECK(request.request_id == std::numeric_limits<std::uint64_t>::max());
    CHECK(request.event == Event::limit(std::numeric_limits<OrderId>::max(), Side::Sell,
          std::numeric_limits<Quantity>::max(), std::numeric_limits<Price>::max()));
}

TEST("protocol accepts positive integer tokens with leading zeroes") {
    CHECK(parsed("EC1 0007 LIMIT 0091 BUY 0005 024001") ==
          Request{7, Command::Limit, Event::limit(91, Side::Buy, 5, 24001)});
}

TEST("protocol rejects absent and unsupported versions while retaining usable correlation") {
    rejected("", ParseError::InvalidVersion);
    rejected(" \t\r", ParseError::InvalidVersion);
    rejected("EC2 7 PING", ParseError::InvalidVersion, 7);
    rejected("ec1 7 PING", ParseError::InvalidVersion, 7);
    rejected("PING", ParseError::InvalidVersion);
}

TEST("protocol rejects missing, zero, signed, decimal, junk and overflowing request IDs") {
    rejected("EC1", ParseError::InvalidRequestId);
    for (const auto id : {"0", "-1", "+1", "1.0", "1x", "18446744073709551616"}) {
        rejected(std::string("EC1 ") + id + " PING", ParseError::InvalidRequestId);
    }
}

TEST("protocol rejects unknown and wrong-case commands") {
    for (const auto command : {"NEW", "limit", "market", "Cancel", "best", "ping", "LIMITX"}) {
        rejected(std::string("EC1 9 ") + command, ParseError::UnknownCommand, 9);
    }
}

TEST("protocol requires exact field counts for every command") {
    constexpr std::array<std::string_view, 13> invalid{
        "EC1 7", "EC1 7 LIMIT", "EC1 7 LIMIT 1 BUY 2", "EC1 7 LIMIT 1 BUY 2 100 extra",
        "EC1 7 MARKET", "EC1 7 MARKET 1 BUY", "EC1 7 MARKET 1 BUY 2 0",
        "EC1 7 CANCEL", "EC1 7 CANCEL 1 extra", "EC1 7 BEST extra", "EC1 7 PING extra",
        "EC1 7 LIMIT 1 BUY 2 100 a b c d e f g", "EC1 7 MARKET 1 BUY 2 extra extra"};
    for (const auto line : invalid) rejected(line, ParseError::FieldCount, 7);
}

TEST("protocol rejects invalid order IDs in every order command") {
    for (const auto id : {"0", "-1", "+1", "1x", "1.5", "18446744073709551616"}) {
        rejected(std::string("EC1 9 LIMIT ") + id + " BUY 1 100", ParseError::InvalidOrderId, 9);
        rejected(std::string("EC1 9 MARKET ") + id + " BUY 1", ParseError::InvalidOrderId, 9);
        rejected(std::string("EC1 9 CANCEL ") + id, ParseError::InvalidOrderId, 9);
    }
}

TEST("protocol rejects invalid and wrong-case sides") {
    for (const auto side : {"buy", "sell", "BID", "ASK", "0", "B", "SELLX"}) {
        rejected(std::string("EC1 11 LIMIT 1 ") + side + " 1 100", ParseError::InvalidSide, 11);
        rejected(std::string("EC1 11 MARKET 1 ") + side + " 1", ParseError::InvalidSide, 11);
    }
}

TEST("protocol rejects invalid quantities without silent numeric truncation") {
    for (const auto quantity : {"0", "-1", "+1", "1.25", "1x", "1e3", "18446744073709551616"}) {
        rejected(std::string("EC1 12 LIMIT 1 BUY ") + quantity + " 100", ParseError::InvalidQuantity, 12);
        rejected(std::string("EC1 12 MARKET 1 SELL ") + quantity, ParseError::InvalidQuantity, 12);
    }
}

TEST("protocol prices are positive exact int64 ticks with checked overflow") {
    for (const auto price : {"0", "-1", "+100", "6000.25", "1e3", "100x", "9223372036854775808", "18446744073709551615"}) {
        rejected(std::string("EC1 13 LIMIT 1 BUY 2 ") + price, ParseError::InvalidPrice, 13);
    }
}

TEST("protocol rejects embedded NUL, ASCII controls and DEL") {
    for (unsigned value = 0; value <= 127; ++value) {
        if (value >= 32 && value < 127) continue;
        if (value == static_cast<unsigned>('\t')) continue;
        std::string line = "EC1 14 PI";
        line.push_back(static_cast<char>(value)); line += "NG";
        rejected(line, ParseError::InvalidCharacter, 14);
    }
}

TEST("protocol rejects non-ASCII bytes and UTF-8 payloads") {
    for (unsigned value = 128; value <= 255; ++value) {
        std::string line = "EC1 15 PI";
        line.push_back(static_cast<char>(value)); line += "NG";
        rejected(line, ParseError::InvalidCharacter, 15);
    }
    rejected("EC1 15 P\xc3\x8dNG", ParseError::InvalidCharacter, 15);
}

TEST("protocol parser receives one frame, never treats embedded LF as whitespace") {
    rejected("EC1 16 PING\nEC1 17 BEST", ParseError::InvalidCharacter, 16);
    rejected("EC1 16 PING\n", ParseError::InvalidCharacter, 16);
    rejected("EC1 16 PING\r\r", ParseError::InvalidCharacter, 16);
    rejected("EC1 16 PI\rNG", ParseError::InvalidCharacter, 16);
}

TEST("protocol accepts the exact maximum frame size including optional CR") {
    std::string line = "EC1 17 PING";
    line.resize(max_frame_bytes, ' ');
    CHECK(parsed(line).request_id == 17);
    line.back() = '\r';
    CHECK(parsed(line).command == Command::Ping);
}

TEST("protocol rejects overlong frames before parsing with zero correlation fallback") {
    std::string line = "EC1 17 PING";
    line.resize(max_frame_bytes + 1, ' ');
    rejected(line, ParseError::TooLarge);
}

TEST("protocol preserves usable request IDs on syntax errors and uses zero otherwise") {
    rejected("EC1 88 CANCEL 0", ParseError::InvalidOrderId, 88);
    rejected("EC1 bad CANCEL 1", ParseError::InvalidRequestId);
    rejected("EC1 88 UNKNOWN", ParseError::UnknownCommand, 88);
    std::string line = "EC1 88 PING"; line.push_back('\0');
    rejected(line, ParseError::InvalidCharacter, 88);
}

TEST("protocol parser leaves per-connection request ordering to the I/O owner") {
    CHECK(parsed("EC1 10 PING").request_id == 10);
    CHECK(parsed("EC1 10 PING").request_id == 10);
    CHECK(parsed("EC1 1 PING").request_id == 1);
}

TEST("protocol formatter emits accepted result fields without formatting trades") {
    const Result result{true, RejectReason::None, 91, 5, 2, 0, {{1, 91, 100, 2}, {2, 91, 101, 3}}};
    const auto line = format_result(7, 12, result);
    CHECK(line == "EC1 7 12 OK 91 5 2 0 2\n"); bounded(line);
}

TEST("protocol formatter distinguishes successful cancellation quantities") {
    const Result result{true, RejectReason::None, 91, 0, 0, 12, {}};
    CHECK(format_result(7, 12, result) == "EC1 7 12 OK 91 0 0 12 0\n");
}

TEST("protocol formatter covers every matching rejection reason") {
    constexpr std::array<std::pair<RejectReason, std::string_view>, 13> reasons{{
        {RejectReason::None, "NONE"}, {RejectReason::InvalidId, "INVALID_ID"},
        {RejectReason::InvalidSide, "INVALID_SIDE"}, {RejectReason::InvalidType, "INVALID_TYPE"},
        {RejectReason::InvalidQuantity, "INVALID_QUANTITY"}, {RejectReason::InvalidPrice, "INVALID_PRICE"},
        {RejectReason::DuplicateId, "DUPLICATE_ID"}, {RejectReason::UnknownOrder, "UNKNOWN_ORDER"},
        {RejectReason::CapacityExceeded, "CAPACITY_EXCEEDED"}, {RejectReason::QuantityOverflow, "QUANTITY_OVERFLOW"},
        {RejectReason::SequenceExhausted, "SEQUENCE_EXHAUSTED"}, {RejectReason::InvalidEvent, "INVALID_EVENT"},
        {static_cast<RejectReason>(255), "INVALID_REJECTION"}}};
    for (const auto& [reason, token] : reasons) {
        CHECK(to_string(reason) == token);
        const auto line = format_result(7, 12, Result{false, reason, 91, 0, 5, 0, {}});
        CHECK(line == std::string("EC1 7 12 REJECT ") + std::string(token) + " 91 5\n");
        bounded(line);
    }
}

TEST("protocol formatter emits one exact trade with maker and taker correlation") {
    const auto line = format_trade(7, 12, Trade{3, 91, 24001, 5});
    CHECK(line == "EC1 7 12 TRADE 3 91 24001 5\n"); bounded(line);
}

TEST("protocol best query formats all empty-side combinations and quantities") {
    CHECK(format_best(7, 12, {}, {}, 0, 0) == "EC1 7 12 BEST NONE NONE 0 0\n");
    CHECK(format_best(7, 12, 99, {}, 2, 7) == "EC1 7 12 BEST 99 NONE 2 7\n");
    CHECK(format_best(7, 12, {}, 101, 3, 8) == "EC1 7 12 BEST NONE 101 3 8\n");
    CHECK(format_best(7, 12, 99, 101, 5, 15) == "EC1 7 12 BEST 99 101 5 15\n");
}

TEST("protocol pong and END preserve both IDs including zero-sequence transport errors") {
    CHECK(format_pong(7, 12) == "EC1 7 12 PONG\n");
    CHECK(format_end(7, 12) == "EC1 7 12 END\n");
    CHECK(format_error(0, 0, "QUEUE_FULL") == "EC1 0 0 ERROR QUEUE_FULL\n");
    CHECK(format_end(0, 0) == "EC1 0 0 END\n");
}

TEST("protocol syntax-error formatter covers every parse failure") {
    constexpr std::array<std::pair<ParseError, std::string_view>, 12> errors{{
        {ParseError::None, "NONE"}, {ParseError::TooLarge, "LINE_TOO_LARGE"},
        {ParseError::InvalidCharacter, "INVALID_CHARACTER"}, {ParseError::InvalidVersion, "INVALID_VERSION"},
        {ParseError::InvalidRequestId, "INVALID_REQUEST_ID"}, {ParseError::UnknownCommand, "UNKNOWN_COMMAND"},
        {ParseError::FieldCount, "FIELD_COUNT"}, {ParseError::InvalidOrderId, "INVALID_ORDER_ID"},
        {ParseError::InvalidSide, "INVALID_SIDE"}, {ParseError::InvalidQuantity, "INVALID_QUANTITY"},
        {ParseError::InvalidPrice, "INVALID_PRICE"}, {static_cast<ParseError>(255), "INVALID_ERROR"}}};
    for (const auto& [error, token] : errors) {
        CHECK(to_string(error) == token);
        const auto line = format_error(7, 0, error);
        CHECK(line == std::string("EC1 7 0 ERROR ") + std::string(token) + '\n');
        bounded(line);
    }
}

TEST("protocol error codes cannot inject tokens or lines") {
    std::vector<std::string> invalid{"", "bad", "BAD CODE", "BAD\nEND", "BAD\r", "BAD\t", std::string(65, 'A')};
    invalid.emplace_back("BAD\0CODE", 8);
    for (const auto& code : invalid) {
        bool caught = false;
        try { (void)format_error(1, 0, code); } catch (const std::invalid_argument&) { caught = true; }
        CHECK(caught);
    }
    const auto line = format_error(1, 0, std::string(64, 'A'));
    bounded(line);
}

TEST("protocol numeric formatting remains bounded at all integer extremes") {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    const Result result{true, RejectReason::None, maximum, maximum, maximum, maximum, {}};
    std::vector<std::string> lines{
        format_result(maximum, maximum, result),
        format_trade(maximum, maximum, {maximum, maximum, std::numeric_limits<Price>::min(), maximum}),
        format_best(maximum, maximum, std::numeric_limits<Price>::max(), std::numeric_limits<Price>::min(),
                    std::numeric_limits<std::size_t>::max(), maximum),
        format_error(maximum, maximum, std::string(64, 'A')), format_pong(maximum, maximum), format_end(maximum, maximum)};
    for (const auto& line : lines) {
        bounded(line);
        CHECK(line.starts_with("EC1 18446744073709551615 18446744073709551615 "));
    }
    CHECK(lines[1].find("-9223372036854775808") != std::string::npos);
}

TEST("protocol result header size is independent of the number of trades") {
    Result result{true, RejectReason::None, 91, 10000, 0, 0, {}};
    result.trades.resize(10000);
    const auto line = format_result(7, 12, result);
    CHECK(line == "EC1 7 12 OK 91 10000 0 0 10000\n"); bounded(line);
}

TEST("protocol engine response streams ordered trades and a matching END") {
    MatchingEngine engine;
    CHECK(engine.submit_limit(1, Side::Sell, 2, 100).accepted);
    CHECK(engine.submit_limit(2, Side::Sell, 3, 101).accepted);
    CHECK(engine.submit_limit(3, Side::Sell, 4, 102).accepted);
    const auto request = parsed("EC1 19 LIMIT 4 BUY 7 102");
    const auto result = engine.process(request.event);
    CHECK(format_result(request.request_id, 5, result) == "EC1 19 5 OK 4 7 0 0 3\n");
    CHECK(format_trade(19, 5, result.trades[0]) == "EC1 19 5 TRADE 1 4 100 2\n");
    CHECK(format_trade(19, 5, result.trades[1]) == "EC1 19 5 TRADE 2 4 101 3\n");
    CHECK(format_trade(19, 5, result.trades[2]) == "EC1 19 5 TRADE 3 4 102 2\n");
    CHECK(format_end(19, 5) == "EC1 19 5 END\n");
    CHECK(format_best(20, 6, engine.best_bid(), engine.best_ask(), engine.active_order_count(), engine.resting_quantity()) ==
          "EC1 20 6 BEST NONE 102 1 2\n");
    engine.check_invariants();
}

} // namespace

int main() {
    std::size_t passed = 0, failed = 0;
    for (const auto& test : cases()) {
        try { test.run(); ++passed; std::cout << "[PASS] " << test.name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n'; }
    }
    std::cout << passed << " protocol tests passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
