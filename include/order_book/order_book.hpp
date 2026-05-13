#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

#include "order_book/cache.hpp"
#include "order_book/pool_allocator.hpp"
#include "order_book/price_level.hpp"
#include "order_book/types.hpp"

namespace ob {

// Sorted vector of price levels — `std::flat_map`-style storage. For a single
// instrument the active price range is small (tens to a few thousand levels),
// so a contiguous vector beats `std::map` on every operation that matters:
// best-price lookup is O(1), and inserts/erases are rare relative to matches
// (which touch only the front).
//
// Bids are stored in *descending* price order so the best bid is at the back;
// asks in *ascending* order so the best ask is at the back. "Back" is the hot
// end and stays in cache.
class SideBook {
public:
    explicit SideBook(Side side) : side_(side) {}

    // Returns pointer to the price level, creating one if needed.
    PriceLevel* get_or_insert(Price p) {
        auto it = find_level(p);
        if (it != levels_.end() && it->price == p) return &*it;
        auto inserted = levels_.insert(it, PriceLevel{p, 0, nullptr, nullptr});
        return &*inserted;
    }

    PriceLevel* find(Price p) noexcept {
        auto it = find_level(p);
        if (it != levels_.end() && it->price == p) return &*it;
        return nullptr;
    }

    void erase_if_empty(PriceLevel* lvl) {
        if (!lvl->empty()) return;
        // Levels stay sorted; find by price.
        auto it = find_level(lvl->price);
        if (it != levels_.end() && it->price == lvl->price) {
            levels_.erase(it);
        }
    }

    // Best price level (top of book) or nullptr if side is empty.
    PriceLevel* best() noexcept {
        return levels_.empty() ? nullptr : &levels_.back();
    }

    Side side() const noexcept { return side_; }
    std::size_t depth() const noexcept { return levels_.size(); }

    // Read-only view of levels in storage order (worst -> best). The last
    // element is the top of book. Used for dry-run scans (FOK).
    std::span<const PriceLevel> levels_view() const noexcept {
        return {levels_.data(), levels_.size()};
    }

private:
    // Predicate that places "best" at the back.
    bool less_for_side(Price a, Price b) const noexcept {
        // Buy:  ascending (best = highest = back)
        // Sell: descending (best = lowest = back)
        return side_ == Side::Buy ? a < b : a > b;
    }

    std::vector<PriceLevel>::iterator find_level(Price p) {
        return std::lower_bound(
            levels_.begin(), levels_.end(), p,
            [this](const PriceLevel& lvl, Price q) {
                return less_for_side(lvl.price, q);
            });
    }

    Side side_;
    std::vector<PriceLevel> levels_;
};

class OrderBook {
public:
    using TradeCallback = std::function<void(const Trade&)>;

    explicit OrderBook(std::size_t pool_capacity = 1u << 20)
        : pool_(pool_capacity), bids_(Side::Buy), asks_(Side::Sell) {
        id_index_.reserve(pool_capacity);
    }

    // Submit an order. Returns false only if the order pool is exhausted.
    // Generated trades are reported via `on_trade`.
    bool submit(OrderId id, Side side, OrderType type, Price price,
                Quantity qty, Timestamp ts, const TradeCallback& on_trade);

    // Cancel a resting order by id. Returns true if found and cancelled.
    bool cancel(OrderId id);

    // Read-only inspection helpers — used by tests and the demo.
    Price best_bid() const noexcept {
        auto* l = const_cast<SideBook&>(bids_).best();
        return l ? l->price : 0;
    }
    Price best_ask() const noexcept {
        auto* l = const_cast<SideBook&>(asks_).best();
        return l ? l->price : 0;
    }
    std::size_t resting_orders() const noexcept { return id_index_.size(); }

private:
    // Match a taker against the opposite side. Returns the unfilled remainder.
    template <Side TakerSide>
    Quantity match(Order& taker, const TradeCallback& on_trade);

    void rest(Order* o);

    ObjectPool<Order> pool_;
    SideBook bids_;
    SideBook asks_;
    std::unordered_map<OrderId, Order*> id_index_;
    Timestamp clock_{0};
};

}  // namespace ob
