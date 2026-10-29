#pragma once

#include "exchangecore/event.hpp"
#include "exchangecore/order_book.hpp"
#include "exchangecore/trade.hpp"

#include <limits>
#include <optional>
#include <unordered_set>

namespace exchangecore {

// Single-threaded deterministic price/time matching. Expected validation errors
// return Result without mutations. Allocation failures propagate; see README's
// exception contract before attempting recovery from std::bad_alloc.
class MatchingEngine {
public:
    explicit MatchingEngine(EngineConfig config = {});
    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;
    MatchingEngine(MatchingEngine&&) = delete;
    MatchingEngine& operator=(MatchingEngine&&) = delete;

    [[nodiscard]] Result process(const Event& event);
    [[nodiscard]] Result submit_limit(OrderId id, Side side, Quantity quantity, Price price);
    [[nodiscard]] Result submit_market(OrderId id, Side side, Quantity quantity);
    [[nodiscard]] Result cancel(OrderId id);

    [[nodiscard]] std::optional<Price> best_bid() const noexcept;
    [[nodiscard]] std::optional<Price> best_ask() const noexcept;
    [[nodiscard]] std::vector<Level> depth(
        Side side, std::size_t levels = std::numeric_limits<std::size_t>::max()) const;
    [[nodiscard]] std::optional<RestingOrder> find(OrderId id) const;
    [[nodiscard]] Snapshot snapshot() const;
    [[nodiscard]] std::size_t active_order_count() const noexcept;
    [[nodiscard]] std::size_t accepted_order_count() const noexcept;
    [[nodiscard]] Quantity resting_quantity() const noexcept;
    void check_invariants() const;

private:
    EngineConfig config_;
    OrderBook book_;
    std::unordered_set<OrderId> accepted_ids_;
    std::uint64_t next_sequence_{1};

    [[nodiscard]] Result submit(const Order& order);
    [[nodiscard]] RejectReason validate(const Order& order) const;
    void debug_check() const;
};

}  // namespace exchangecore
