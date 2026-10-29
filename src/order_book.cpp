#include "exchangecore/order_book.hpp"

#include <algorithm>
#include <iterator>
#include <memory>
#include <stdexcept>

namespace exchangecore {

OrderBook::OrderBook(std::size_t reserve_orders) {
    if (reserve_orders != 0) {
        locations_.reserve(reserve_orders);
    }
}

std::optional<Price> OrderBook::best_bid() const noexcept {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.rbegin()->first;
}

std::optional<Price> OrderBook::best_ask() const noexcept {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

std::vector<Level> OrderBook::depth(Side side, std::size_t levels) const {
    if (side != Side::Buy && side != Side::Sell) {
        throw std::invalid_argument("depth requires Buy or Sell side");
    }
    std::vector<Level> result;
    const std::size_t available = side == Side::Buy ? bids_.size() : asks_.size();
    result.reserve(std::min(levels, available));
    const auto append = [&](auto first, auto last) {
        for (; first != last && result.size() < levels; ++first) {
            result.push_back({first->first, first->second.quantity, first->second.orders.size()});
        }
    };
    if (side == Side::Buy) {
        append(bids_.crbegin(), bids_.crend());
    } else {
        append(asks_.cbegin(), asks_.cend());
    }
    return result;
}

std::optional<RestingOrder> OrderBook::find(OrderId id) const {
    const auto found = locations_.find(id);
    if (found == locations_.end()) {
        return std::nullopt;
    }
    return *found->second.order;
}

Snapshot OrderBook::snapshot() const {
    Snapshot result;
    // Separate counts avoid allocating capacity for the other side's orders.
    std::size_t bid_count = 0;
    for (const auto& [price, level] : bids_) {
        static_cast<void>(price);
        bid_count += level.orders.size();
    }
    result.bids.reserve(bid_count);
    result.asks.reserve(locations_.size() - bid_count);
    for (auto level = bids_.crbegin(); level != bids_.crend(); ++level) {
        result.bids.insert(result.bids.end(), level->second.orders.begin(), level->second.orders.end());
    }
    for (const auto& [price, level] : asks_) {
        static_cast<void>(price);
        result.asks.insert(result.asks.end(), level.orders.begin(), level.orders.end());
    }
    return result;
}

std::size_t OrderBook::active_order_count() const noexcept {
    return locations_.size();
}

Quantity OrderBook::resting_quantity() const noexcept {
    return total_quantity_;
}

void OrderBook::add(const RestingOrder& order) {
    auto& levels = order.side == Side::Buy ? bids_ : asks_;
    const auto [level, inserted_level] = levels.try_emplace(order.price);
    try {
        level->second.orders.push_back(order);
        try {
            const auto [location, inserted_order] = locations_.emplace(
                order.id, Location{order.side, level, std::prev(level->second.orders.end())});
            static_cast<void>(location);
            if (!inserted_order) {
                throw std::logic_error("attempted duplicate resting order insertion");
            }
        } catch (...) {
            level->second.orders.pop_back();
            throw;
        }
    } catch (...) {
        if (inserted_level) {
            levels.erase(level);
        }
        throw;
    }
    // The engine has already established that both additions fit.
    level->second.quantity += order.quantity;
    total_quantity_ += order.quantity;
}

Quantity OrderBook::erase(OrderId id) {
    const auto found = locations_.find(id);
    if (found == locations_.end()) {
        return 0;
    }
    const Location location = found->second;
    const Quantity removed = location.order->quantity;
    auto& levels = location.side == Side::Buy ? bids_ : asks_;
    location.level->second.quantity -= removed;
    total_quantity_ -= removed;
    // Destroy the index before its referenced list node or level.
    locations_.erase(found);
    location.level->second.orders.erase(location.order);
    if (location.level->second.orders.empty()) {
        levels.erase(location.level);
    }
    return removed;
}

void OrderBook::check_invariants() const {
    std::size_t order_count = 0;
    Quantity total = 0;
    const auto inspect = [&](const Levels& levels, Side side) {
        for (auto level = levels.cbegin(); level != levels.cend(); ++level) {
            if (level->first <= 0 || level->second.orders.empty()) {
                throw std::logic_error("book contains invalid price or empty price level");
            }
            Quantity level_quantity = 0;
            std::uint64_t prior_sequence = 0;
            for (const RestingOrder& order : level->second.orders) {
                if (order.id == 0 || order.quantity == 0 || order.sequence <= prior_sequence) {
                    throw std::logic_error("resting order has invalid ID, quantity, or FIFO sequence");
                }
                if (order.side != side || order.price != level->first) {
                    throw std::logic_error("resting order belongs to wrong side or price level");
                }
                const auto found = locations_.find(order.id);
                if (found == locations_.end() || found->second.side != side ||
                    found->second.level != level ||
                    std::addressof(*found->second.order) != std::addressof(order)) {
                    throw std::logic_error("order index does not reference its unique active order");
                }
                if (order.quantity > std::numeric_limits<Quantity>::max() - level_quantity) {
                    throw std::logic_error("price level quantity overflows");
                }
                level_quantity += order.quantity;
                prior_sequence = order.sequence;
                ++order_count;
            }
            if (level_quantity != level->second.quantity) {
                throw std::logic_error("cached price level quantity is inconsistent");
            }
            if (level_quantity > std::numeric_limits<Quantity>::max() - total) {
                throw std::logic_error("total resting quantity overflows");
            }
            total += level_quantity;
        }
    };
    inspect(bids_, Side::Buy);
    inspect(asks_, Side::Sell);
    if (order_count != locations_.size() || total != total_quantity_) {
        throw std::logic_error("active count or total resting quantity is inconsistent");
    }
    if (!bids_.empty() && !asks_.empty() && bids_.rbegin()->first >= asks_.begin()->first) {
        throw std::logic_error("order book is crossed");
    }
}

}  // namespace exchangecore
