#pragma once

#include "exchangecore/order.hpp"

#include <limits>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace exchangecore {

struct Snapshot {
    // Bids descending, asks ascending; FIFO within each price level.
    std::vector<RestingOrder> bids;
    std::vector<RestingOrder> asks;

    bool operator==(const Snapshot&) const = default;
};

class MatchingEngine;

// Owns orders and their lookup iterators. Intentionally noncopyable/nonmovable:
// transferring this object must never detach iterators from their containers.
class OrderBook {
public:
    explicit OrderBook(std::size_t reserve_orders = 0);
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = delete;
    OrderBook& operator=(OrderBook&&) = delete;

    [[nodiscard]] std::optional<Price> best_bid() const noexcept;
    [[nodiscard]] std::optional<Price> best_ask() const noexcept;
    [[nodiscard]] std::vector<Level> depth(
        Side side, std::size_t levels = std::numeric_limits<std::size_t>::max()) const;
    [[nodiscard]] std::optional<RestingOrder> find(OrderId id) const;
    [[nodiscard]] Snapshot snapshot() const;
    [[nodiscard]] std::size_t active_order_count() const noexcept;
    [[nodiscard]] Quantity resting_quantity() const noexcept;
    // Throws std::logic_error with a diagnostic on broken internal invariants.
    void check_invariants() const;

private:
    friend class MatchingEngine;

    struct PriceLevel {
        std::list<RestingOrder> orders;
        Quantity quantity{};
    };
    using Levels = std::map<Price, PriceLevel>;
    struct Location {
        Side side;
        Levels::iterator level;
        std::list<RestingOrder>::iterator order;
    };

    Levels bids_;
    Levels asks_;
    std::unordered_map<OrderId, Location> locations_;
    Quantity total_quantity_{};

    void add(const RestingOrder& order);
    Quantity erase(OrderId id);
};

}  // namespace exchangecore
