#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "order_book/feed.hpp"
#include "order_book/order_book.hpp"
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

    Position position() const noexcept { return pos_; }
    Price last_mid() const noexcept { return last_mid_.load(std::memory_order_relaxed); }
    std::int64_t equity_at(Price mid) const noexcept;

private:
    void run();
    void apply_action(const StrategyAction& a, const MarketSnapshot& s);

    SPSCQueue<MarketEvent>& source_;
    std::unique_ptr<Strategy> strategy_;
    FillCallback on_fill_;
    Position pos_{};
    OrderId  next_id_{1};
    std::atomic<Price> last_mid_{0};

    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace ob
