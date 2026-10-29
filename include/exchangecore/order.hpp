#pragma once

#include <cstddef>
#include <cstdint>

namespace exchangecore {

using OrderId = std::uint64_t;
using Quantity = std::uint64_t;
using Price = std::int64_t;

enum class Side : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market };

struct Order {
    OrderId id{};
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    Quantity quantity{};
    Price price{};

    bool operator==(const Order&) const = default;
};

// Arrival sequence is assigned to every accepted new order, including markets.
struct RestingOrder {
    OrderId id{};
    Side side{Side::Buy};
    Price price{};
    Quantity quantity{};
    std::uint64_t sequence{};

    bool operator==(const RestingOrder&) const = default;
};

struct Level {
    Price price{};
    Quantity quantity{};
    std::size_t order_count{};

    bool operator==(const Level&) const = default;
};

struct EngineConfig {
    std::size_t reserve_orders{0};
    // Bounds all IDs ever accepted, including canceled and fully filled orders.
    std::size_t max_orders{1'000'000};
};

}  // namespace exchangecore
