#pragma once

#include "exchangecore/order.hpp"

#include <vector>

namespace exchangecore {

struct Trade {
    OrderId maker_id{};
    OrderId taker_id{};
    Price price{};
    Quantity quantity{};

    bool operator==(const Trade&) const = default;
};

enum class RejectReason : std::uint8_t {
    None,
    InvalidId,
    InvalidSide,
    InvalidType,
    InvalidQuantity,
    InvalidPrice,
    DuplicateId,
    UnknownOrder,
    CapacityExceeded,
    QuantityOverflow,
    SequenceExhausted,
    InvalidEvent
};

struct Result {
    bool accepted{};
    RejectReason reason{RejectReason::None};
    OrderId order_id{};
    Quantity filled{};
    // Unfilled incoming quantity, including a resting limit remainder. Zero for
    // cancels. Rejected new orders report their original requested quantity.
    Quantity remaining{};
    Quantity canceled{};
    std::vector<Trade> trades;

    bool operator==(const Result&) const = default;
};

}  // namespace exchangecore
