#include "order_book/order_book.hpp"

#include <algorithm>

namespace ob {

namespace {

OB_ALWAYS_INLINE bool price_crosses(Side taker, Price taker_px, Price book_px) noexcept {
    // Buy crosses an ask priced <= taker_px; Sell crosses a bid priced >= taker_px.
    return taker == Side::Buy ? book_px <= taker_px : book_px >= taker_px;
}

}  // namespace

template <Side TakerSide>
Quantity OrderBook::match(Order& taker, const TradeCallback& on_trade) {
    SideBook& opp = (TakerSide == Side::Buy) ? asks_ : bids_;

    while (taker.qty > 0) {
        PriceLevel* best = opp.best();
        if (!best) break;

        // For Market orders, treat as cross-anything; for Limit/IOC/FOK
        // respect the limit price.
        const bool unlimited = (taker.type == OrderType::Market);
        if (OB_UNLIKELY(!unlimited &&
                        !price_crosses(TakerSide, taker.price, best->price))) {
            break;
        }

        // Fill against the FIFO at this level until exhausted or taker filled.
        while (taker.qty > 0 && best->head) {
            Order* maker = best->head;
            const Quantity fill = std::min(taker.qty, maker->qty);

            on_trade(Trade{
                .maker_id = maker->id,
                .taker_id = taker.id,
                .price    = best->price,
                .qty      = fill,
                .ts       = taker.ts,
            });

            taker.qty   -= fill;
            maker->qty  -= fill;
            best->total_qty -= fill;

            if (maker->qty == 0) {
                // Maker fully filled — remove from level and pool.
                best->head = maker->next;
                if (best->head) best->head->prev = nullptr;
                else            best->tail = nullptr;
                id_index_.erase(maker->id);
                pool_.release(maker);
            }
        }

        if (best->empty()) opp.erase_if_empty(best);
    }
    return taker.qty;
}

void OrderBook::rest(Order* o) {
    SideBook& book = (o->side == Side::Buy) ? bids_ : asks_;
    PriceLevel* lvl = book.get_or_insert(o->price);
    lvl->push_back(o);
    id_index_.emplace(o->id, o);
}

bool OrderBook::submit(OrderId id, Side side, OrderType type, Price price,
                       Quantity qty, Timestamp ts,
                       const TradeCallback& on_trade) {
    if (OB_UNLIKELY(qty == 0)) return true;

    // Allocate transient order on the pool — even if it never rests, the
    // matcher needs a stable home for its mutable qty field.
    Order* o = pool_.acquire();
    if (OB_UNLIKELY(!o)) return false;
    o->id    = id;
    o->price = price;
    o->qty   = qty;
    o->ts    = (ts == 0 ? ++clock_ : ts);
    o->side  = side;
    o->type  = type;
    o->prev  = o->next = nullptr;

    // FOK: dry-run scan of opposite side from best toward worst. If the full
    // qty can't be filled at crossing prices, drop the order without trading.
    if (type == OrderType::FOK) {
        const SideBook& opp = (side == Side::Buy) ? asks_ : bids_;
        const auto levels = opp.levels_view();
        Quantity needed = qty;
        for (auto it = levels.rbegin(); it != levels.rend() && needed > 0; ++it) {
            if (!price_crosses(side, price, it->price)) break;
            needed = (it->total_qty >= needed) ? 0 : needed - it->total_qty;
        }
        if (needed > 0) {
            pool_.release(o);
            return true;  // FOK rejected — no fills, no resting.
        }
    }

    Quantity remaining = (side == Side::Buy)
        ? match<Side::Buy>(*o, on_trade)
        : match<Side::Sell>(*o, on_trade);
    o->qty = remaining;

    const bool should_rest =
        remaining > 0 && type == OrderType::Limit;

    if (should_rest) {
        rest(o);
    } else {
        // IOC/Market/FOK leftovers and fully-filled orders are released.
        pool_.release(o);
    }
    return true;
}

bool OrderBook::cancel(OrderId id) {
    auto it = id_index_.find(id);
    if (it == id_index_.end()) return false;

    Order* o = it->second;
    SideBook& book = (o->side == Side::Buy) ? bids_ : asks_;
    PriceLevel* lvl = book.find(o->price);
    if (lvl) {
        lvl->unlink(o);
        if (lvl->empty()) book.erase_if_empty(lvl);
    }
    id_index_.erase(it);
    pool_.release(o);
    return true;
}

// Explicit template instantiation so the matcher lives in this TU.
template Quantity OrderBook::match<Side::Buy>(Order&, const TradeCallback&);
template Quantity OrderBook::match<Side::Sell>(Order&, const TradeCallback&);

}  // namespace ob
