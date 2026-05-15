#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "order_book/feed.hpp"
#include "order_book/spsc_queue.hpp"
#include "order_book/strategy.hpp"
#include "order_book/trading_engine.hpp"

using namespace ob;

namespace {

MarketEvent make_event(double bid, double ask) {
    MarketEvent e;
    e.kind     = MarketEventKind::BookTicker;
    e.best_bid = to_price(bid);
    e.best_ask = to_price(ask);
    e.bid_qty  = to_qty(1.0);
    e.ask_qty  = to_qty(1.0);
    e.last_trade = (e.best_bid + e.best_ask) / 2;
    e.ts       = 0;
    return e;
}

}  // namespace

TEST(MeanReversion, NoActionWhileWindowFilling) {
    MeanReversion::Config cfg;
    cfg.window    = 5;
    cfg.entry_bps = 1.0;
    cfg.trade_qty = to_qty(0.001);
    cfg.cool_down = 0;
    MeanReversion strat(cfg);

    for (int i = 0; i < 4; ++i) {
        MarketSnapshot s{to_price(100.0), to_price(100.0), 0, 0, 0};
        EXPECT_FALSE(strat.on_tick(s).has_value());
    }
}

TEST(MeanReversion, BuysOnNegativeDeviation) {
    MeanReversion::Config cfg;
    cfg.window    = 5;
    cfg.entry_bps = 5.0;
    cfg.trade_qty = to_qty(0.001);
    cfg.cool_down = 0;
    MeanReversion strat(cfg);

    for (int i = 0; i < 5; ++i) {
        MarketSnapshot s{to_price(100.00), to_price(100.00), 0, 0, 0};
        strat.on_tick(s);
    }
    // Drop ~10 bps below mean (100 -> 99.9) should trigger a buy.
    MarketSnapshot drop{to_price(99.90), to_price(99.90), 0, 0, 0};
    auto act = strat.on_tick(drop);
    ASSERT_TRUE(act.has_value());
    EXPECT_EQ(act->side, Side::Buy);
}

TEST(MeanReversion, SellsOnPositiveDeviation) {
    MeanReversion::Config cfg;
    cfg.window    = 5;
    cfg.entry_bps = 5.0;
    cfg.trade_qty = to_qty(0.001);
    cfg.cool_down = 0;
    MeanReversion strat(cfg);

    for (int i = 0; i < 5; ++i) {
        MarketSnapshot s{to_price(100.0), to_price(100.0), 0, 0, 0};
        strat.on_tick(s);
    }
    MarketSnapshot up{to_price(100.10), to_price(100.10), 0, 0, 0};
    auto act = strat.on_tick(up);
    ASSERT_TRUE(act.has_value());
    EXPECT_EQ(act->side, Side::Sell);
}

TEST(SyntheticFeed, ProducesEventsAndIsDeterministic) {
    SPSCQueue<MarketEvent> q1(128);
    SPSCQueue<MarketEvent> q2(128);

    SyntheticFeed::Config c;
    c.initial_price = 100.0;
    c.spread        = 0.10;
    c.sigma         = 1.0;
    c.drift         = 0.0;
    c.seed          = 1234;

    SyntheticFeed f1(q1, std::chrono::milliseconds(1), c);
    SyntheticFeed f2(q2, std::chrono::milliseconds(1), c);
    f1.start();
    f2.start();

    std::vector<MarketEvent> s1, s2;
    for (int wait_ms = 0; wait_ms < 500 && (s1.size() < 20 || s2.size() < 20); ++wait_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        MarketEvent e;
        while (q1.try_pop(e)) s1.push_back(e);
        while (q2.try_pop(e)) s2.push_back(e);
    }
    f1.stop();
    f2.stop();

    ASSERT_GE(s1.size(), 20u);
    ASSERT_GE(s2.size(), 20u);
    for (std::size_t i = 0; i < std::min(s1.size(), s2.size()); ++i) {
        EXPECT_EQ(s1[i].best_bid, s2[i].best_bid);
        EXPECT_EQ(s1[i].best_ask, s2[i].best_ask);
        EXPECT_LT(s1[i].best_bid, s1[i].best_ask);
    }
}

TEST(TradingEngine, EndToEndPaperFill) {
    SPSCQueue<MarketEvent> q(64);

    MeanReversion::Config cfg;
    cfg.window    = 3;
    cfg.entry_bps = 5.0;
    cfg.trade_qty = to_qty(0.001);
    cfg.cool_down = 0;

    std::vector<Fill> fills;
    auto on_fill = [&](const Fill& f) { fills.push_back(f); };

    TradingEngine engine(q, std::make_unique<MeanReversion>(cfg), on_fill);
    engine.start();

    for (int i = 0; i < 3; ++i) {
        while (!q.try_push(make_event(100.0, 100.0))) {}
    }
    while (!q.try_push(make_event(99.90, 99.90))) {}

    // Give the consumer thread time to drain and act.
    for (int wait_ms = 0; wait_ms < 500 && fills.empty(); ++wait_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    engine.stop();

    ASSERT_FALSE(fills.empty());
    EXPECT_EQ(fills[0].side, Side::Buy);
    EXPECT_EQ(fills[0].qty, to_qty(0.001));
}
