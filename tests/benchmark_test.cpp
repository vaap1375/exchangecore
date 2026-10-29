#include "exchangecore/benchmark.hpp"
#include "exchangecore/matching_engine.hpp"
#include "reference_model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace exchangecore;
using exchangecore::test::ReferenceModel;
constexpr std::array<std::string_view, 6> names{"resting", "crossing", "cancellation", "deep", "touch", "mixed"};

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

struct Activity {
    std::size_t trades{};
    std::size_t cancellations{};
    Quantity input_quantity{};
};

Activity verify(std::string_view name, std::size_t count, std::uint64_t seed) {
    const auto events = make_benchmark_workload(name, count, seed);
    require(events.size() == count, "generator returned wrong event count");
    require(events == make_benchmark_workload(name, count, seed), "same seed generated a different event sequence");
    MatchingEngine engine;
    ReferenceModel reference;
    Activity activity;
    for (const auto& event : events) {
        const auto actual = engine.process(event);
        const auto expected = reference.process(event);
        require(actual == expected, "benchmark event differs from independent reference result");
        require(actual.accepted, "benchmark generator emitted a rejected event");
        activity.trades += actual.trades.size();
        activity.cancellations += static_cast<std::size_t>(actual.canceled != 0);
        if (event.type == EventType::New) activity.input_quantity += event.order.quantity;
        require(engine.snapshot() == reference.snapshot(), "benchmark state differs from independent reference");
        require(engine.best_bid() == reference.best_bid(), "benchmark best bid differs from reference");
        require(engine.best_ask() == reference.best_ask(), "benchmark best ask differs from reference");
        require(engine.resting_quantity() == reference.resting_quantity(), "benchmark aggregate differs from reference");
        engine.check_invariants();
    }
    if (name == "resting" || name == "deep") {
        require(activity.trades == 0 && activity.cancellations == 0, "resting workloads performed destructive events");
        require(engine.active_order_count() == count, "resting workload lost an order");
        require(engine.resting_quantity() == activity.input_quantity, "resting workload did not conserve input quantity");
    }
    if (name == "deep") {
        require(engine.depth(Side::Buy).size() + engine.depth(Side::Sell).size() == count,
                "deep workload must create a distinct level for each order");
    }
    if (name == "cancellation") {
        require(activity.trades == 0, "cancellation workload unexpectedly matched");
        require(engine.active_order_count() <= 2, "cancellation workload left excess active orders");
    }
    if (count == 120) {
        if (name == "crossing") {
            require(activity.trades == 80 && activity.cancellations == 0, "crossing workload activity is wrong");
        } else if (name == "cancellation") {
            require(activity.cancellations == 60, "cancellation workload activity is wrong");
        } else if (name == "touch") {
            require(activity.trades == 40 && activity.cancellations == 20, "touch workload activity is wrong");
        } else if (name == "mixed") {
            require(activity.trades == 36 && activity.cancellations == 24, "mixed workload activity is wrong");
        }
        if (name != "resting" && name != "deep") {
            require(engine.active_order_count() == 0 && engine.resting_quantity() == 0,
                    "complete generated blocks must leave an empty book");
        }
    }
    return activity;
}

void test_boundaries() {
    constexpr std::array<std::uint64_t, 3> seeds{1, 42, 60448};
    std::size_t combinations = 0;
    for (const auto name : names) {
        for (const auto seed : seeds) {
            for (std::size_t count = 0; count <= 20; ++count) { (void)verify(name, count, seed); ++combinations; }
            for (std::size_t count = 64; count <= 75; ++count) { (void)verify(name, count, seed); ++combinations; }
        }
    }
    require(combinations == 594, "boundary test matrix is incomplete");
    std::cout << "[PASS] all six generators: 594 seed/count boundaries agree with independent reference\n";
}

void test_activity() {
    for (const auto name : names) (void)verify(name, 120, 60448);
    std::cout << "[PASS] complete blocks have expected matching, cancellation, depth, and quantity behavior\n";
}

void test_seed_sensitivity() {
    for (const auto name : names) {
        require(make_benchmark_workload(name, 120, 1) != make_benchmark_workload(name, 120, 2),
                "different seeds must change a nontrivial workload");
    }
    std::cout << "[PASS] all six nontrivial generators respond to the supplied seed\n";
}

void test_invalid_arguments() {
    for (const auto name : {std::string_view{}, std::string_view{"unknown"}, std::string_view{"all"}}) {
        bool rejected = false;
        try { (void)make_benchmark_workload(name, 64, 1); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "unknown concrete workload must be rejected");
    }
    bool rejected = false;
    try { (void)make_benchmark_workload("resting", 5'000'001, 1); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "oversized workload must be rejected before allocation");
    std::cout << "[PASS] generator rejects unknown names and allocation-limit overflow\n";
}
} // namespace

int main() {
    try {
        test_boundaries(); test_activity(); test_seed_sensitivity(); test_invalid_arguments();
        std::cout << "4 benchmark generator tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] benchmark generator: " << error.what() << '\n';
        return 1;
    }
}
