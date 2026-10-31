#include "exchangecore/protocol.hpp"

#include <array>
#include <charconv>
#include <stdexcept>
#include <system_error>

namespace exchangecore::protocol {
namespace {

constexpr bool separator(char value) noexcept { return value == ' ' || value == '\t'; }

template<class Integer>
bool positive(std::string_view token, Integer& result) noexcept {
    if (token.empty()) return false;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size() && result > 0;
}

template<class Integer>
void number(std::string& output, Integer value) {
    std::array<char, 32> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    output.append(buffer.data(), converted.ptr);
}

std::string prefix(std::uint64_t request_id, std::uint64_t sequence) {
    std::string output;
    output.reserve(max_response_line_bytes);
    output += "EC1 ";
    number(output, request_id);
    output += ' ';
    number(output, sequence);
    output += ' ';
    return output;
}

template<class Integer>
void field(std::string& output, Integer value) {
    output += ' ';
    number(output, value);
}

} // namespace

ParseResult parse_request(std::string_view line) noexcept {
    ParseResult result;
    if (line.size() > max_frame_bytes) {
        result.error = ParseError::TooLarge;
        return result;
    }
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    std::array<std::string_view, 7> tokens{};
    std::size_t token_count = 0;
    bool invalid_character = false;
    for (const char character : line) {
        const auto value = static_cast<unsigned char>(character);
        if (value != '\t' && (value < 32 || value > 126)) invalid_character = true;
    }
    for (std::size_t position = 0; position < line.size();) {
        if (separator(line[position])) { ++position; continue; }
        const auto first = position;
        while (position < line.size() && !separator(line[position])) ++position;
        if (token_count < tokens.size()) tokens[token_count] = line.substr(first, position - first);
        ++token_count;
    }
    std::uint64_t request_id = 0;
    if (token_count >= 2 && positive(tokens[1], request_id)) result.request.request_id = request_id;
    const auto fail = [&](ParseError error) {
        result.error = error;
        return result;
    };
    if (invalid_character) return fail(ParseError::InvalidCharacter);
    if (token_count == 0 || tokens[0] != "EC1") return fail(ParseError::InvalidVersion);
    if (result.request.request_id == 0) return fail(ParseError::InvalidRequestId);
    if (token_count < 3) return fail(ParseError::FieldCount);

    std::size_t expected = 0;
    if (tokens[2] == "LIMIT") { result.request.command = Command::Limit; expected = 7; }
    else if (tokens[2] == "MARKET") { result.request.command = Command::Market; expected = 6; }
    else if (tokens[2] == "CANCEL") { result.request.command = Command::Cancel; expected = 4; }
    else if (tokens[2] == "BEST") { result.request.command = Command::Best; expected = 3; }
    else if (tokens[2] == "PING") { result.request.command = Command::Ping; expected = 3; }
    else return fail(ParseError::UnknownCommand);
    if (token_count != expected) return fail(ParseError::FieldCount);
    if (result.request.command == Command::Best || result.request.command == Command::Ping) return result;

    OrderId id = 0;
    if (!positive(tokens[3], id)) return fail(ParseError::InvalidOrderId);
    if (result.request.command == Command::Cancel) {
        result.request.event = Event::cancel(id);
        return result;
    }
    Side side{};
    if (tokens[4] == "BUY") side = Side::Buy;
    else if (tokens[4] == "SELL") side = Side::Sell;
    else return fail(ParseError::InvalidSide);
    Quantity quantity = 0;
    if (!positive(tokens[5], quantity)) return fail(ParseError::InvalidQuantity);
    if (result.request.command == Command::Market) {
        result.request.event = Event::market(id, side, quantity);
    } else {
        Price price = 0;
        if (!positive(tokens[6], price)) return fail(ParseError::InvalidPrice);
        result.request.event = Event::limit(id, side, quantity, price);
    }
    return result;
}

std::string_view to_string(ParseError error) noexcept {
    switch (error) {
        case ParseError::None: return "NONE";
        case ParseError::TooLarge: return "LINE_TOO_LARGE";
        case ParseError::InvalidCharacter: return "INVALID_CHARACTER";
        case ParseError::InvalidVersion: return "INVALID_VERSION";
        case ParseError::InvalidRequestId: return "INVALID_REQUEST_ID";
        case ParseError::UnknownCommand: return "UNKNOWN_COMMAND";
        case ParseError::FieldCount: return "FIELD_COUNT";
        case ParseError::InvalidOrderId: return "INVALID_ORDER_ID";
        case ParseError::InvalidSide: return "INVALID_SIDE";
        case ParseError::InvalidQuantity: return "INVALID_QUANTITY";
        case ParseError::InvalidPrice: return "INVALID_PRICE";
    }
    return "INVALID_ERROR";
}

std::string_view to_string(RejectReason reason) noexcept {
    switch (reason) {
        case RejectReason::None: return "NONE";
        case RejectReason::InvalidId: return "INVALID_ID";
        case RejectReason::InvalidSide: return "INVALID_SIDE";
        case RejectReason::InvalidType: return "INVALID_TYPE";
        case RejectReason::InvalidQuantity: return "INVALID_QUANTITY";
        case RejectReason::InvalidPrice: return "INVALID_PRICE";
        case RejectReason::DuplicateId: return "DUPLICATE_ID";
        case RejectReason::UnknownOrder: return "UNKNOWN_ORDER";
        case RejectReason::CapacityExceeded: return "CAPACITY_EXCEEDED";
        case RejectReason::QuantityOverflow: return "QUANTITY_OVERFLOW";
        case RejectReason::SequenceExhausted: return "SEQUENCE_EXHAUSTED";
        case RejectReason::InvalidEvent: return "INVALID_EVENT";
    }
    return "INVALID_REJECTION";
}

std::string format_result(std::uint64_t request_id, std::uint64_t sequence, const Result& result) {
    auto output = prefix(request_id, sequence);
    if (result.accepted) {
        output += "OK";
        field(output, result.order_id);
        field(output, result.filled);
        field(output, result.remaining);
        field(output, result.canceled);
        field(output, result.trades.size());
    } else {
        output += "REJECT ";
        output += to_string(result.reason);
        field(output, result.order_id);
        field(output, result.remaining);
    }
    output += '\n';
    return output;
}

std::string format_trade(std::uint64_t request_id, std::uint64_t sequence, const Trade& trade) {
    auto output = prefix(request_id, sequence);
    output += "TRADE";
    field(output, trade.maker_id);
    field(output, trade.taker_id);
    field(output, trade.price);
    field(output, trade.quantity);
    output += '\n';
    return output;
}

std::string format_best(std::uint64_t request_id, std::uint64_t sequence,
                        std::optional<Price> bid, std::optional<Price> ask,
                        std::size_t active_count, Quantity total_quantity) {
    auto output = prefix(request_id, sequence);
    output += "BEST ";
    if (bid) number(output, *bid); else output += "NONE";
    output += ' ';
    if (ask) number(output, *ask); else output += "NONE";
    field(output, active_count);
    field(output, total_quantity);
    output += '\n';
    return output;
}

std::string format_pong(std::uint64_t request_id, std::uint64_t sequence) {
    auto output = prefix(request_id, sequence);
    output += "PONG\n";
    return output;
}

std::string format_error(std::uint64_t request_id, std::uint64_t sequence, ParseError error) {
    return format_error(request_id, sequence, to_string(error));
}

std::string format_error(std::uint64_t request_id, std::uint64_t sequence, std::string_view code) {
    if (code.empty() || code.size() > 64) throw std::invalid_argument("invalid protocol error code length");
    for (const char value : code) {
        if (!((value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') || value == '_')) {
            throw std::invalid_argument("invalid protocol error code character");
        }
    }
    auto output = prefix(request_id, sequence);
    output += "ERROR ";
    output += code;
    output += '\n';
    return output;
}

std::string format_end(std::uint64_t request_id, std::uint64_t sequence) {
    auto output = prefix(request_id, sequence);
    output += "END\n";
    return output;
}

} // namespace exchangecore::protocol
