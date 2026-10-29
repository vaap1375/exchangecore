#include "exchangecore/csv.hpp"
#include "exchangecore/matching_engine.hpp"
#include "exchangecore/price.hpp"
#include "reference_model.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace exchangecore;
using exchangecore::test::ReferenceModel;

struct TestCase { std::string_view name; void (*run)(); };
std::vector<TestCase>& cases() { static std::vector<TestCase> result; return result; }
struct Register {
    Register(std::string_view name, void (*run)()) { cases().push_back({name, run}); }
};
void check(bool condition, const char* expression, const char* file, int line) {
    if (!condition) {
        throw std::runtime_error(std::string(file) + ':' + std::to_string(line) + ": " + expression);
    }
}
#define JOIN_IMPL(a, b) a##b
#define JOIN(a, b) JOIN_IMPL(a, b)
#define TEST(name) \
    void JOIN(test_, __LINE__)(); \
    const Register JOIN(register_, __LINE__)(name, JOIN(test_, __LINE__)); \
    void JOIN(test_, __LINE__)()
#define CHECK(...) check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __FILE__, __LINE__)
#define CHECK_THROW(expression, exception_type) \
    do { bool caught = false; try { (void)(expression); } \
    catch (const exception_type&) { caught = true; } CHECK(caught); } while (false)

Result accept(MatchingEngine& engine, const Event& event) {
    auto result = engine.process(event);
    CHECK(result.accepted);
    CHECK(result.reason == RejectReason::None);
    engine.check_invariants();
    return result;
}

Result reject(MatchingEngine& engine, const Event& event, RejectReason reason) {
    const auto before = engine.snapshot();
    const auto accepted_before = engine.accepted_order_count();
    const auto quantity_before = engine.resting_quantity();
    auto result = engine.process(event);
    CHECK(!result.accepted);
    CHECK(result.reason == reason);
    CHECK(result.filled == 0);
    CHECK(result.canceled == 0);
    CHECK(result.trades.empty());
    CHECK(engine.snapshot() == before);
    CHECK(engine.accepted_order_count() == accepted_before);
    CHECK(engine.resting_quantity() == quantity_before);
    engine.check_invariants();
    return result;
}

RestingOrder active(const MatchingEngine& engine, OrderId id) {
    const auto order = engine.find(id);
    CHECK(order.has_value());
    return *order;
}

TEST("01 empty book has no best prices, orders, or depth") {
    MatchingEngine engine;
    CHECK(!engine.best_bid()); CHECK(!engine.best_ask());
    CHECK(engine.active_order_count() == 0); CHECK(engine.accepted_order_count() == 0);
    CHECK(engine.resting_quantity() == 0);
    CHECK(engine.snapshot() == Snapshot{});
    CHECK(engine.depth(Side::Buy).empty()); CHECK(engine.depth(Side::Sell).empty());
    CHECK(!engine.find(1));
    engine.check_invariants();
}

TEST("02 a single resting buy retains price, quantity, and sequence") {
    MatchingEngine engine;
    const auto result = accept(engine, Event::limit(1, Side::Buy, 7, 24000));
    CHECK(result.order_id == 1); CHECK(result.filled == 0); CHECK(result.remaining == 7);
    CHECK(result.trades.empty());
    CHECK(active(engine, 1) == RestingOrder{1, Side::Buy, 24000, 7, 1});
    CHECK(engine.best_bid() == 24000); CHECK(!engine.best_ask());
    CHECK(engine.depth(Side::Buy) == std::vector<Level>{{24000, 7, 1}});
}

TEST("03 a single resting sell appears on the ask") {
    MatchingEngine engine;
    accept(engine, Event::limit(8, Side::Sell, 4, 24001));
    CHECK(active(engine, 8) == RestingOrder{8, Side::Sell, 24001, 4, 1});
    CHECK(engine.best_ask() == 24001); CHECK(!engine.best_bid());
    CHECK(engine.depth(Side::Sell) == std::vector<Level>{{24001, 4, 1}});
}

TEST("04 noncrossing limits rest independently") {
    MatchingEngine engine;
    accept(engine, Event::limit(1, Side::Buy, 3, 99));
    const auto result = accept(engine, Event::limit(2, Side::Sell, 5, 101));
    CHECK(result.trades.empty()); CHECK(engine.active_order_count() == 2);
    CHECK(engine.best_bid() == 99); CHECK(engine.best_ask() == 101);
    CHECK(engine.resting_quantity() == 8);
}

TEST("05 an equal-price crossing limit executes") {
    MatchingEngine engine;
    accept(engine, Event::limit(1, Side::Sell, 4, 100));
    const auto result = accept(engine, Event::limit(2, Side::Buy, 4, 100));
    CHECK(result.trades == std::vector<Trade>{{1, 2, 100, 4}});
    CHECK(result.filled == 4); CHECK(result.remaining == 0);
}

TEST("06 full fills remove both orders and the empty level") {
    MatchingEngine engine;
    accept(engine, Event::limit(1, Side::Buy, 6, 100));
    accept(engine, Event::limit(2, Side::Sell, 6, 99));
    CHECK(!engine.find(1)); CHECK(!engine.find(2));
    CHECK(!engine.best_bid()); CHECK(!engine.best_ask());
    CHECK(engine.active_order_count() == 0); CHECK(engine.resting_quantity() == 0);
    CHECK(engine.accepted_order_count() == 2);
}

TEST("07 partial fills retain maker priority and exact residual") {
    MatchingEngine engine;
    accept(engine, Event::limit(1, Side::Sell, 10, 100));
    const auto result = accept(engine, Event::limit(2, Side::Buy, 4, 110));
    CHECK(result.filled == 4); CHECK(result.remaining == 0);
    CHECK(active(engine, 1) == RestingOrder{1, Side::Sell, 100, 6, 1});
    CHECK(!engine.find(2)); CHECK(engine.resting_quantity() == 6);
}

TEST("08 one incoming order generates several ordered fills") {
    MatchingEngine engine;
    accept(engine, Event::limit(1, Side::Sell, 2, 100));
    accept(engine, Event::limit(2, Side::Sell, 3, 100));
    accept(engine, Event::limit(3, Side::Sell, 5, 100));
    const auto result = accept(engine, Event::market(4, Side::Buy, 7));
    CHECK(result.trades == std::vector<Trade>{{1, 4, 100, 2}, {2, 4, 100, 3}, {3, 4, 100, 2}});
    CHECK(result.filled == 7); CHECK(active(engine, 3).quantity == 3);
}

TEST("09 crossing walks multiple prices and stops at the limit") {
    MatchingEngine engine;
    accept(engine, Event::limit(1, Side::Sell, 2, 101));
    accept(engine, Event::limit(2, Side::Sell, 3, 102));
    accept(engine, Event::limit(3, Side::Sell, 4, 103));
    const auto result = accept(engine, Event::limit(4, Side::Buy, 8, 102));
    CHECK(result.trades == std::vector<Trade>{{1, 4, 101, 2}, {2, 4, 102, 3}});
    CHECK(result.filled == 5); CHECK(result.remaining == 3);
    CHECK(active(engine, 4).quantity == 3); CHECK(engine.best_bid() == 102);
    CHECK(engine.best_ask() == 103); CHECK(active(engine, 3).quantity == 4);
}

TEST("10 best price outranks arrival priority on both sides") {
    MatchingEngine engine;
    accept(engine, Event::limit(1, Side::Sell, 1, 103));
    accept(engine, Event::limit(2, Side::Sell, 1, 101));
    accept(engine, Event::limit(3, Side::Sell, 1, 102));
    CHECK(accept(engine, Event::market(4, Side::Buy, 3)).trades ==
          std::vector<Trade>{{2, 4, 101, 1}, {3, 4, 102, 1}, {1, 4, 103, 1}});
    accept(engine, Event::limit(5, Side::Buy, 1, 97));
    accept(engine, Event::limit(6, Side::Buy, 1, 99));
    accept(engine, Event::limit(7, Side::Buy, 1, 98));
    CHECK(accept(engine, Event::market(8, Side::Sell, 3)).trades ==
          std::vector<Trade>{{6, 8, 99, 1}, {7, 8, 98, 1}, {5, 8, 97, 1}});
}

TEST("11 FIFO uses arrival rather than numeric order ID") {
    MatchingEngine engine;
    accept(engine, Event::limit(90, Side::Buy, 2, 100));
    accept(engine, Event::limit(3, Side::Buy, 2, 100));
    accept(engine, Event::limit(40, Side::Buy, 2, 100));
    const auto result = accept(engine, Event::market(100, Side::Sell, 5));
    CHECK(result.trades == std::vector<Trade>{{90, 100, 100, 2}, {3, 100, 100, 2}, {40, 100, 100, 1}});
    CHECK(active(engine, 40).quantity == 1);
}

TEST("12 both buy and sell takers execute at the resting price") {
    MatchingEngine engine;
    accept(engine, Event::limit(1, Side::Sell, 2, 100));
    CHECK(accept(engine, Event::limit(2, Side::Buy, 2, 120)).trades.front().price == 100);
    accept(engine, Event::limit(3, Side::Buy, 2, 110));
    CHECK(accept(engine, Event::limit(4, Side::Sell, 2, 90)).trades.front().price == 110);
}

TEST("13 trade, residual, cancellation, and book quantities conserve units") {
    MatchingEngine engine;
    accept(engine, Event::limit(1, Side::Sell, 9, 100));
    accept(engine, Event::limit(2, Side::Sell, 6, 101));
    const auto fill = accept(engine, Event::limit(3, Side::Buy, 20, 101));
    Quantity traded = 0;
    for (const auto& trade : fill.trades) traded += trade.quantity;
    CHECK(traded == 15); CHECK(fill.filled + fill.remaining == 20);
    CHECK(engine.resting_quantity() == 5);
    const auto cancel = accept(engine, Event::cancel(3));
    CHECK(cancel.canceled == 5); CHECK(engine.resting_quantity() == 0);
}

TEST("14 an unmatched market never rests and still consumes its ID") {
    MatchingEngine engine;
    const auto result = accept(engine, Event::market(1, Side::Buy, 10));
    CHECK(result.filled == 0); CHECK(result.remaining == 10); CHECK(result.trades.empty());
    CHECK(engine.active_order_count() == 0); CHECK(!engine.find(1));
    reject(engine, Event::limit(1, Side::Buy, 1, 100), RejectReason::DuplicateId);
}

TEST("15 a partially matched market reports unfilled quantity without resting it") {
    MatchingEngine engine;
    accept(engine, Event::limit(1, Side::Buy, 3, 100));
    const auto result = accept(engine, Event::market(2, Side::Sell, 8));
    CHECK(result.filled == 3); CHECK(result.remaining == 5);
    CHECK(result.trades == std::vector<Trade>{{1, 2, 100, 3}});
    CHECK(engine.resting_quantity() == 0); CHECK(engine.active_order_count() == 0);
}

TEST("16 canceling an active partially filled order removes its remaining quantity") {
    MatchingEngine engine;
    accept(engine, Event::limit(1, Side::Buy, 10, 100));
    accept(engine, Event::market(2, Side::Sell, 4));
    const auto result = accept(engine, Event::cancel(1));
    CHECK(result.order_id == 1); CHECK(result.canceled == 6);
    CHECK(result.filled == 0); CHECK(result.remaining == 0); CHECK(result.trades.empty());
    CHECK(!engine.find(1)); CHECK(!engine.best_bid());
}

TEST("17 canceling an unknown ID is a rejected operation") {
    MatchingEngine engine;
    reject(engine, Event::cancel(42), RejectReason::UnknownOrder);
    CHECK(engine.accepted_order_count() == 0);
}

TEST("18 canceling an already filled ID is rejected") {
    MatchingEngine engine;
    accept(engine, Event::limit(1, Side::Buy, 2, 100));
    accept(engine, Event::market(2, Side::Sell, 2));
    reject(engine, Event::cancel(1), RejectReason::UnknownOrder);
    reject(engine, Event::cancel(2), RejectReason::UnknownOrder);
}

TEST("19 canceling twice is rejected and ID reuse remains forbidden") {
    MatchingEngine engine;
    accept(engine, Event::limit(1, Side::Sell, 2, 100));
    accept(engine, Event::cancel(1));
    reject(engine, Event::cancel(1), RejectReason::UnknownOrder);
    reject(engine, Event::limit(1, Side::Buy, 2, 100), RejectReason::DuplicateId);
}

TEST("20 duplicate active and fully filled IDs are rejected") {
    MatchingEngine engine;
    accept(engine, Event::limit(1, Side::Sell, 2, 100));
    reject(engine, Event::market(1, Side::Buy, 2), RejectReason::DuplicateId);
    CHECK(active(engine, 1).quantity == 2);
    accept(engine, Event::market(2, Side::Buy, 2));
    reject(engine, Event::limit(1, Side::Buy, 1, 100), RejectReason::DuplicateId);
    reject(engine, Event::market(2, Side::Sell, 1), RejectReason::DuplicateId);
}

TEST("21 zero quantity is rejected for limit and market without consuming ID") {
    MatchingEngine engine;
    CHECK(reject(engine, Event::limit(1, Side::Buy, 0, 100), RejectReason::InvalidQuantity).remaining == 0);
    reject(engine, Event::market(2, Side::Sell, 0), RejectReason::InvalidQuantity);
    accept(engine, Event::limit(1, Side::Buy, 1, 100));
    accept(engine, Event::market(2, Side::Sell, 1));
    CHECK(engine.accepted_order_count() == 2);
}

TEST("22 zero and negative limit prices are rejected") {
    MatchingEngine engine;
    reject(engine, Event::limit(1, Side::Buy, 1, 0), RejectReason::InvalidPrice);
    reject(engine, Event::limit(2, Side::Sell, 1, -1), RejectReason::InvalidPrice);
    reject(engine, Event::limit(3, Side::Sell, 1, std::numeric_limits<Price>::min()), RejectReason::InvalidPrice);
    accept(engine, Event::limit(1, Side::Buy, 1, 1));
}

TEST("23 maximum integer quantities and prices match without overflow") {
    MatchingEngine engine;
    constexpr auto maximum = std::numeric_limits<Quantity>::max();
    constexpr auto max_price = std::numeric_limits<Price>::max();
    constexpr auto max_id = std::numeric_limits<OrderId>::max();
    accept(engine, Event::limit(max_id, Side::Sell, maximum, max_price));
    CHECK(engine.resting_quantity() == maximum);
    const auto result = accept(engine, Event::market(1, Side::Buy, maximum));
    CHECK(result.filled == maximum); CHECK(result.remaining == 0);
    CHECK(result.trades == std::vector<Trade>{{max_id, 1, max_price, maximum}});
    CHECK(engine.resting_quantity() == 0);
}

TEST("24 active lookup changes after a partial fill and disappears on cancel") {
    MatchingEngine engine;
    CHECK(!engine.find(10));
    accept(engine, Event::limit(10, Side::Sell, 8, 101));
    CHECK(active(engine, 10).quantity == 8);
    accept(engine, Event::market(11, Side::Buy, 3));
    CHECK(active(engine, 10).quantity == 5);
    accept(engine, Event::cancel(10));
    CHECK(!engine.find(10));
}

TEST("25 best bid and ask update after insertion, fill, and cancellation") {
    MatchingEngine engine;
    accept(engine, Event::limit(1, Side::Buy, 1, 98));
    accept(engine, Event::limit(2, Side::Buy, 1, 99));
    accept(engine, Event::limit(3, Side::Sell, 1, 101));
    accept(engine, Event::limit(4, Side::Sell, 1, 102));
    CHECK(engine.best_bid() == 99); CHECK(engine.best_ask() == 101);
    accept(engine, Event::market(5, Side::Sell, 1)); CHECK(engine.best_bid() == 98);
    accept(engine, Event::cancel(3)); CHECK(engine.best_ask() == 102);
    accept(engine, Event::cancel(1)); CHECK(!engine.best_bid());
    accept(engine, Event::market(6, Side::Buy, 1)); CHECK(!engine.best_ask());
}

TEST("26 removal of last order deletes a level while sibling orders preserve it") {
    MatchingEngine engine;
    accept(engine, Event::limit(1, Side::Buy, 1, 100));
    accept(engine, Event::limit(2, Side::Buy, 2, 100));
    accept(engine, Event::limit(3, Side::Buy, 4, 99));
    accept(engine, Event::cancel(1));
    CHECK(engine.depth(Side::Buy) == std::vector<Level>{{100, 2, 1}, {99, 4, 1}});
    accept(engine, Event::cancel(2));
    CHECK(engine.depth(Side::Buy) == std::vector<Level>{{99, 4, 1}});
    CHECK(engine.best_bid() == 99);
}

TEST("27 repeated event replay yields identical results and final state") {
    const std::vector<Event> events{
        Event::limit(1, Side::Sell, 5, 101), Event::limit(2, Side::Sell, 4, 102),
        Event::limit(3, Side::Buy, 6, 102), Event::cancel(2), Event::cancel(2),
        Event::market(4, Side::Sell, 9), Event::limit(5, Side::Buy, 2, 98),
        Event::limit(5, Side::Buy, 2, 99), Event::limit(6, Side::Sell, 1, 97)};
    MatchingEngine first, second;
    for (const auto& event : events) {
        CHECK(first.process(event) == second.process(event));
        CHECK(first.snapshot() == second.snapshot());
        first.check_invariants(); second.check_invariants();
    }
    CHECK(first.resting_quantity() == 1); CHECK(active(first, 5).quantity == 1);
}

TEST("28 canceling queue head, middle, and tail leaves valid FIFO iterators") {
    MatchingEngine engine;
    for (OrderId id = 1; id <= 7; ++id) accept(engine, Event::limit(id, Side::Sell, 2, 100));
    accept(engine, Event::cancel(1)); accept(engine, Event::cancel(4)); accept(engine, Event::cancel(7));
    accept(engine, Event::limit(8, Side::Sell, 2, 100));
    CHECK(accept(engine, Event::market(9, Side::Buy, 10)).trades ==
          std::vector<Trade>{{2, 9, 100, 2}, {3, 9, 100, 2}, {5, 9, 100, 2}, {6, 9, 100, 2}, {8, 9, 100, 2}});
    CHECK(engine.active_order_count() == 0);
}

TEST("29 rejections preserve complete state and subsequent matching") {
    MatchingEngine engine;
    accept(engine, Event::limit(1, Side::Sell, 5, 100));
    reject(engine, Event::limit(1, Side::Buy, 2, 100), RejectReason::DuplicateId);
    reject(engine, Event::limit(2, Side::Buy, 0, 100), RejectReason::InvalidQuantity);
    reject(engine, Event::cancel(33), RejectReason::UnknownOrder);
    const auto result = accept(engine, Event::market(2, Side::Buy, 3));
    CHECK(result.trades == std::vector<Trade>{{1, 2, 100, 3}});
    CHECK(active(engine, 1).quantity == 2);
}

TEST("30 invariant checks cover mixed insertion, matching, rejection, and erasure") {
    MatchingEngine engine;
    ReferenceModel reference;
    const std::vector<Event> events{
        Event::limit(1, Side::Buy, 3, 98), Event::limit(2, Side::Buy, 5, 99),
        Event::limit(3, Side::Sell, 8, 101), Event::limit(4, Side::Sell, 3, 101),
        Event::market(5, Side::Buy, 9), Event::cancel(2), Event::market(6, Side::Sell, 1),
        Event::limit(7, Side::Buy, 5, 103), Event::cancel(3), Event::cancel(7)};
    for (const auto& event : events) {
        CHECK(engine.process(event) == reference.process(event));
        CHECK(engine.snapshot() == reference.snapshot());
        CHECK(engine.resting_quantity() == reference.resting_quantity());
        engine.check_invariants();
    }
}

TEST("31 aggregate resting quantity overflow is rejected before any matching") {
    MatchingEngine engine;
    constexpr auto maximum = std::numeric_limits<Quantity>::max();
    accept(engine, Event::limit(1, Side::Sell, maximum, 100));
    reject(engine, Event::limit(2, Side::Buy, 1, 100), RejectReason::QuantityOverflow);
    reject(engine, Event::limit(3, Side::Sell, 1, 101), RejectReason::QuantityOverflow);
    const auto result = accept(engine, Event::market(2, Side::Buy, maximum));
    CHECK(result.filled == maximum); CHECK(engine.resting_quantity() == 0);
}

TEST("32 combined bid and ask quantities share one safe aggregate bound") {
    MatchingEngine engine;
    constexpr auto maximum = std::numeric_limits<Quantity>::max();
    accept(engine, Event::limit(1, Side::Buy, maximum - 3, 99));
    accept(engine, Event::limit(2, Side::Sell, 3, 101));
    CHECK(engine.resting_quantity() == maximum);
    reject(engine, Event::limit(3, Side::Sell, 1, 102), RejectReason::QuantityOverflow);
    accept(engine, Event::cancel(2));
    accept(engine, Event::limit(3, Side::Sell, 3, 102));
    CHECK(engine.resting_quantity() == maximum);
}

TEST("33 lifetime capacity counts filled, canceled, and unmatched market IDs") {
    MatchingEngine engine(EngineConfig{0, 3});
    accept(engine, Event::limit(1, Side::Buy, 1, 100));
    accept(engine, Event::cancel(1));
    accept(engine, Event::market(2, Side::Sell, 1));
    accept(engine, Event::limit(3, Side::Sell, 2, 101));
    reject(engine, Event::market(4, Side::Buy, 2), RejectReason::CapacityExceeded);
    CHECK(active(engine, 3).quantity == 2);
    accept(engine, Event::cancel(3));
    reject(engine, Event::limit(5, Side::Buy, 1, 100), RejectReason::CapacityExceeded);
    reject(engine, Event::limit(1, Side::Buy, 1, 100), RejectReason::DuplicateId);
}

TEST("34 zero lifetime capacity rejects new orders while cancel stays meaningful") {
    MatchingEngine engine(EngineConfig{0, 0});
    reject(engine, Event::limit(1, Side::Buy, 1, 100), RejectReason::CapacityExceeded);
    reject(engine, Event::cancel(1), RejectReason::UnknownOrder);
}

TEST("35 reserved and unreserved indexes produce identical behavior") {
    MatchingEngine baseline(EngineConfig{0, 1000}), reserved(EngineConfig{1000, 1000});
    for (OrderId id = 1; id <= 300; ++id) {
        const auto event = Event::limit(id, Side::Buy, id % 9 + 1, 100 - static_cast<Price>(id % 11));
        CHECK(baseline.process(event) == reserved.process(event));
    }
    for (OrderId id = 1; id <= 300; id += 3) CHECK(baseline.cancel(id) == reserved.cancel(id));
    CHECK(baseline.submit_market(400, Side::Sell, 600) == reserved.submit_market(400, Side::Sell, 600));
    CHECK(baseline.snapshot() == reserved.snapshot());
    baseline.check_invariants(); reserved.check_invariants();
}

TEST("36 invalid ID, side, type, and event have explicit rejection reasons") {
    MatchingEngine engine;
    reject(engine, Event::limit(0, Side::Buy, 1, 100), RejectReason::InvalidId);
    reject(engine, Event::cancel(0), RejectReason::InvalidId);
    reject(engine, Event::limit(1, static_cast<Side>(255), 1, 100), RejectReason::InvalidSide);
    auto event = Event::limit(1, Side::Buy, 1, 100);
    event.order.type = static_cast<OrderType>(255);
    reject(engine, event, RejectReason::InvalidType);
    event = Event::limit(1, Side::Buy, 1, 100); event.type = static_cast<EventType>(255);
    reject(engine, event, RejectReason::InvalidEvent);
    accept(engine, Event::limit(1, Side::Buy, 1, 100));
    CHECK(active(engine, 1).sequence == 1);
}

TEST("37 market prices must be zero and cancellation ignores irrelevant fields") {
    MatchingEngine engine;
    auto event = Event::market(1, Side::Buy, 1); event.order.price = 100;
    reject(engine, event, RejectReason::InvalidPrice);
    accept(engine, Event::limit(1, Side::Buy, 2, 100));
    event = Event::cancel(1); event.order.side = static_cast<Side>(255);
    event.order.type = static_cast<OrderType>(255); event.order.quantity = 123; event.order.price = -5;
    CHECK(accept(engine, event).canceled == 2);
}

TEST("38 sequence counts markets and accepted limits while rejections do not advance it") {
    MatchingEngine engine;
    accept(engine, Event::market(1, Side::Buy, 1));
    reject(engine, Event::limit(2, Side::Buy, 0, 100), RejectReason::InvalidQuantity);
    accept(engine, Event::limit(2, Side::Buy, 2, 100));
    accept(engine, Event::limit(3, Side::Sell, 1, 99));
    accept(engine, Event::limit(4, Side::Buy, 2, 98));
    CHECK(active(engine, 2).sequence == 2); CHECK(active(engine, 4).sequence == 4);
}

TEST("39 incoming residual keeps its original arrival sequence") {
    MatchingEngine engine;
    accept(engine, Event::limit(1, Side::Sell, 2, 100));
    accept(engine, Event::limit(2, Side::Buy, 7, 101));
    accept(engine, Event::limit(3, Side::Buy, 3, 101));
    CHECK(active(engine, 2) == RestingOrder{2, Side::Buy, 101, 5, 2});
    CHECK(accept(engine, Event::market(4, Side::Sell, 6)).trades ==
          std::vector<Trade>{{2, 4, 101, 5}, {3, 4, 101, 1}});
}

TEST("40 depth aggregates price levels in side order and respects level limits") {
    MatchingEngine engine;
    accept(engine, Event::limit(1, Side::Buy, 2, 99));
    accept(engine, Event::limit(2, Side::Buy, 3, 99));
    accept(engine, Event::limit(3, Side::Buy, 4, 98));
    accept(engine, Event::limit(4, Side::Sell, 5, 101));
    accept(engine, Event::limit(5, Side::Sell, 6, 102));
    CHECK(engine.depth(Side::Buy) == std::vector<Level>{{99, 5, 2}, {98, 4, 1}});
    CHECK(engine.depth(Side::Sell) == std::vector<Level>{{101, 5, 1}, {102, 6, 1}});
    CHECK(engine.depth(Side::Buy, 1) == std::vector<Level>{{99, 5, 2}});
    CHECK(engine.depth(Side::Sell, 0).empty());
    CHECK_THROW(engine.depth(static_cast<Side>(255)), std::invalid_argument);
}

TEST("41 snapshots are detached values with deterministic price and FIFO ordering") {
    MatchingEngine engine;
    accept(engine, Event::limit(9, Side::Buy, 1, 98));
    accept(engine, Event::limit(8, Side::Buy, 1, 99));
    accept(engine, Event::limit(7, Side::Buy, 1, 99));
    auto snapshot = engine.snapshot();
    CHECK(snapshot.bids == std::vector<RestingOrder>{{8, Side::Buy, 99, 1, 2},
          {7, Side::Buy, 99, 1, 3}, {9, Side::Buy, 98, 1, 1}});
    snapshot.bids.front().quantity = 900;
    accept(engine, Event::cancel(8));
    CHECK(snapshot.bids.front().id == 8); CHECK(active(engine, 7).quantity == 1);
}

TEST("42 public convenience submission methods agree with event processing") {
    MatchingEngine direct, events;
    CHECK(direct.submit_limit(1, Side::Sell, 5, 100) == events.process(Event::limit(1, Side::Sell, 5, 100)));
    CHECK(direct.submit_market(2, Side::Buy, 2) == events.process(Event::market(2, Side::Buy, 2)));
    CHECK(direct.cancel(1) == events.process(Event::cancel(1)));
    CHECK(direct.snapshot() == events.snapshot());
}

TEST("43 index growth followed by randomized cancellation preserves every survivor") {
    MatchingEngine engine;
    constexpr OrderId count = 600;
    std::vector<OrderId> ids;
    for (OrderId id = 1; id <= count; ++id) {
        accept(engine, Event::limit(id, Side::Buy, id, 1000 - static_cast<Price>(id % 100)));
        ids.push_back(id);
    }
    std::mt19937_64 generator(114);
    std::shuffle(ids.begin(), ids.end(), generator);
    Quantity remaining = count * (count + 1) / 2;
    for (const auto id : ids) {
        CHECK(accept(engine, Event::cancel(id)).canceled == id);
        remaining -= id; CHECK(engine.resting_quantity() == remaining);
    }
    CHECK(engine.depth(Side::Buy).empty()); CHECK(engine.active_order_count() == 0);
}

void compare_state(const MatchingEngine& engine, const ReferenceModel& reference, OrderId touched) {
    CHECK(engine.snapshot() == reference.snapshot());
    CHECK(engine.active_order_count() == reference.active_order_count());
    CHECK(engine.accepted_order_count() == reference.accepted_order_count());
    CHECK(engine.resting_quantity() == reference.resting_quantity());
    CHECK(engine.best_bid() == reference.best_bid()); CHECK(engine.best_ask() == reference.best_ask());
    CHECK(engine.find(touched) == reference.find(touched));
    const auto snapshot = reference.snapshot();
    for (const auto& order : snapshot.bids) CHECK(engine.find(order.id) == order);
    for (const auto& order : snapshot.asks) CHECK(engine.find(order.id) == order);
    for (const auto side : {Side::Buy, Side::Sell}) {
        for (const std::size_t limit : {std::size_t{0}, std::size_t{1}, std::size_t{3}, std::size_t{10000}}) {
            CHECK(engine.depth(side, limit) == reference.depth(side, limit));
        }
    }
    if (engine.best_bid() && engine.best_ask()) CHECK(*engine.best_bid() < *engine.best_ask());
    engine.check_invariants();
}

enum class Regime { LowLiquidity, HighLiquidity, HeavyMatching, HeavyCancellation,
                    DeepBook, ShallowBook, InvalidInput, Mixed };

std::string_view regime_name(Regime regime) {
    switch (regime) {
        case Regime::LowLiquidity: return "low-liquidity";
        case Regime::HighLiquidity: return "high-liquidity";
        case Regime::HeavyMatching: return "heavy-matching";
        case Regime::HeavyCancellation: return "heavy-cancellation";
        case Regime::DeepBook: return "deep-book";
        case Regime::ShallowBook: return "shallow-book";
        case Regime::InvalidInput: return "invalid-input";
        case Regime::Mixed: return "mixed";
    }
    throw std::logic_error("unrecognized workload");
}

void save_failure(std::uint64_t seed, Regime regime, const std::vector<Event>& events,
                  const std::exception& error) {
    std::filesystem::create_directories("test-failures");
    const auto stem = std::string("test-failures/") + std::string(regime_name(regime)) + '-' + std::to_string(seed);
    std::ofstream metadata(stem + ".txt");
    metadata << "seed=" << seed << "\nregime=" << regime_name(regime) << "\nevent_count=" << events.size()
             << "\nerror=" << error.what() << "\n"
             << "Rerun: exchangecore_tests --filter \"reference: " << regime_name(regime) << "\"\n"
             << "Invalid-input artifacts intentionally preserve invalid tokens.\n";
    std::ofstream csv(stem + ".csv");
    csv << "type,id,side,quantity,price\n";
    for (const auto& event : events) {
        if (event.type == EventType::Cancel) {
            csv << "CANCEL," << event.order.id << ",,,\n";
        } else {
            csv << (event.order.type == OrderType::Market ? "MARKET" : "LIMIT") << ',' << event.order.id << ',';
            if (event.order.side == Side::Buy) csv << "BUY";
            else if (event.order.side == Side::Sell) csv << "SELL";
            else csv << "INVALID_SIDE_" << static_cast<unsigned>(event.order.side);
            csv << ',' << event.order.quantity << ',';
            if (event.order.type != OrderType::Market || event.order.price != 0) csv << event.order.price;
            csv << '\n';
        }
    }
    if (!metadata || !csv) throw std::runtime_error("could not persist failing reference replay");
}

Event random_event(std::mt19937_64& generator, Regime regime,
                   const ReferenceModel& reference, OrderId& next_id) {
    const auto choice = generator() % 100;
    const auto side = generator() % 2 == 0 ? Side::Buy : Side::Sell;
    const auto state = reference.snapshot();
    const auto active_count = state.bids.size() + state.asks.size();
    const auto cancel_percent = regime == Regime::HeavyCancellation ? 67U :
        (regime == Regime::LowLiquidity ? 30U : 15U);
    if (choice < cancel_percent) {
        if (active_count != 0 && generator() % 5 != 0) {
            const auto offset = static_cast<std::size_t>(generator() % active_count);
            return Event::cancel(offset < state.bids.size() ? state.bids[offset].id : state.asks[offset - state.bids.size()].id);
        }
        return Event::cancel(next_id + generator() % 50);
    }
    auto id = next_id++;
    if (generator() % 13 == 0 && id > 1) id = generator() % (id - 1) + 1;
    Quantity quantity = generator() % 20 + 1;
    Price price = 10000;
    const auto spread = regime == Regime::DeepBook ? 2000U : (regime == Regime::ShallowBook ? 2U : 25U);
    const auto offset = static_cast<Price>(generator() % spread + 1);
    const bool resting = regime == Regime::HighLiquidity || regime == Regime::DeepBook ||
        (regime == Regime::HeavyCancellation && generator() % 3 != 0);
    if (resting) price += side == Side::Buy ? -offset : offset;
    else price += static_cast<Price>(generator() % (2 * spread + 1)) - static_cast<Price>(spread);
    const auto market_percent = regime == Regime::HeavyMatching ? 70U :
        (regime == Regime::LowLiquidity ? 55U : (resting ? 2U : 15U));
    auto event = generator() % 100 < market_percent ? Event::market(id, side, quantity) : Event::limit(id, side, quantity, price);
    if (regime == Regime::HeavyMatching && event.order.type == OrderType::Limit) {
        event.order.price = side == Side::Buy ? 10040 : 9960;
    }
    if (regime == Regime::InvalidInput && generator() % 2 == 0) {
        switch (generator() % 5) {
            case 0: event.order.id = 0; break;
            case 1: event.order.quantity = 0; break;
            case 2: event.order.side = static_cast<Side>(3); break;
            case 3: event.order.price = -1; break;
            case 4: event.order.price = event.order.type == OrderType::Market ? 1 : 0; break;
        }
    }
    return event;
}

void differential(Regime regime) {
    constexpr std::array<std::uint64_t, 4> seeds{1, 42, 0xc0ffee, 0xdeadbeef1234ULL};
    for (const auto seed : seeds) {
        MatchingEngine engine(EngineConfig{0, 2000});
        ReferenceModel reference(EngineConfig{0, 2000});
        std::mt19937_64 generator(seed);
        OrderId next_id = 1;
        std::vector<Event> events;
        events.reserve(1500);
        try {
            for (std::size_t index = 0; index < 1500; ++index) {
                events.push_back(random_event(generator, regime, reference, next_id));
                const auto& event = events.back();
                const auto actual = engine.process(event);
                const auto expected = reference.process(event);
                CHECK(actual == expected);
                if (actual.accepted && event.type == EventType::New) {
                    CHECK(actual.filled <= event.order.quantity);
                    CHECK(actual.remaining == event.order.quantity - actual.filled);
                    Quantity trade_sum = 0;
                    for (const auto& trade : actual.trades) {
                        CHECK(trade.quantity > 0); CHECK(trade.price > 0);
                        CHECK(trade.taker_id == event.order.id); CHECK(trade.maker_id != trade.taker_id);
                        trade_sum += trade.quantity;
                    }
                    CHECK(trade_sum == actual.filled);
                }
                compare_state(engine, reference, event.order.id);
            }
        } catch (const std::exception& error) {
            save_failure(seed, regime, events, error);
            throw std::runtime_error(std::string(regime_name(regime)) + " seed=" + std::to_string(seed) +
                                     " at event=" + std::to_string(events.size()) + ": " + error.what());
        }
    }
}

TEST("reference: low-liquidity") { differential(Regime::LowLiquidity); }
TEST("reference: high-liquidity") { differential(Regime::HighLiquidity); }
TEST("reference: heavy-matching") { differential(Regime::HeavyMatching); }
TEST("reference: heavy-cancellation") { differential(Regime::HeavyCancellation); }
TEST("reference: deep-book") { differential(Regime::DeepBook); }
TEST("reference: shallow-book") { differential(Regime::ShallowBook); }
TEST("reference: invalid-input") { differential(Regime::InvalidInput); }
TEST("reference: mixed") { differential(Regime::Mixed); }

ParseResult parse(std::string_view text) {
    std::istringstream input{std::string(text)};
    return parse_csv(input);
}
constexpr std::string_view header = "type,id,side,quantity,price\n";
ParseResult body(std::string_view text) { return parse(std::string(header) + std::string(text)); }

TEST("CSV empty, blank, and comment-only files contain no events or errors") {
    for (const auto text : {"", "\n\r\n   \n", "# comment\n  # second\n"}) {
        const auto result = parse(text);
        CHECK(result.events.empty()); CHECK(result.errors.empty());
    }
}

TEST("CSV recognizes all event types and preserves source line numbers") {
    const auto result = parse("# comment\n\ntype,id,side,quantity,price\nLIMIT,1,BUY,5,24001\n\nMARKET,2,SELL,3,\nCANCEL,1,,,\n");
    CHECK(result.errors.empty()); CHECK(result.events.size() == 3);
    CHECK(result.events[0].line == 4); CHECK(result.events[1].line == 6); CHECK(result.events[2].line == 7);
    CHECK(result.events[0].event == Event::limit(1, Side::Buy, 5, 24001));
    CHECK(result.events[1].event == Event::market(2, Side::Sell, 3));
    CHECK(result.events[2].event == Event::cancel(1));
}

TEST("CSV trims field whitespace and handles CRLF and EOF without newline") {
    const auto result = parse(" type , id , side , quantity , price \r\n LIMIT , 1 , SELL , 5 , 101 \r\n CANCEL , 1 , , , ");
    CHECK(result.errors.empty()); CHECK(result.events.size() == 2);
    CHECK(result.events[0].event == Event::limit(1, Side::Sell, 5, 101));
    CHECK(result.events[1].event == Event::cancel(1));
}

TEST("CSV rejects missing or malformed headers") {
    for (const auto text : {"LIMIT,1,BUY,1,100\n", "id,type,side,quantity,price\n", "type,id,side,quantity\n",
                            "type,id,side,quantity,price,extra\n", "TYPE,id,side,quantity,price\n"}) {
        const auto result = parse(text);
        CHECK(!result.errors.empty()); CHECK(result.errors[0].line == 1);
        CHECK(result.events.empty()); CHECK(!result.errors[0].message.empty());
    }
}

TEST("CSV rejects missing fields, extra fields, and empty required fields") {
    for (const auto text : {"LIMIT,1,BUY,1\n", "LIMIT,1,BUY,1,100,x\n", "LIMIT,,BUY,1,100\n",
                            "LIMIT,1,,1,100\n", "LIMIT,1,BUY,,100\n", "LIMIT,1,BUY,1,\n"}) {
        const auto result = body(text);
        CHECK(result.events.empty()); CHECK(result.errors.size() == 1); CHECK(result.errors[0].line == 2);
    }
}

TEST("CSV rejects invalid enum tokens and unexpected irrelevant fields") {
    for (const auto text : {"NEW,1,BUY,1,100\n", "LIMIT,1,BID,1,100\n", "limit,1,BUY,1,100\n",
                            "MARKET,1,BUY,1,100\n", "CANCEL,1,BUY,,\n", "CANCEL,1,,1,\n", "CANCEL,1,,,100\n"}) {
        const auto result = body(text);
        CHECK(result.events.empty()); CHECK(result.errors.size() == 1);
    }
}

TEST("CSV numeric parsing rejects signs, decimals, junk, zero, and overflow") {
    const std::array<std::string_view, 19> rows{
        "LIMIT,0,BUY,1,100", "LIMIT,-1,BUY,1,100", "LIMIT,+1,BUY,1,100",
        "LIMIT,18446744073709551616,BUY,1,100", "LIMIT,1x,BUY,1,100",
        "LIMIT,1,BUY,0,100", "LIMIT,1,BUY,-1,100", "LIMIT,1,BUY,+1,100",
        "LIMIT,1,BUY,18446744073709551616,100", "LIMIT,1,BUY,1.5,100", "LIMIT,1,BUY,1x,100",
        "LIMIT,1,BUY,1,0", "LIMIT,1,BUY,1,-1", "LIMIT,1,BUY,1,+100",
        "LIMIT,1,BUY,1,9223372036854775808", "LIMIT,1,BUY,1,100.25", "LIMIT,1,BUY,1,100x",
        "MARKET,1,BUY,0,", "CANCEL,0,,,"};
    for (const auto row : rows) {
        const auto result = body(row);
        CHECK(result.events.empty()); CHECK(result.errors.size() == 1);
    }
}

TEST("CSV accepts full uint64 quantity and ID and int64 positive price boundaries") {
    const auto result = body("LIMIT,18446744073709551615,SELL,18446744073709551615,9223372036854775807");
    CHECK(result.errors.empty()); CHECK(result.events.size() == 1);
    CHECK(result.events[0].event == Event::limit(std::numeric_limits<OrderId>::max(), Side::Sell,
          std::numeric_limits<Quantity>::max(), std::numeric_limits<Price>::max()));
}

TEST("CSV continues after a bad row while preserving good rows and diagnostics") {
    const auto result = body("LIMIT,1,BUY,3,99\nBAD,2,SELL,1,100\nMARKET,3,SELL,1,\n");
    CHECK(result.errors.size() == 1); CHECK(result.errors[0].line == 3);
    CHECK(!result.errors[0].message.empty()); CHECK(result.events.size() == 2);
    CHECK(result.events[0].line == 2); CHECK(result.events[1].line == 4);
}

TEST("CSV leaves duplicate IDs and unknown cancellations to semantic validation") {
    const auto result = body("LIMIT,1,BUY,3,99\nLIMIT,1,BUY,3,99\nCANCEL,99,,,\n");
    CHECK(result.errors.empty()); CHECK(result.events.size() == 3);
    MatchingEngine engine;
    accept(engine, result.events[0].event);
    reject(engine, result.events[1].event, RejectReason::DuplicateId);
    reject(engine, result.events[2].event, RejectReason::UnknownOrder);
}

TEST("CSV parsing and replay is deterministic including semantic rejections") {
    constexpr std::string_view data = "type,id,side,quantity,price\nLIMIT,1,SELL,5,101\nLIMIT,2,BUY,3,102\n"
        "MARKET,3,BUY,4,\nCANCEL,1,,,\nLIMIT,2,BUY,1,99\n";
    const auto first = parse(data), second = parse(data);
    CHECK(first.errors.empty()); CHECK(second.errors.empty()); CHECK(first.events.size() == second.events.size());
    MatchingEngine a, b;
    for (std::size_t i = 0; i < first.events.size(); ++i) {
        CHECK(first.events[i].event == second.events[i].event);
        CHECK(a.process(first.events[i].event) == b.process(second.events[i].event));
    }
    CHECK(a.snapshot() == b.snapshot()); CHECK(a.active_order_count() == 0);
}

TEST("CSV caps error accumulation and rejects oversized lines") {
    std::string input(header);
    input += std::string(5000, 'x') + '\n';
    for (int i = 0; i < 150; ++i) input += "BAD,1,BUY,1,100\n";
    const auto result = parse(input);
    CHECK(result.events.empty()); CHECK(!result.errors.empty()); CHECK(result.errors.size() <= 100);
    CHECK(result.errors.front().line == 2);
}

class ReadFailureBuffer final : public std::stringbuf {
public:
    explicit ReadFailureBuffer(std::string data) : std::stringbuf(std::move(data), std::ios::in) {}

protected:
    int_type underflow() override { throw std::runtime_error("simulated input read failure"); }
};

TEST("CSV reports a prefailed stream instead of successful empty input") {
    std::istringstream input(std::string(header) + "LIMIT,1,BUY,3,99\n");
    input.setstate(std::ios::failbit);
    const auto result = parse_csv(input);
    CHECK(result.events.empty()); CHECK(result.errors.size() == 1);
    CHECK(result.errors.front().line == 1);
    CHECK(result.errors.front().message == "input read failure");
}

TEST("CSV locates input read failures on partial and new physical lines") {
    for (const bool newline : {false, true}) {
        ReadFailureBuffer buffer(std::string(header) + "LIMIT,1,BUY,3,99" + (newline ? "\n" : ""));
        std::istream input(&buffer);
        const auto result = parse_csv(input);
        CHECK(input.bad()); CHECK(result.errors.size() == 1);
        CHECK(result.errors.front().line == (newline ? 3U : 2U));
        CHECK(result.errors.front().message == "input read failure");
    }
}

TEST("CSV preserves the diagnostic cap when the final malformed line has a read failure") {
    std::string data(header);
    for (int i = 0; i < 99; ++i) data += "BAD,1,BUY,1,100\n";
    data += "BAD,1,BUY,1,100";
    ReadFailureBuffer buffer(std::move(data));
    std::istream input(&buffer);
    const auto result = parse_csv(input);
    CHECK(input.bad()); CHECK(result.events.empty()); CHECK(result.errors.size() == 100);
    CHECK(result.errors.front().line == 2); CHECK(result.errors.back().line == 101);
}

TEST("price formatting uses exact quarter points including signed boundaries") {
    CHECK(format_price(24001) == "6000.25"); CHECK(format_price(24000) == "6000.00");
    CHECK(format_price(1) == "0.25"); CHECK(format_price(2) == "0.50"); CHECK(format_price(3) == "0.75");
    CHECK(format_price(0) == "0.00"); CHECK(format_price(-1) == "-0.25"); CHECK(format_price(-4) == "-1.00");
    CHECK(format_price(std::numeric_limits<Price>::min()) == "-2305843009213693952.00");
    CHECK(format_price(std::numeric_limits<Price>::max()) == "2305843009213693951.75");
}

} // namespace

int main(int argc, char** argv) {
    std::string_view filter;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--filter" && i + 1 < argc) filter = argv[++i];
        else if (arg == "--list") list = true;
        else {
            std::cerr << "Usage: exchangecore_tests [--list] [--filter substring]\n";
            return 2;
        }
    }
    std::size_t passed = 0, failed = 0, selected = 0;
    for (const auto& test : cases()) {
        if (test.name.find(filter) == std::string_view::npos) continue;
        ++selected;
        if (list) { std::cout << test.name << '\n'; continue; }
        try {
            test.run(); ++passed; std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failed; std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        } catch (...) {
            ++failed; std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
        }
    }
    if (selected == 0) { std::cerr << "No matching tests\n"; return 2; }
    if (!list) std::cout << passed << " passed, " << failed << " failed (" << selected << " tests)\n";
    return failed == 0 ? 0 : 1;
}
