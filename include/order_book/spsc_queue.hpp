#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

#include "order_book/cache.hpp"

namespace ob {

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

    template <class U>
    [[nodiscard]] bool try_push(U&& v) noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next = head + 1;

        if (next - cached_tail_ > slots_.size()) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (OB_UNLIKELY(next - cached_tail_ > slots_.size())) return false;
        }

        slots_[head & mask_].value = std::forward<U>(v);
        head_.store(next, std::memory_order_release);
        return true;
    }

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

    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    alignas(kCacheLine) std::size_t cached_tail_{0};

    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
    alignas(kCacheLine) std::size_t cached_head_{0};

    std::vector<Slot> slots_;
};

}  // namespace ob
