#include <gtest/gtest.h>

#include <vector>

#include "order_book/order_book.hpp"
#include "order_book/pool_allocator.hpp"
#include "order_book/spsc_queue.hpp"

using namespace ob;

namespace {

struct Recorder {
    std::vector<Trade> trades;
    void operator()(const Trade& t) { trades.push_back(t); }
};

}  // namespace

TEST(OrderBook, RestsLimitOnEmptyBook) {
    OrderBook book(64);
    Recorder r;
    book.submit(1, Side::Buy, OrderType::Limit, 100, 10, 0, std::ref(r));
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.best_ask(), 0);
    EXPECT_EQ(book.resting_orders(), 1u);
    EXPECT_TRUE(r.trades.empty());
}

TEST(OrderBook, MatchesAtBestPrice) {
    OrderBook book(64);
    Recorder r;
    book.submit(1, Side::Sell, OrderType::Limit, 100, 10, 0, std::ref(r));
    book.submit(2, Side::Buy,  OrderType::Limit, 101,  4, 0, std::ref(r));
    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.trades[0].maker_id, 1u);
    EXPECT_EQ(r.trades[0].taker_id, 2u);
    EXPECT_EQ(r.trades[0].price, 100);
    EXPECT_EQ(r.trades[0].qty, 4u);
    EXPECT_EQ(book.best_ask(), 100);  // 6 remains
    EXPECT_EQ(book.best_bid(), 0);
}

TEST(OrderBook, PriceTimeFifo) {
    OrderBook book(64);
    Recorder r;
    book.submit(1, Side::Sell, OrderType::Limit, 100, 5, 1, std::ref(r));
    book.submit(2, Side::Sell, OrderType::Limit, 100, 5, 2, std::ref(r));
    book.submit(3, Side::Buy,  OrderType::Limit, 100, 7, 3, std::ref(r));
    ASSERT_EQ(r.trades.size(), 2u);
    EXPECT_EQ(r.trades[0].maker_id, 1u);  // older filled first
    EXPECT_EQ(r.trades[0].qty, 5u);
    EXPECT_EQ(r.trades[1].maker_id, 2u);
    EXPECT_EQ(r.trades[1].qty, 2u);
    EXPECT_EQ(book.best_ask(), 100);      // 3 left on order 2
}

TEST(OrderBook, SweepsMultipleLevels) {
    OrderBook book(64);
    Recorder r;
    book.submit(1, Side::Sell, OrderType::Limit, 100, 5, 0, std::ref(r));
    book.submit(2, Side::Sell, OrderType::Limit, 101, 5, 0, std::ref(r));
    book.submit(3, Side::Sell, OrderType::Limit, 102, 5, 0, std::ref(r));
    book.submit(4, Side::Buy,  OrderType::Limit, 101, 8, 0, std::ref(r));
    ASSERT_EQ(r.trades.size(), 2u);
    EXPECT_EQ(r.trades[0].price, 100);
    EXPECT_EQ(r.trades[0].qty, 5u);
    EXPECT_EQ(r.trades[1].price, 101);
    EXPECT_EQ(r.trades[1].qty, 3u);
    EXPECT_EQ(book.best_ask(), 101);   // 2 left on order 2
}

TEST(OrderBook, MarketOrderIgnoresPrice) {
    OrderBook book(64);
    Recorder r;
    book.submit(1, Side::Sell, OrderType::Limit, 999, 10, 0, std::ref(r));
    book.submit(2, Side::Buy,  OrderType::Market,   0,  4, 0, std::ref(r));
    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.trades[0].price, 999);
    EXPECT_EQ(r.trades[0].qty, 4u);
}

TEST(OrderBook, IocDropsLeftover) {
    OrderBook book(64);
    Recorder r;
    book.submit(1, Side::Sell, OrderType::Limit, 100, 3, 0, std::ref(r));
    book.submit(2, Side::Buy,  OrderType::IOC,   100, 7, 0, std::ref(r));
    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.trades[0].qty, 3u);
    EXPECT_EQ(book.best_bid(), 0);  // leftover NOT rested
    EXPECT_EQ(book.best_ask(), 0);
    EXPECT_EQ(book.resting_orders(), 0u);
}

TEST(OrderBook, FokRejectsIfInsufficient) {
    OrderBook book(64);
    Recorder r;
    book.submit(1, Side::Sell, OrderType::Limit, 100, 3, 0, std::ref(r));
    book.submit(2, Side::Buy,  OrderType::FOK,   100, 5, 0, std::ref(r));
    EXPECT_TRUE(r.trades.empty());          // nothing trades
    EXPECT_EQ(book.best_ask(), 100);        // maker untouched
    EXPECT_EQ(book.resting_orders(), 1u);
}

TEST(OrderBook, FokFillsIfSufficient) {
    OrderBook book(64);
    Recorder r;
    book.submit(1, Side::Sell, OrderType::Limit, 100, 5, 0, std::ref(r));
    book.submit(2, Side::Buy,  OrderType::FOK,   100, 5, 0, std::ref(r));
    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.trades[0].qty, 5u);
}

TEST(OrderBook, Cancel) {
    OrderBook book(64);
    Recorder r;
    book.submit(1, Side::Buy, OrderType::Limit, 100, 10, 0, std::ref(r));
    EXPECT_EQ(book.resting_orders(), 1u);
    EXPECT_TRUE(book.cancel(1));
    EXPECT_EQ(book.resting_orders(), 0u);
    EXPECT_EQ(book.best_bid(), 0);
    EXPECT_FALSE(book.cancel(1));  // already gone
}

TEST(SpscQueue, RoundTrip) {
    SPSCQueue<int> q(8);
    for (int i = 0; i < 7; ++i) ASSERT_TRUE(q.try_push(i));
    int out = -1;
    for (int i = 0; i < 7; ++i) {
        ASSERT_TRUE(q.try_pop(out));
        EXPECT_EQ(out, i);
    }
    EXPECT_FALSE(q.try_pop(out));
}

TEST(ObjectPool, AcquireReleaseCycle) {
    ObjectPool<int> p(4);
    int* a = p.acquire(1);
    int* b = p.acquire(2);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(*a, 1);
    EXPECT_EQ(*b, 2);
    p.release(a);
    int* c = p.acquire(3);  // should re-use a's slot
    EXPECT_EQ(c, a);
    EXPECT_EQ(*c, 3);
    p.release(b);
    p.release(c);
}
