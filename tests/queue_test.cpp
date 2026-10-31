#include "exchangecore/bounded_queue.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <latch>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using exchangecore::BoundedQueue;
struct TestCase { std::string_view name; void (*run)(); };
std::vector<TestCase>& cases() { static std::vector<TestCase> result; return result; }
struct Register { Register(std::string_view name, void (*run)()) { cases().push_back({name, run}); } };
void check(bool condition, const char* expression, int line) {
    if (!condition) throw std::runtime_error("line " + std::to_string(line) + ": " + expression);
}
#define JOIN_IMPL(a, b) a##b
#define JOIN(a, b) JOIN_IMPL(a, b)
#define TEST(name) \
    void JOIN(test_, __LINE__)(); \
    const Register JOIN(register_, __LINE__)(name, JOIN(test_, __LINE__)); \
    void JOIN(test_, __LINE__)()
#define CHECK(...) check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

TEST("queue requires positive capacity") {
    bool rejected = false;
    try { BoundedQueue<int> queue(0); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
}

TEST("queue empty and saturated operations preserve output and bounded size") {
    BoundedQueue<int> queue(2);
    int output = 91;
    CHECK(queue.capacity() == 2); CHECK(queue.size() == 0); CHECK(queue.high_watermark() == 0);
    CHECK(!queue.try_pop(output)); CHECK(output == 91);
    CHECK(queue.try_push(1)); CHECK(queue.try_push(2)); CHECK(!queue.try_push(3));
    CHECK(queue.size() == 2); CHECK(queue.high_watermark() == 2);
    CHECK(queue.try_pop(output)); CHECK(output == 1);
    CHECK(queue.try_pop(output)); CHECK(output == 2);
    CHECK(queue.size() == 0); CHECK(queue.high_watermark() == 2);
}

TEST("queue ring wraparound preserves exact FIFO order") {
    BoundedQueue<int> queue(3);
    CHECK(queue.try_push(0)); CHECK(queue.try_push(1)); CHECK(queue.try_push(2));
    int output = -1;
    for (int value = 0; value < 2000; ++value) {
        CHECK(queue.try_pop(output)); CHECK(output == value);
        CHECK(queue.try_push(value + 3));
        CHECK(queue.size() == 3);
    }
    for (int value = 2000; value < 2003; ++value) { CHECK(queue.try_pop(output)); CHECK(output == value); }
    CHECK(queue.size() == 0); CHECK(queue.high_watermark() == 3);
}

TEST("queue supports move-only payloads and failed pushes retain producer ownership") {
    BoundedQueue<std::unique_ptr<int>> queue(1);
    auto first = std::make_unique<int>(1), second = std::make_unique<int>(2);
    CHECK(queue.try_push(std::move(first))); CHECK(!first);
    CHECK(!queue.try_push(std::move(second))); CHECK(second && *second == 2);
    std::unique_ptr<int> received;
    CHECK(queue.try_pop(received)); CHECK(received && *received == 1);
    CHECK(queue.try_push(std::move(second))); CHECK(!second);
    CHECK(queue.try_pop(received)); CHECK(received && *received == 2);
    queue.close();
    auto third = std::make_unique<int>(3);
    CHECK(!queue.wait_push(std::move(third))); CHECK(third && *third == 3);
}

TEST("queue close is idempotent and empty consumers stop without changing output") {
    BoundedQueue<int> queue(2);
    queue.close(); queue.close();
    CHECK(queue.closed()); CHECK(queue.size() == 0);
    CHECK(!queue.try_push(1)); CHECK(!queue.wait_push(2));
    int output = 91;
    CHECK(!queue.try_pop(output)); CHECK(output == 91);
    CHECK(!queue.wait_pop(output)); CHECK(output == 91);
}

TEST("queue close drains every admitted value in FIFO order and rejects new values") {
    BoundedQueue<int> queue(3);
    CHECK(queue.try_push(1)); CHECK(queue.try_push(2)); CHECK(queue.try_push(3));
    queue.close();
    CHECK(!queue.try_push(4)); CHECK(!queue.wait_push(5));
    int output = 0;
    for (int expected = 1; expected <= 3; ++expected) { CHECK(queue.wait_pop(output)); CHECK(output == expected); }
    CHECK(!queue.wait_pop(output)); CHECK(queue.size() == 0);
}

TEST("queue waiting consumer makes progress when a producer admits an item") {
    BoundedQueue<int> queue(1);
    std::latch started(1);
    int received = 0;
    bool success = false;
    std::jthread consumer([&] { started.count_down(); success = queue.wait_pop(received); });
    started.wait();
    const bool pushed = queue.try_push(17);
    if (!pushed) queue.close();
    consumer.join();
    CHECK(pushed); CHECK(success); CHECK(received == 17);
}

TEST("queue waiting producer makes progress when a consumer frees capacity") {
    BoundedQueue<int> queue(1);
    CHECK(queue.try_push(11));
    std::latch started(1);
    bool success = false;
    std::jthread producer([&] { started.count_down(); success = queue.wait_push(12); });
    started.wait();
    int received = 0;
    const bool popped = queue.try_pop(received);
    if (!popped) queue.close();
    producer.join();
    CHECK(popped); CHECK(received == 11); CHECK(success);
    CHECK(queue.try_pop(received)); CHECK(received == 12);
}

TEST("queue close wakes all consumers waiting on an empty queue") {
    constexpr std::size_t count = 8;
    BoundedQueue<int> queue(2);
    std::latch started(count);
    std::array<bool, count> success{};
    std::array<int, count> received{};
    std::vector<std::jthread> consumers;
    consumers.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        consumers.emplace_back([&, i] { received[i] = 91; started.count_down(); success[i] = queue.wait_pop(received[i]); });
    }
    started.wait(); queue.close();
    for (auto& consumer : consumers) consumer.join();
    for (std::size_t i = 0; i < count; ++i) { CHECK(!success[i]); CHECK(received[i] == 91); }
}

TEST("queue close wakes all saturated producers and retains the admitted item") {
    constexpr std::size_t count = 8;
    BoundedQueue<int> queue(1);
    CHECK(queue.try_push(91));
    std::latch started(count);
    std::array<bool, count> success{};
    std::vector<std::jthread> producers;
    producers.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        producers.emplace_back([&, i] { started.count_down(); success[i] = queue.wait_push(static_cast<int>(i)); });
    }
    started.wait(); queue.close();
    for (auto& producer : producers) producer.join();
    for (const auto accepted : success) CHECK(!accepted);
    int received = 0;
    CHECK(queue.wait_pop(received)); CHECK(received == 91); CHECK(!queue.wait_pop(received));
}

TEST("queue concurrent producers preserve every admitted value and each producer's order") {
    constexpr std::size_t producer_count = 4, per_producer = 1000, total = producer_count * per_producer;
    BoundedQueue<std::uint64_t> queue(7);
    std::latch start(producer_count + 1);
    std::atomic<bool> push_failed{false};
    std::vector<std::uint64_t> observed;
    observed.reserve(total);
    std::jthread consumer([&] {
        start.count_down(); start.wait();
        std::uint64_t value = 0;
        while (queue.wait_pop(value)) observed.push_back(value);
    });
    std::vector<std::jthread> producers;
    producers.reserve(producer_count);
    for (std::size_t producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer] {
            start.count_down(); start.wait();
            for (std::size_t index = 0; index < per_producer; ++index) {
                if (!queue.wait_push(static_cast<std::uint64_t>(producer * per_producer + index))) {
                    push_failed = true; break;
                }
            }
        });
    }
    for (auto& producer : producers) producer.join();
    queue.close(); consumer.join();
    CHECK(!push_failed); CHECK(observed.size() == total);
    std::array<std::size_t, producer_count> next{};
    for (const auto value : observed) {
        const auto producer = static_cast<std::size_t>(value / per_producer);
        const auto index = static_cast<std::size_t>(value % per_producer);
        CHECK(producer < producer_count); CHECK(index == next[producer]); ++next[producer];
    }
    for (const auto count : next) CHECK(count == per_producer);
    CHECK(queue.size() == 0); CHECK(queue.high_watermark() > 0); CHECK(queue.high_watermark() <= queue.capacity());
}

TEST("queue multiple producers and consumers deliver each value exactly once") {
    constexpr std::size_t producers_count = 4, consumers_count = 4, per_producer = 1000;
    constexpr std::size_t total = producers_count * per_producer;
    BoundedQueue<std::uint64_t> queue(3);
    std::latch start(producers_count + consumers_count);
    std::atomic<bool> push_failed{false};
    std::array<std::vector<std::uint64_t>, consumers_count> observed;
    for (auto& values : observed) values.reserve(total);
    std::vector<std::jthread> producers, consumers;
    producers.reserve(producers_count); consumers.reserve(consumers_count);
    for (std::size_t i = 0; i < consumers_count; ++i) {
        consumers.emplace_back([&, i] {
            start.count_down(); start.wait();
            std::uint64_t value = 0;
            while (queue.wait_pop(value)) observed[i].push_back(value);
        });
    }
    for (std::size_t i = 0; i < producers_count; ++i) {
        producers.emplace_back([&, i] {
            start.count_down(); start.wait();
            for (std::size_t j = 0; j < per_producer; ++j) {
                if (!queue.wait_push(static_cast<std::uint64_t>(i * per_producer + j))) { push_failed = true; break; }
            }
        });
    }
    for (auto& producer : producers) producer.join();
    queue.close();
    for (auto& consumer : consumers) consumer.join();
    CHECK(!push_failed);
    std::array<unsigned, total> occurrences{};
    std::size_t received = 0;
    for (const auto& values : observed) {
        for (const auto value : values) { CHECK(value < total); ++occurrences[static_cast<std::size_t>(value)]; ++received; }
    }
    CHECK(received == total);
    for (const auto count : occurrences) CHECK(count == 1);
    CHECK(queue.size() == 0); CHECK(queue.high_watermark() <= 3);
}

} // namespace

int main() {
    std::size_t passed = 0, failed = 0;
    for (const auto& test : cases()) {
        try { test.run(); ++passed; std::cout << "[PASS] " << test.name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n'; }
    }
    std::cout << passed << " queue tests passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
