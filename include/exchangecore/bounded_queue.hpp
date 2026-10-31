#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace exchangecore {

// Fixed-storage FIFO. close() rejects producers, wakes every waiter, and lets
// consumers drain already admitted values. No caller lock spans processing.
template<class T> class BoundedQueue {
    static_assert(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>,
                  "bounded FIFO values must support noexcept move construction and assignment");
public:
    explicit BoundedQueue(std::size_t capacity) : slots_(capacity) {
        if (capacity == 0) throw std::invalid_argument("queue capacity must be positive");
    }
    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    bool try_push(T&& value) {
        std::lock_guard lock(mutex_);
        if (closed_ || size_ == slots_.size()) return false;
        push_locked(std::move(value));
        return true;
    }
    bool wait_push(T&& value) {
        std::unique_lock lock(mutex_);
        writable_.wait(lock, [&] { return closed_ || size_ < slots_.size(); });
        if (closed_) return false;
        push_locked(std::move(value));
        return true;
    }
    bool try_pop(T& value) {
        std::lock_guard lock(mutex_);
        if (size_ == 0) return false;
        pop_locked(value);
        return true;
    }
    bool wait_pop(T& value) {
        std::unique_lock lock(mutex_);
        readable_.wait(lock, [&] { return closed_ || size_ != 0; });
        if (size_ == 0) return false;
        pop_locked(value);
        return true;
    }
    void close() {
        std::lock_guard lock(mutex_);
        closed_ = true;
        readable_.notify_all();
        writable_.notify_all();
    }
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
    [[nodiscard]] std::size_t size() const { std::lock_guard lock(mutex_); return size_; }
    [[nodiscard]] std::size_t high_watermark() const {
        std::lock_guard lock(mutex_); return high_watermark_;
    }
    [[nodiscard]] bool closed() const { std::lock_guard lock(mutex_); return closed_; }

private:
    void push_locked(T&& value) {
        slots_[(head_ + size_) % slots_.size()].emplace(std::move(value));
        ++size_;
        if (size_ > high_watermark_) high_watermark_ = size_;
        readable_.notify_one();
    }
    void pop_locked(T& value) {
        value = std::move(*slots_[head_]);
        slots_[head_].reset();
        head_ = (head_ + 1) % slots_.size();
        --size_;
        writable_.notify_one();
    }
    std::vector<std::optional<T>> slots_;
    mutable std::mutex mutex_;
    std::condition_variable readable_;
    std::condition_variable writable_;
    std::size_t head_{};
    std::size_t size_{};
    std::size_t high_watermark_{};
    bool closed_{};
};

}  // namespace exchangecore
