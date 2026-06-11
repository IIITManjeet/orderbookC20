#include "order_book/trading_engine.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <utility>

namespace ob {

std::optional<StrategyAction> MeanReversion::on_tick(const MarketSnapshot& s) {
    const Price mid = s.mid();
    if (mid == 0) return std::nullopt;

    window_.push_back(mid);
    sum_ += mid;
    if (window_.size() > cfg_.window) {
        sum_ -= window_.front();
        window_.pop_front();
    }
    if (window_.size() < cfg_.window) return std::nullopt;
    if (since_action_++ < cfg_.cool_down) return std::nullopt;

    const double mean = static_cast<double>(sum_) / static_cast<double>(window_.size());
    const double deviation_bps = (static_cast<double>(mid) - mean) * 1e4 / mean;

    if (deviation_bps <= -cfg_.entry_bps) {
        since_action_ = 0;
        return StrategyAction{Side::Buy, cfg_.trade_qty};
    }
    if (deviation_bps >= cfg_.entry_bps) {
        since_action_ = 0;
        return StrategyAction{Side::Sell, cfg_.trade_qty};
    }
    return std::nullopt;
}

TradingEngine::TradingEngine(SPSCQueue<MarketEvent>& source,
                             std::unique_ptr<Strategy> strategy,
                             FillCallback on_fill)
    : source_(source),
      strategy_(std::move(strategy)),
      on_fill_(std::move(on_fill)) {}

TradingEngine::~TradingEngine() { stop(); }

void TradingEngine::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { run(); });
}

void TradingEngine::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

std::int64_t TradingEngine::equity_at(Price mid) const noexcept {
    const std::int64_t notional =
        (pos_.btc_qty * static_cast<std::int64_t>(mid)) / kQuantityScale;
    return pos_.cash + notional;
}

void TradingEngine::refresh_quotes(const MarketSnapshot& s) {
    const auto noop = [](const Trade&) {};

    // Cancel/replace the previous resting quotes so state stays bounded and the
    // two quote OrderIds remain stable (kBidQuoteId / kAskQuoteId).
    if (bid_quote_live_) { book_.cancel(kBidQuoteId); bid_quote_live_ = false; }
    if (ask_quote_live_) { book_.cancel(kAskQuoteId); ask_quote_live_ = false; }

    // Skip ticks with zero or crossed prices.
    if (s.best_bid == 0 || s.best_ask == 0 || s.best_bid > s.best_ask) {
        cur_bid_ = cur_ask_ = 0;
        cur_bid_qty_ = cur_ask_qty_ = 0;
        return;
    }

    cur_bid_     = s.best_bid;
    cur_ask_     = s.best_ask;
    cur_bid_qty_ = s.bid_qty;
    cur_ask_qty_ = s.ask_qty;

    // Rest both quotes when the book is genuinely two-sided (best_bid <
    // best_ask). On a locked book (best_bid == best_ask) two opposing limits at
    // the same price would self-match, so we defer placement to action time and
    // rest only the single opposite quote a taker actually crosses.
    if (s.best_bid < s.best_ask) {
        if (cur_bid_qty_ > 0 &&
            book_.submit(kBidQuoteId, Side::Buy, OrderType::Limit, cur_bid_,
                         cur_bid_qty_, /*ts=*/0, noop)) {
            bid_quote_live_ = true;
        }
        if (cur_ask_qty_ > 0 &&
            book_.submit(kAskQuoteId, Side::Sell, OrderType::Limit, cur_ask_,
                         cur_ask_qty_, /*ts=*/0, noop)) {
            ask_quote_live_ = true;
        }
    }
}

void TradingEngine::apply_action(const StrategyAction& a, const MarketSnapshot& s) {
    if (a.qty == 0) return;

    // The taker crosses the opposite resting quote. Ensure that quote is resting
    // at the current top-of-book price/qty (it may have been deferred on a
    // locked book). Place it on demand using the stable quote OrderId.
    const auto noop = [](const Trade&) {};
    if (a.side == Side::Buy) {
        if (cur_ask_ == 0 || cur_ask_qty_ == 0) return;
        if (!ask_quote_live_ &&
            book_.submit(kAskQuoteId, Side::Sell, OrderType::Limit, cur_ask_,
                         cur_ask_qty_, /*ts=*/0, noop)) {
            ask_quote_live_ = true;
        }
        if (!ask_quote_live_) return;
    } else {
        if (cur_bid_ == 0 || cur_bid_qty_ == 0) return;
        if (!bid_quote_live_ &&
            book_.submit(kBidQuoteId, Side::Buy, OrderType::Limit, cur_bid_,
                         cur_bid_qty_, /*ts=*/0, noop)) {
            bid_quote_live_ = true;
        }
        if (!bid_quote_live_) return;
    }

    // Submit a marketable IOC taker that crosses the resting opposite quote.
    // Aggregate the resulting Trade(s); if the requested qty exceeds the resting
    // size we fill partially and book only the actually-filled qty.
    Quantity  filled = 0;
    std::int64_t cash_delta = 0;      // signed Price-scale
    Quantity  agg_volume = 0;
    Price     last_px = 0;
    const auto collect = [&](const Trade& t) {
        filled     += t.qty;
        agg_volume += t.qty;
        last_px     = t.price;
        const std::int64_t notional =
            (static_cast<std::int64_t>(t.qty) * static_cast<std::int64_t>(t.price))
            / kQuantityScale;
        cash_delta += (a.side == Side::Buy) ? -notional : notional;
    };

    const OrderId taker_id  = next_id_++;
    const Price   limit_px  = (a.side == Side::Buy) ? cur_ask_ : cur_bid_;
    book_.submit(taker_id, a.side, OrderType::IOC, limit_px, a.qty, /*ts=*/0, collect);

    // Whatever remains of the opposite quote (if the taker only partially
    // consumed it) is left resting; refresh_quotes cancels it next tick. If the
    // taker fully consumed it, the quote id is no longer in the book.
    if (a.side == Side::Buy) {
        if (book_.best_ask() == 0) ask_quote_live_ = false;
    } else {
        if (book_.best_bid() == 0) bid_quote_live_ = false;
    }

    if (filled == 0) return;   // no liquidity crossed

    if (a.side == Side::Buy) pos_.btc_qty += static_cast<std::int64_t>(filled);
    else                     pos_.btc_qty -= static_cast<std::int64_t>(filled);
    pos_.cash         += cash_delta;
    pos_.gross_volume += agg_volume;
    ++pos_.fills;

    if (on_fill_) {
        on_fill_(Fill{
            .side     = a.side,
            .price    = last_px,
            .qty      = filled,
            .event_ts = s.event_ts,
            .ts       = static_cast<Timestamp>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count()),
        });
    }
}

void TradingEngine::run() {
    MarketEvent ev;
    while (running_.load(std::memory_order_acquire)) {
        if (!source_.try_pop(ev)) {
            // std::this_thread::sleep_for(std::chrono::microseconds(200));
            continue;
        }
        if (ev.kind != MarketEventKind::BookTicker) continue;

        MarketSnapshot snap{
            .best_bid   = ev.best_bid,
            .best_ask   = ev.best_ask,
            .bid_qty    = ev.bid_qty,
            .ask_qty    = ev.ask_qty,
            .last_trade = ev.last_trade,
            .event_ts   = ev.ts,
        };
        last_mid_.store(snap.mid(), std::memory_order_relaxed);

        // Maintain the internal order book's synthetic top-of-book before the
        // strategy acts so any taker crosses real resting liquidity.
        refresh_quotes(snap);

        if (auto act = strategy_->on_tick(snap)) {
            apply_action(*act, snap);
        }
    }
}

}  // namespace ob
