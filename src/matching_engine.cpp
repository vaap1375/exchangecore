#include "exchangecore/matching_engine.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>

namespace exchangecore {

MatchingEngine::MatchingEngine(EngineConfig config)
    : config_(config), book_(std::min(config.reserve_orders, config.max_orders)) {
    if (config.reserve_orders != 0 && config.max_orders != 0) {
        accepted_ids_.reserve(std::min(config.reserve_orders, config.max_orders));
    }
}

Result MatchingEngine::process(const Event& event) {
    switch (event.type) {
        case EventType::New:
            return submit(event.order);
        case EventType::Cancel:
            return cancel(event.order.id);
    }
    return {false, RejectReason::InvalidEvent, event.order.id, 0, event.order.quantity, 0, {}};
}

Result MatchingEngine::submit_limit(OrderId id, Side side, Quantity quantity, Price price) {
    return submit({id, side, OrderType::Limit, quantity, price});
}

Result MatchingEngine::submit_market(OrderId id, Side side, Quantity quantity) {
    return submit({id, side, OrderType::Market, quantity, 0});
}

RejectReason MatchingEngine::validate(const Order& order) const {
    if (order.id == 0) {
        return RejectReason::InvalidId;
    }
    if (order.side != Side::Buy && order.side != Side::Sell) {
        return RejectReason::InvalidSide;
    }
    if (order.type != OrderType::Limit && order.type != OrderType::Market) {
        return RejectReason::InvalidType;
    }
    if (order.quantity == 0) {
        return RejectReason::InvalidQuantity;
    }
    if ((order.type == OrderType::Limit && order.price <= 0) ||
        (order.type == OrderType::Market && order.price != 0)) {
        return RejectReason::InvalidPrice;
    }
    if (accepted_ids_.contains(order.id)) {
        return RejectReason::DuplicateId;
    }
    if (accepted_ids_.size() >= config_.max_orders) {
        return RejectReason::CapacityExceeded;
    }
    // Conservative bound permits staging the final remainder before fills and
    // keeps all book/level totals exact even at uint64_t boundaries.
    if (order.type == OrderType::Limit &&
        order.quantity > std::numeric_limits<Quantity>::max() - book_.total_quantity_) {
        return RejectReason::QuantityOverflow;
    }
    // Keep the maximum value as an exhaustion sentinel; never wrap priority.
    if (next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        return RejectReason::SequenceExhausted;
    }
    return RejectReason::None;
}

Result MatchingEngine::submit(const Order& order) {
    const RejectReason reason = validate(order);
    if (reason != RejectReason::None) {
        return {false, reason, order.id, 0, order.quantity, 0, {}};
    }

    Result result{true, RejectReason::None, order.id, 0, order.quantity, 0, {}};
    // Prepare the exact execution output while the book is read-only. Any
    // vector allocation failure therefore occurs before a fill is applied.
    const auto prepare = [&](auto first, auto last) {
        for (; first != last && result.remaining != 0; ++first) {
            const Price price = first->first;
            if (order.type == OrderType::Limit &&
                ((order.side == Side::Buy && price > order.price) ||
                 (order.side == Side::Sell && price < order.price))) {
                break;
            }
            for (const RestingOrder& maker : first->second.orders) {
                const Quantity filled = std::min(result.remaining, maker.quantity);
                result.trades.push_back({maker.id, order.id, price, filled});
                result.filled += filled;
                result.remaining -= filled;
                if (result.remaining == 0) {
                    break;
                }
            }
        }
    };
    if (order.side == Side::Buy) {
        prepare(book_.asks_.cbegin(), book_.asks_.cend());
    } else {
        prepare(book_.bids_.crbegin(), book_.bids_.crend());
    }

    // Finish all allocations before committing executions. add() rolls back
    // its containers on allocation failure, then we roll back ID consumption.
    const auto [accepted, inserted] = accepted_ids_.insert(order.id);
    if (!inserted) {
        throw std::logic_error("validated order ID unexpectedly already accepted");
    }
    try {
        if (order.type == OrderType::Limit && result.remaining != 0) {
            book_.add({order.id, order.side, order.price, result.remaining, next_sequence_});
        }
    } catch (...) {
        accepted_ids_.erase(accepted);
        throw;
    }
    ++next_sequence_;

    auto& opposite = order.side == Side::Buy ? book_.asks_ : book_.bids_;
    for (const Trade& trade : result.trades) {
        // Preparation fixed execution order; no lookup or allocation is needed
        // for partially filled makers. Map/list erasure preserves other nodes.
        auto level = order.side == Side::Buy ? opposite.begin() : std::prev(opposite.end());
        auto& orders = level->second.orders;
        RestingOrder& maker = orders.front();
        maker.quantity -= trade.quantity;
        level->second.quantity -= trade.quantity;
        book_.total_quantity_ -= trade.quantity;
        if (maker.quantity == 0) {
            book_.locations_.erase(maker.id);
            orders.pop_front();
            if (orders.empty()) {
                opposite.erase(level);
            }
        }
    }
    debug_check();
    return result;
}

Result MatchingEngine::cancel(OrderId id) {
    if (id == 0) {
        return {false, RejectReason::InvalidId, id, 0, 0, 0, {}};
    }
    const Quantity canceled = book_.erase(id);
    if (canceled == 0) {
        return {false, RejectReason::UnknownOrder, id, 0, 0, 0, {}};
    }
    debug_check();
    return {true, RejectReason::None, id, 0, 0, canceled, {}};
}

std::optional<Price> MatchingEngine::best_bid() const noexcept {
    return book_.best_bid();
}

std::optional<Price> MatchingEngine::best_ask() const noexcept {
    return book_.best_ask();
}

std::vector<Level> MatchingEngine::depth(Side side, std::size_t levels) const {
    return book_.depth(side, levels);
}

std::optional<RestingOrder> MatchingEngine::find(OrderId id) const {
    return book_.find(id);
}

Snapshot MatchingEngine::snapshot() const {
    return book_.snapshot();
}

std::size_t MatchingEngine::active_order_count() const noexcept {
    return book_.active_order_count();
}

std::size_t MatchingEngine::accepted_order_count() const noexcept {
    return accepted_ids_.size();
}

Quantity MatchingEngine::resting_quantity() const noexcept {
    return book_.resting_quantity();
}

void MatchingEngine::check_invariants() const {
    book_.check_invariants();
    if (accepted_ids_.size() > config_.max_orders ||
        accepted_ids_.size() != next_sequence_ - 1 || accepted_ids_.contains(0)) {
        throw std::logic_error("accepted ID lifecycle or arrival sequence is inconsistent");
    }
    for (const auto& [id, location] : book_.locations_) {
        if (!accepted_ids_.contains(id) || location.order->sequence >= next_sequence_) {
            throw std::logic_error("active order was never accepted or has invalid arrival sequence");
        }
    }
}

void MatchingEngine::debug_check() const {
#if defined(EXCHANGECORE_ENABLE_INVARIANTS) && EXCHANGECORE_ENABLE_INVARIANTS
    check_invariants();
#endif
}

}  // namespace exchangecore
