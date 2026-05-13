#pragma once

#include <cstdint>
#include <limits>

#include "order_book/cache.hpp"

namespace ob {

using OrderId   = std::uint64_t;
using Price     = std::int64_t;   // fixed-point ticks (e.g. 1 tick = 0.01)
using Quantity  = std::uint64_t;
using Timestamp = std::uint64_t;

inline constexpr OrderId kInvalidOrderId = std::numeric_limits<OrderId>::max();

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

enum class OrderType : std::uint8_t {
    Limit,
    Market,
    IOC,   // Immediate-or-Cancel
    FOK,   // Fill-or-Kill
};

enum class FillKind : std::uint8_t { Partial, Full };

struct Trade {
    OrderId  maker_id;
    OrderId  taker_id;
    Price    price;
    Quantity qty;
    Timestamp ts;
};

// 64-byte cache-line-sized order. Intrusive list links live here so we don't
// allocate a separate list node per order.
struct alignas(kCacheLine) Order {
    OrderId   id;
    Price     price;
    Quantity  qty;          // remaining quantity
    Timestamp ts;
    Side      side;
    OrderType type;

    // Intrusive doubly-linked list within a price level.
    Order* prev{nullptr};
    Order* next{nullptr};
};

static_assert(sizeof(Order) <= 2 * kCacheLine,
              "Order should fit in 1-2 cache lines");

}  // namespace ob
