#include <gtest/gtest.h>

#include "order_book/risk.hpp"

using namespace ob;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// kRiskQuantityScale = 1'000'000 (same as kQuantityScale in feed.hpp)
// kPriceScale = 100 (0.01 USD ticks)
// Notional formula: qty * exec_px / kRiskQuantityScale
//   e.g. qty=1'000'000, px=10'000'00 (=$100,000.00) => notional = 10'000'00

// ---------------------------------------------------------------------------
// Position-limit tests
// ---------------------------------------------------------------------------

TEST(RiskManager, PositionLimitAllowsIfUnderLimit) {
    RiskConfig cfg;
    cfg.max_position_qty = 5'000'000;  // 5 BTC
    RiskManager rm(cfg);

    StrategyAction action{Side::Buy, 1'000'000};  // buy 1 BTC
    auto result = rm.gate(action, /*cur_btc_qty=*/0, /*exec_px=*/5'000'000);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->qty, 1'000'000u);
    EXPECT_EQ(result->side, Side::Buy);
}

TEST(RiskManager, PositionLimitClampsQty) {
    RiskConfig cfg;
    cfg.max_position_qty = 3'000'000;  // 3 BTC limit
    RiskManager rm(cfg);

    // cur=2BTC, want to buy 2BTC => post=4BTC exceeds limit; should clamp to 1BTC
    StrategyAction action{Side::Buy, 2'000'000};
    auto result = rm.gate(action, /*cur_btc_qty=*/2'000'000, /*exec_px=*/5'000'000);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->qty, 1'000'000u);  // clamped to room: 3M - 2M = 1M
}

TEST(RiskManager, PositionLimitFullRejectAtMax) {
    RiskConfig cfg;
    cfg.max_position_qty = 3'000'000;  // 3 BTC limit
    RiskManager rm(cfg);

    // cur=3BTC, want to buy any => no room in Buy direction
    StrategyAction action{Side::Buy, 1'000'000};
    auto result = rm.gate(action, /*cur_btc_qty=*/3'000'000, /*exec_px=*/5'000'000);
    EXPECT_FALSE(result.has_value());
}

TEST(RiskManager, PositionLimitFullRejectBeyondMax) {
    RiskConfig cfg;
    cfg.max_position_qty = 2'000'000;
    RiskManager rm(cfg);

    // cur=3BTC (already beyond limit), Buy should be rejected
    StrategyAction action{Side::Buy, 1'000'000};
    auto result = rm.gate(action, /*cur_btc_qty=*/3'000'000, /*exec_px=*/5'000'000);
    EXPECT_FALSE(result.has_value());
}

TEST(RiskManager, PositionLimitSellDirectionClamped) {
    RiskConfig cfg;
    cfg.max_position_qty = 3'000'000;  // 3 BTC limit (abs)
    RiskManager rm(cfg);

    // cur=-2BTC (short), want to sell 2BTC => post=-4BTC, exceeds limit; clamp to 1BTC
    StrategyAction action{Side::Sell, 2'000'000};
    auto result = rm.gate(action, /*cur_btc_qty=*/-2'000'000, /*exec_px=*/5'000'000);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->qty, 1'000'000u);
    EXPECT_EQ(result->side, Side::Sell);
}

TEST(RiskManager, PositionLimitSellFullReject) {
    RiskConfig cfg;
    cfg.max_position_qty = 3'000'000;
    RiskManager rm(cfg);

    // cur=-3BTC, want to sell => no room
    StrategyAction action{Side::Sell, 1'000'000};
    auto result = rm.gate(action, /*cur_btc_qty=*/-3'000'000, /*exec_px=*/5'000'000);
    EXPECT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// Notional-limit tests
// ---------------------------------------------------------------------------

TEST(RiskManager, NotionalLimitAllowsIfUnder) {
    RiskConfig cfg;
    // notional = qty * px / scale = 1'000'000 * 5'000'000 / 1'000'000 = 5'000'000 (=$50,000)
    cfg.max_notional = 6'000'000;  // $60,000 notional
    RiskManager rm(cfg);

    StrategyAction action{Side::Buy, 1'000'000};
    auto result = rm.gate(action, 0, /*exec_px=*/5'000'000);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->qty, 1'000'000u);
}

TEST(RiskManager, NotionalLimitClampsQty) {
    RiskConfig cfg;
    // max notional = 3'000'000 price-scale units; exec_px = 5'000'000
    // max_abs_qty = max_notional * scale / px = 3'000'000 * 1'000'000 / 5'000'000 = 600'000
    // cur=0, want 1'000'000 => clamp to 600'000
    cfg.max_notional = 3'000'000;
    RiskManager rm(cfg);

    StrategyAction action{Side::Buy, 1'000'000};
    auto result = rm.gate(action, 0, /*exec_px=*/5'000'000);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->qty, 600'000u);
}

TEST(RiskManager, NotionalLimitFullRejectAtMax) {
    RiskConfig cfg;
    // max_abs_qty = 3'000'000 * 1'000'000 / 5'000'000 = 600'000
    // cur=600'000, Buy => no room
    cfg.max_notional = 3'000'000;
    RiskManager rm(cfg);

    StrategyAction action{Side::Buy, 500'000};
    auto result = rm.gate(action, /*cur_btc_qty=*/600'000, /*exec_px=*/5'000'000);
    EXPECT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// Drawdown stop-out tests
// ---------------------------------------------------------------------------

TEST(RiskManager, DrawdownNoHaltWhenEquityRising) {
    RiskConfig cfg;
    cfg.max_drawdown = 1'000'000;  // $10,000 drawdown allowed
    RiskManager rm(cfg);

    rm.observe_equity(5'000'000);
    rm.observe_equity(6'000'000);
    rm.observe_equity(7'000'000);
    EXPECT_FALSE(rm.halted());

    StrategyAction action{Side::Buy, 1'000};
    auto result = rm.gate(action, 0, 5'000'000);
    EXPECT_TRUE(result.has_value());
}

TEST(RiskManager, DrawdownHaltsOnExceedingLimit) {
    RiskConfig cfg;
    cfg.max_drawdown = 1'000'000;  // $10,000 max drawdown
    RiskManager rm(cfg);

    rm.observe_equity(10'000'000);   // peak = 10M
    rm.observe_equity(8'999'999);    // drawdown = 1'000'001 > 1'000'000 => halted
    EXPECT_TRUE(rm.halted());

    StrategyAction action{Side::Buy, 1'000};
    auto result = rm.gate(action, 0, 5'000'000);
    EXPECT_FALSE(result.has_value());  // halted => nullopt
}

TEST(RiskManager, DrawdownHaltIsPermanent) {
    RiskConfig cfg;
    cfg.max_drawdown = 500'000;
    RiskManager rm(cfg);

    rm.observe_equity(5'000'000);
    rm.observe_equity(4'000'000);  // drawdown = 1'000'000 > 500'000 => halted
    EXPECT_TRUE(rm.halted());

    // Even if equity recovers, still halted
    rm.observe_equity(6'000'000);
    EXPECT_TRUE(rm.halted());

    StrategyAction action{Side::Buy, 1'000};
    auto result = rm.gate(action, 0, 5'000'000);
    EXPECT_FALSE(result.has_value());
}

TEST(RiskManager, DrawdownExactlyAtLimitIsNotHalted) {
    RiskConfig cfg;
    cfg.max_drawdown = 1'000'000;  // exactly 1M allowed
    RiskManager rm(cfg);

    rm.observe_equity(5'000'000);  // peak = 5M
    rm.observe_equity(4'000'000);  // drawdown = 1'000'000 == max_drawdown (NOT > => not halted)
    EXPECT_FALSE(rm.halted());
}

// ---------------------------------------------------------------------------
// Unlimited (zero) limit tests
// ---------------------------------------------------------------------------

TEST(RiskManager, UnlimitedPositionQtyZero) {
    RiskConfig cfg;
    cfg.max_position_qty = 0;  // unlimited
    RiskManager rm(cfg);

    StrategyAction action{Side::Buy, 999'999'999};
    auto result = rm.gate(action, 1'000'000'000, 5'000'000);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->qty, 999'999'999u);
}

TEST(RiskManager, UnlimitedNotionalZero) {
    RiskConfig cfg;
    cfg.max_notional = 0;  // unlimited
    RiskManager rm(cfg);

    StrategyAction action{Side::Buy, 999'999'999};
    auto result = rm.gate(action, 0, 5'000'000);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->qty, 999'999'999u);
}

TEST(RiskManager, UnlimitedDrawdownZero) {
    RiskConfig cfg;
    cfg.max_drawdown = 0;  // unlimited
    RiskManager rm(cfg);

    rm.observe_equity(1'000'000'000);
    rm.observe_equity(-1'000'000'000);  // massive drawdown — no limit
    EXPECT_FALSE(rm.halted());

    StrategyAction action{Side::Buy, 1'000};
    auto result = rm.gate(action, 0, 5'000'000);
    EXPECT_TRUE(result.has_value());
}

TEST(RiskManager, AllZeroLimitsPermitsEverything) {
    RiskConfig cfg;  // all zeros = all unlimited
    RiskManager rm(cfg);

    StrategyAction action{Side::Sell, 1'000'000'000};
    auto result = rm.gate(action, 0, 5'000'000);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->qty, 1'000'000'000u);
}

// ---------------------------------------------------------------------------
// Buy vs sell direction symmetry
// ---------------------------------------------------------------------------

TEST(RiskManager, BuyAndSellBothConstrainedSymmetrically) {
    RiskConfig cfg;
    cfg.max_position_qty = 2'000'000;
    RiskManager rm(cfg);

    // Long side: cur=0, buy 3BTC => clamp to 2BTC
    {
        StrategyAction action{Side::Buy, 3'000'000};
        auto r = rm.gate(action, 0, 5'000'000);
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(r->qty, 2'000'000u);
    }

    // Short side: cur=0, sell 3BTC => clamp to 2BTC
    {
        StrategyAction action{Side::Sell, 3'000'000};
        auto r = rm.gate(action, 0, 5'000'000);
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(r->qty, 2'000'000u);
    }
}

TEST(RiskManager, BuyAllowedWhenAlreadyShort) {
    RiskConfig cfg;
    cfg.max_position_qty = 3'000'000;
    RiskManager rm(cfg);

    // cur=-2BTC, want to buy 1BTC => post=-1BTC, |post|=1BTC < 3BTC => allowed fully
    StrategyAction action{Side::Buy, 1'000'000};
    auto result = rm.gate(action, -2'000'000, 5'000'000);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->qty, 1'000'000u);
}

TEST(RiskManager, BuyAllowedBeyondZeroWhenShort) {
    RiskConfig cfg;
    cfg.max_position_qty = 2'000'000;
    RiskManager rm(cfg);

    // cur=-1BTC, want to buy 4BTC => post=+3BTC > 2BTC => clamp: room=2-(-1*1)=3, clamped by max=2
    // room = max_position_qty - cur_btc_qty * dir = 2M - (-1M)*1 = 3M
    // but proposed=4M and allowed stays min(4M, 3M) = 3M, then post=+2M <= 2M? Let's recalc:
    // post = cur + dir*qty = -1M + 1*3M = 2M => abs=2M == max => allowed
    StrategyAction action{Side::Buy, 4'000'000};
    auto result = rm.gate(action, -1'000'000, 5'000'000);
    ASSERT_TRUE(result.has_value());
    // room = 2M - (-1M * 1) = 3M, allowed_qty = min(4M,3M) = 3M
    EXPECT_EQ(result->qty, 3'000'000u);
}

// ---------------------------------------------------------------------------
// Combined limits: tightest one wins
// ---------------------------------------------------------------------------

TEST(RiskManager, TightestLimitWins) {
    RiskConfig cfg;
    cfg.max_position_qty = 5'000'000;  // 5 BTC
    // max_notional clamp: max_abs_qty = 2'000'000 * 1'000'000 / 5'000'000 = 400'000
    cfg.max_notional     = 2'000'000;
    RiskManager rm(cfg);

    // cur=0, want 1'000'000 (1BTC) at px=5'000'000
    // position clamp: room=5M, allowed=1M
    // notional clamp: post_notional = 1M*5M/1M = 5M > 2M => max_abs_qty=400'000 => clamp to 400'000
    StrategyAction action{Side::Buy, 1'000'000};
    auto result = rm.gate(action, 0, 5'000'000);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->qty, 400'000u);
}
