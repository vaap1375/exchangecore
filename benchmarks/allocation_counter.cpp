// Only the standalone benchmark links these replacements. Normal library and
// CLI consumers keep their allocator untouched. Counters are per-thread and
// enabled only in a separate, untimed allocation replay.
#include "allocation_counter.hpp"

#include <cstdlib>
#include <new>

namespace {
thread_local bool counting = false;
thread_local exchangecore::AllocationStats counters;

void account(std::size_t bytes) noexcept {
    if (counting) {
        ++counters.calls;
        counters.requested_bytes += bytes;
    }
}

void* allocate(std::size_t bytes) {
    for (;;) {
        if (void* pointer = std::malloc(bytes == 0 ? 1 : bytes)) {
            account(bytes);
            return pointer;
        }
        const auto handler = std::get_new_handler();
        if (handler == nullptr) throw std::bad_alloc{};
        handler();
    }
}

void* allocate_aligned(std::size_t bytes, std::size_t alignment) {
    for (;;) {
        void* pointer = nullptr;
        if (posix_memalign(&pointer, alignment, bytes == 0 ? 1 : bytes) == 0) {
            account(bytes);
            return pointer;
        }
        const auto handler = std::get_new_handler();
        if (handler == nullptr) throw std::bad_alloc{};
        handler();
    }
}

void start() noexcept {
    counters = {};
    counting = true;
}

exchangecore::AllocationStats stop() noexcept {
    counting = false;
    return counters;
}
}  // namespace

void* operator new(std::size_t bytes) { return allocate(bytes); }
void* operator new[](std::size_t bytes) { return allocate(bytes); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }

void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    try { return allocate(bytes); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
    try { return allocate(bytes); } catch (...) { return nullptr; }
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }

void* operator new(std::size_t bytes, std::align_val_t alignment) {
    return allocate_aligned(bytes, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t bytes, std::align_val_t alignment) {
    return allocate_aligned(bytes, static_cast<std::size_t>(alignment));
}
void operator delete(void* pointer, std::align_val_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { std::free(pointer); }
void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return allocate_aligned(bytes, static_cast<std::size_t>(alignment)); }
    catch (...) { return nullptr; }
}
void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return allocate_aligned(bytes, static_cast<std::size_t>(alignment)); }
    catch (...) { return nullptr; }
}
void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
    std::free(pointer);
}

#if defined(__cpp_sized_deallocation)
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept { std::free(pointer); }
#endif

namespace exchangecore {
AllocationProbe standalone_allocation_probe() noexcept { return {start, stop}; }
}
