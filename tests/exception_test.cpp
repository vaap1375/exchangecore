#include "exchangecore/matching_engine.hpp"

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Keep global allocation replacement in its own executable: the normal tests
// and benchmarks retain the system allocation functions and their usual costs.
namespace fault {
thread_local bool enabled = false;
thread_local std::size_t countdown = 0;

void before_allocation() {
    if (enabled) {
        if (countdown == 0) throw std::bad_alloc{};
        --countdown;
    }
}
void* allocate(std::size_t size) {
    before_allocation();
    if (void* result = std::malloc(size == 0 ? 1 : size)) return result;
    throw std::bad_alloc{};
}
void* allocate_aligned(std::size_t size, std::size_t alignment) {
    before_allocation();
    void* result = nullptr;
    if (posix_memalign(&result, alignment, size == 0 ? 1 : size) == 0) return result;
    throw std::bad_alloc{};
}
} // namespace fault

void* operator new(std::size_t size) { return fault::allocate(size); }
void* operator new[](std::size_t size) { return fault::allocate(size); }
void* operator new(std::size_t size, std::align_val_t alignment) {
    return fault::allocate_aligned(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return fault::allocate_aligned(size, static_cast<std::size_t>(alignment));
}
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::align_val_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept { std::free(pointer); }

namespace {
using namespace exchangecore;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

struct Scenario {
    std::string_view name;
    std::vector<Event> fixture;
    Event target;
    EngineConfig config{};
};

void populate(MatchingEngine& engine, const std::vector<Event>& events) {
    for (const auto& event : events) require(engine.process(event).accepted, "fixture must be accepted");
}

std::size_t test_transaction(const Scenario& scenario) {
    MatchingEngine expected(scenario.config);
    populate(expected, scenario.fixture);
    const auto expected_result = expected.process(scenario.target);
    require(expected_result.accepted, "target operation must be accepted without allocation failure");
    const auto expected_snapshot = expected.snapshot();
    std::size_t failure_count = 0;
    bool reached_success = false;
    for (std::size_t fail_at = 0; fail_at < 128; ++fail_at) {
        MatchingEngine engine(scenario.config);
        populate(engine, scenario.fixture);
        const auto before = engine.snapshot();
        const auto accepted_before = engine.accepted_order_count();
        const auto active_before = engine.active_order_count();
        const auto quantity_before = engine.resting_quantity();
        const auto bid_before = engine.best_bid();
        const auto ask_before = engine.best_ask();
        Result result;
        bool failed = false;
        fault::countdown = fail_at;
        fault::enabled = true;
        try {
            result = engine.process(scenario.target);
            fault::enabled = false;
        } catch (const std::bad_alloc&) {
            fault::enabled = false;
            failed = true;
        } catch (...) {
            fault::enabled = false;
            throw;
        }
        if (failed) {
            ++failure_count;
            require(engine.snapshot() == before, "bad_alloc changed resting orders or arrival priority");
            require(engine.accepted_order_count() == accepted_before, "bad_alloc consumed accepted ID capacity");
            require(engine.active_order_count() == active_before, "bad_alloc changed active order count");
            require(engine.resting_quantity() == quantity_before, "bad_alloc changed resting quantity");
            require(engine.best_bid() == bid_before && engine.best_ask() == ask_before, "bad_alloc changed best prices");
            for (const auto& order : before.bids) require(engine.find(order.id) == order, "bad_alloc damaged buy lookup");
            for (const auto& order : before.asks) require(engine.find(order.id) == order, "bad_alloc damaged sell lookup");
            engine.check_invariants();
            // Retrying the same ID also checks that both ID lifetime and the
            // sequence counter are rolled back, not only visible quantities.
            result = engine.process(scenario.target);
        } else {
            reached_success = true;
        }
        require(result == expected_result, "successful operation differs after fault/retry");
        require(engine.snapshot() == expected_snapshot, "successful final state differs after fault/retry");
        require(engine.accepted_order_count() == expected.accepted_order_count(), "post-retry accepted count differs");
        engine.check_invariants();
        if (reached_success) break;
    }
    require(reached_success, "allocation fault bound exhausted without successful operation");
    require(failure_count != 0, "scenario did not exercise any allocation site");
    std::cout << "[PASS] " << scenario.name << " (" << failure_count << " allocation failure points)\n";
    return failure_count;
}

std::size_t test_constructor() {
    std::size_t failures = 0;
    bool reached_success = false;
    for (std::size_t fail_at = 0; fail_at < 32; ++fail_at) {
        fault::countdown = fail_at;
        fault::enabled = true;
        try {
            MatchingEngine engine(EngineConfig{128, 256});
            fault::enabled = false;
            require(engine.snapshot() == Snapshot{}, "constructor must create empty book");
            require(engine.accepted_order_count() == 0, "constructor must create empty accepted ID registry");
            engine.check_invariants();
            reached_success = true;
        } catch (const std::bad_alloc&) {
            fault::enabled = false;
            ++failures;
            // Constructor RAII cleanup is additionally checked under ASan.
            MatchingEngine retry(EngineConfig{128, 256});
            require(retry.submit_limit(1, Side::Buy, 1, 100).accepted, "constructor retry failed");
            retry.check_invariants();
        } catch (...) {
            fault::enabled = false;
            throw;
        }
        if (reached_success) break;
    }
    require(reached_success && failures != 0, "constructor fault coverage incomplete");
    std::cout << "[PASS] reserved constructor (" << failures << " allocation failure points)\n";
    return failures;
}
} // namespace

int main() {
    try {
        std::size_t failure_points = test_constructor();
        const std::vector<Scenario> scenarios{
            {"insert first price level", {}, Event::limit(1, Side::Buy, 7, 100)},
            {"append to existing FIFO level", {Event::limit(1, Side::Sell, 7, 100)}, Event::limit(2, Side::Sell, 9, 100)},
            {"partial maker fill", {Event::limit(1, Side::Sell, 10, 100)}, Event::market(2, Side::Buy, 3)},
            {"buy sweep with resting residual", {Event::limit(1, Side::Sell, 2, 100),
                Event::limit(2, Side::Sell, 3, 101), Event::limit(3, Side::Sell, 4, 102)},
                Event::limit(4, Side::Buy, 12, 103)},
            {"sell sweep with resting residual", {Event::limit(1, Side::Buy, 2, 102),
                Event::limit(2, Side::Buy, 3, 101), Event::limit(3, Side::Buy, 4, 100)},
                Event::limit(4, Side::Sell, 12, 99)},
            {"market sweep across FIFO and price levels", {Event::limit(1, Side::Sell, 2, 100),
                Event::limit(2, Side::Sell, 2, 100), Event::limit(3, Side::Sell, 3, 101),
                Event::limit(4, Side::Sell, 4, 102)}, Event::market(5, Side::Buy, 11)},
        };
        for (const auto& scenario : scenarios) failure_points += test_transaction(scenario);
        std::cout << "7 scenarios passed, " << failure_points << " allocation failure points verified\n";
        return 0;
    } catch (const std::exception& error) {
        fault::enabled = false;
        std::cerr << "[FAIL] allocation exception guarantee: " << error.what() << '\n';
        return 1;
    }
}
