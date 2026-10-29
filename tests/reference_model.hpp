#pragma once

#include "exchangecore/matching_engine.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <vector>

namespace exchangecore::test {

// A deliberately slow oracle. There are no price levels, queues, order indexes,
// or calls to MatchingEngine. Every fill selects its maker by a full scan.
class ReferenceModel {
public:
    explicit ReferenceModel(EngineConfig config = {}) : config_(config) {}

    [[nodiscard]] Result process(const Event& event) {
        Result result{};
        result.order_id = event.order.id;
        if (event.type == EventType::Cancel) {
            if (event.order.id == 0) {
                result.reason = RejectReason::InvalidId;
                return result;
            }
            for (auto it = active_.begin(); it != active_.end(); ++it) {
                if (it->id == event.order.id) {
                    result.accepted = true;
                    result.canceled = it->quantity;
                    active_.erase(it);
                    return result;
                }
            }
            result.reason = RejectReason::UnknownOrder;
            return result;
        }
        result.remaining = event.order.quantity;
        if (event.type != EventType::New) {
            result.reason = RejectReason::InvalidEvent;
            return result;
        }
        const auto& order = event.order;
        const auto reject = [&](RejectReason reason) {
            result.reason = reason;
            return result;
        };
        if (order.id == 0) return reject(RejectReason::InvalidId);
        if (order.side != Side::Buy && order.side != Side::Sell) {
            return reject(RejectReason::InvalidSide);
        }
        if (order.type != OrderType::Limit && order.type != OrderType::Market) {
            return reject(RejectReason::InvalidType);
        }
        if (order.quantity == 0) return reject(RejectReason::InvalidQuantity);
        if ((order.type == OrderType::Limit && order.price <= 0) ||
            (order.type == OrderType::Market && order.price != 0)) {
            return reject(RejectReason::InvalidPrice);
        }
        if (std::find(accepted_ids_.begin(), accepted_ids_.end(), order.id) != accepted_ids_.end()) {
            return reject(RejectReason::DuplicateId);
        }
        if (accepted_ids_.size() >= config_.max_orders) return reject(RejectReason::CapacityExceeded);
        if (order.type == OrderType::Limit &&
            order.quantity > std::numeric_limits<Quantity>::max() - resting_quantity()) {
            return reject(RejectReason::QuantityOverflow);
        }
        if (next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
            return reject(RejectReason::SequenceExhausted);
        }

        accepted_ids_.push_back(order.id);
        const auto sequence = next_sequence_++;
        result.accepted = true;
        while (result.remaining != 0) {
            std::optional<std::size_t> best;
            for (std::size_t i = 0; i < active_.size(); ++i) {
                const auto& maker = active_[i];
                if (maker.side == order.side) continue;
                const bool price_ok = order.type == OrderType::Market ||
                    (order.side == Side::Buy ? maker.price <= order.price : maker.price >= order.price);
                if (!price_ok) continue;
                if (!best.has_value() || better_maker(maker, active_[*best], order.side)) best = i;
            }
            if (!best.has_value()) break;
            auto& maker = active_[*best];
            const auto quantity = std::min(result.remaining, maker.quantity);
            result.trades.push_back(Trade{maker.id, order.id, maker.price, quantity});
            result.remaining -= quantity;
            result.filled += quantity;
            maker.quantity -= quantity;
            if (maker.quantity == 0) active_.erase(active_.begin() + static_cast<std::ptrdiff_t>(*best));
        }
        if (result.remaining != 0 && order.type == OrderType::Limit) {
            active_.push_back(RestingOrder{order.id, order.side, order.price, result.remaining, sequence});
        }
        return result;
    }

    [[nodiscard]] Snapshot snapshot() const {
        Snapshot result;
        for (const auto& order : active_) {
            (order.side == Side::Buy ? result.bids : result.asks).push_back(order);
        }
        const auto by_bid = [](const RestingOrder& a, const RestingOrder& b) {
            return a.price != b.price ? a.price > b.price : a.sequence < b.sequence;
        };
        const auto by_ask = [](const RestingOrder& a, const RestingOrder& b) {
            return a.price != b.price ? a.price < b.price : a.sequence < b.sequence;
        };
        std::sort(result.bids.begin(), result.bids.end(), by_bid);
        std::sort(result.asks.begin(), result.asks.end(), by_ask);
        return result;
    }

    [[nodiscard]] std::vector<Level> depth(Side side, std::size_t limit) const {
        const auto state = snapshot();
        const auto& orders = side == Side::Buy ? state.bids : state.asks;
        std::vector<Level> levels;
        for (const auto& order : orders) {
            if (levels.empty() || levels.back().price != order.price) {
                if (levels.size() == limit) break;
                levels.push_back(Level{order.price, 0, 0});
            }
            levels.back().quantity += order.quantity;
            ++levels.back().order_count;
        }
        return levels;
    }

    [[nodiscard]] std::optional<RestingOrder> find(OrderId id) const {
        for (const auto& order : active_) if (order.id == id) return order;
        return std::nullopt;
    }
    [[nodiscard]] std::optional<Price> best_bid() const { return best(Side::Buy); }
    [[nodiscard]] std::optional<Price> best_ask() const { return best(Side::Sell); }
    [[nodiscard]] std::size_t active_order_count() const { return active_.size(); }
    [[nodiscard]] std::size_t accepted_order_count() const { return accepted_ids_.size(); }
    [[nodiscard]] Quantity resting_quantity() const {
        Quantity result = 0;
        for (const auto& order : active_) result += order.quantity;
        return result;
    }

private:
    [[nodiscard]] static bool better_maker(const RestingOrder& candidate,
                                         const RestingOrder& previous,
                                         Side taker_side) {
        if (candidate.price != previous.price) {
            return taker_side == Side::Buy ? candidate.price < previous.price : candidate.price > previous.price;
        }
        return candidate.sequence < previous.sequence;
    }
    [[nodiscard]] std::optional<Price> best(Side side) const {
        std::optional<Price> result;
        for (const auto& order : active_) {
            if (order.side != side) continue;
            if (!result || (side == Side::Buy ? order.price > *result : order.price < *result)) {
                result = order.price;
            }
        }
        return result;
    }

    EngineConfig config_;
    std::vector<RestingOrder> active_;
    std::vector<OrderId> accepted_ids_;
    std::uint64_t next_sequence_{1};
};

} // namespace exchangecore::test
