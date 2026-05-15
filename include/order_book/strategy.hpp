#pragma once

#include <cstdint>
#include <deque>
#include <optional>

#include "order_book/feed.hpp"
#include "order_book/types.hpp"

namespace ob {

struct MarketSnapshot {
    Price     best_bid{0};
    Price     best_ask{0};
    Quantity  bid_qty{0};
    Quantity  ask_qty{0};
    Price     last_trade{0};
    Timestamp event_ts{0};   // set by feed when the tick was parsed
    Price     mid() const noexcept { return (best_bid + best_ask) / 2; }
};

struct StrategyAction {
    Side     side;
    Quantity qty;
};

class Strategy {
public:
    virtual ~Strategy() = default;
    virtual std::optional<StrategyAction> on_tick(const MarketSnapshot&) = 0;
};

// Mean-reversion: tracks rolling window of mid-prices; if current mid deviates
// from the window mean by more than `entry_bps` basis points, take the
// opposite side.
class MeanReversion final : public Strategy {
public:
    struct Config {
        std::size_t window      = 100;
        double      entry_bps   = 5.0;   // 1 bp = 0.01%
        Quantity    trade_qty   = 1'000; // 0.001 BTC at kQuantityScale = 1e-6
        std::size_t cool_down   = 20;    // ticks between actions
    };

    explicit MeanReversion(const Config& cfg) : cfg_(cfg) {}

    std::optional<StrategyAction> on_tick(const MarketSnapshot& s) override;

private:
    Config cfg_;
    std::deque<Price> window_;
    std::int64_t sum_{0};
    std::size_t since_action_{0};
};

}  // namespace ob
