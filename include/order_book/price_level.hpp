#pragma once

#include "order_book/cache.hpp"
#include "order_book/types.hpp"

namespace ob {

struct alignas(kCacheLine) PriceLevel {
    Price    price{0};
    Quantity total_qty{0};
    Order*   head{nullptr};
    Order*   tail{nullptr};

    OB_ALWAYS_INLINE bool empty() const noexcept { return head == nullptr; }

    OB_ALWAYS_INLINE void push_back(Order* o) noexcept {
        o->prev = tail;
        o->next = nullptr;
        if (tail) tail->next = o;
        else      head = o;
        tail = o;
        total_qty += o->qty;
    }

    OB_ALWAYS_INLINE void unlink(Order* o) noexcept {
        if (o->prev) o->prev->next = o->next;
        else         head = o->next;
        if (o->next) o->next->prev = o->prev;
        else         tail = o->prev;
        total_qty -= o->qty;
        o->prev = o->next = nullptr;
    }
};

}  // namespace ob
