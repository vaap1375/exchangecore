#pragma once

#include "exchangecore/order.hpp"

namespace exchangecore {

enum class EventType : std::uint8_t { New, Cancel };

struct Event {
    EventType type{EventType::New};
    Order order{};

    static constexpr Event limit(OrderId id, Side side, Quantity quantity, Price price) noexcept {
        return {EventType::New, {id, side, OrderType::Limit, quantity, price}};
    }

    static constexpr Event market(OrderId id, Side side, Quantity quantity) noexcept {
        return {EventType::New, {id, side, OrderType::Market, quantity, 0}};
    }

    // Only the ID is meaningful for a cancellation.
    static constexpr Event cancel(OrderId id) noexcept {
        return {EventType::Cancel, {id, Side::Buy, OrderType::Limit, 0, 0}};
    }

    bool operator==(const Event&) const = default;
};

}  // namespace exchangecore
