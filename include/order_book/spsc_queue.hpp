#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include "order_book/cache.hpp"

namespace ob {

// Single-Producer Single-Consumer wait-free bounded ring buffer.
//
// Design follows rigtorp/SPSCQueue and the LMAX Disruptor pattern referenced
// in arXiv:2309.04259 — the producer and consumer indices live on separate
// cache lines, and each side keeps a *cached* copy of the other's index so
// the common case doesn't touch the remote cache line at all.
//
// Capacity is rounded up to a power of two so we can mask instead of mod.
template <class T>
class SPSCQueue {
    static_assert(std::is_nothrow_destructible_v<T>);

public:
    explicit SPSCQueue(std::size_t min_capacity) {
        std::size_t cap = 1;
        while (cap < min_capacity) cap <<= 1;
        mask_ = cap - 1;
        slots_.resize(cap);
    }

    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    // Producer side. Returns false if queue is full.
    template <class U>
    [[nodiscard]] bool try_push(U&& v) noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next = head + 1;

        // Fast path: use cached tail to avoid loading the remote atomic.
        if (next - cached_tail_ > slots_.size()) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (OB_UNLIKELY(next - cached_tail_ > slots_.size())) return false;
        }

        slots_[head & mask_].value = std::forward<U>(v);
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false if queue is empty.
    [[nodiscard]] bool try_pop(T& out) noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);

        if (tail == cached_head_) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (OB_UNLIKELY(tail == cached_head_)) return false;
        }

        out = std::move(slots_[tail & mask_].value);
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    std::size_t capacity() const noexcept { return slots_.size(); }

private:
    struct Slot {
        T value{};
    };

    std::size_t mask_{};

    // Producer's writable indices on their own cache line.
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    alignas(kCacheLine) std::size_t cached_tail_{0};

    // Consumer's writable indices on their own cache line.
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
    alignas(kCacheLine) std::size_t cached_head_{0};

    // Slots last — large, accessed by both sides.
    std::vector<Slot> slots_;
};

}  // namespace ob
