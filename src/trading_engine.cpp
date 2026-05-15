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

void TradingEngine::apply_action(const StrategyAction& a, const MarketSnapshot& s) {
    const Price exec_px = (a.side == Side::Buy) ? s.best_ask : s.best_bid;
    if (exec_px == 0 || a.qty == 0) return;

    const std::int64_t notional =
        (static_cast<std::int64_t>(a.qty) * static_cast<std::int64_t>(exec_px))
        / kQuantityScale;

    if (a.side == Side::Buy) {
        pos_.btc_qty += static_cast<std::int64_t>(a.qty);
        pos_.cash    -= notional;
    } else {
        pos_.btc_qty -= static_cast<std::int64_t>(a.qty);
        pos_.cash    += notional;
    }
    pos_.gross_volume += a.qty;
    ++pos_.fills;

    if (on_fill_) {
        on_fill_(Fill{
            .side     = a.side,
            .price    = exec_px,
            .qty      = a.qty,
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

        if (auto act = strategy_->on_tick(snap)) {
            apply_action(*act, snap);
        }
    }
}

}  // namespace ob
