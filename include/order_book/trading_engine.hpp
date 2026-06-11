#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "order_book/feed.hpp"
#include "order_book/order_book.hpp"
#include "order_book/risk.hpp"
#include "order_book/spsc_queue.hpp"
#include "order_book/strategy.hpp"

namespace ob {

struct Position {
    std::int64_t btc_qty{0};  // signed Quantity-scale (1e-6 BTC units)
    std::int64_t cash{0};     // signed Price-scale (0.01 USD units)
    Quantity     gross_volume{0};
    std::size_t  fills{0};
};

struct Fill {
    Side      side;
    Price     price;
    Quantity  qty;
    Timestamp event_ts;   // feed-side timestamp of the tick that triggered this fill
    Timestamp ts;         // engine-side timestamp when the fill was booked
};

class TradingEngine {
public:
    using FillCallback = std::function<void(const Fill&)>;

    TradingEngine(SPSCQueue<MarketEvent>& source,
                  std::unique_ptr<Strategy> strategy,
                  FillCallback on_fill);
    ~TradingEngine();

    TradingEngine(const TradingEngine&) = delete;
    TradingEngine& operator=(const TradingEngine&) = delete;

    void start();
    void stop();

    // Optional: enable a risk gate. Must be called before start(). Actions are
    // clamped/rejected by RiskManager and the engine halts trading once the
    // drawdown limit is breached. Additive — no effect unless called.
    void set_risk(const RiskConfig& cfg) { risk_ = std::make_unique<RiskManager>(cfg); }

    Position position() const noexcept { return pos_; }
    Price last_mid() const noexcept { return last_mid_.load(std::memory_order_relaxed); }
    std::int64_t equity_at(Price mid) const noexcept;

private:
    void run();
    void apply_action(const StrategyAction& a, const MarketSnapshot& s);
    // Refresh the synthetic top-of-book resting quotes (cancel + re-submit
    // with stable OrderIds so resting state stays bounded). Skips ticks with
    // zero or crossed prices.
    void refresh_quotes(const MarketSnapshot& s);

    SPSCQueue<MarketEvent>& source_;
    std::unique_ptr<Strategy> strategy_;
    FillCallback on_fill_;
    std::unique_ptr<RiskManager> risk_;   // null unless set_risk() was called
    Position pos_{};
    OrderId  next_id_{3};   // 1,2 reserved for the synthetic bid/ask quotes
    std::atomic<Price> last_mid_{0};

    // Internal matching engine fed by the live feed. The two synthetic quotes
    // use stable OrderIds (kBidQuoteId / kAskQuoteId) that are cancelled and
    // re-submitted each tick. Taker orders use ids drawn from next_id_.
    OrderBook book_;
    static constexpr OrderId kBidQuoteId = 1;
    static constexpr OrderId kAskQuoteId = 2;
    bool bid_quote_live_{false};   // is kBidQuoteId currently resting?
    bool ask_quote_live_{false};   // is kAskQuoteId currently resting?
    // Cached top-of-book from the most recent valid tick, used to place the
    // single opposite resting quote a taker crosses at action time.
    Price    cur_bid_{0};
    Price    cur_ask_{0};
    Quantity cur_bid_qty_{0};
    Quantity cur_ask_qty_{0};

    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace ob
