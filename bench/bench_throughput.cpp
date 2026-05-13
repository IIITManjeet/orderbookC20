#include <benchmark/benchmark.h>

#include <random>

#include "order_book/order_book.hpp"

using namespace ob;

namespace {

constexpr std::size_t kPool = 1u << 20;

}  // namespace

// Insert-only Limit orders into an empty book, half buy / half sell, narrow
// price band so most rest without matching. Measures pure insert cost.
static void BM_InsertLimit(benchmark::State& state) {
    const std::size_t n = static_cast<std::size_t>(state.range(0));
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int> px(-50, 50);

    auto noop = [](const Trade&) {};

    for (auto _ : state) {
        state.PauseTiming();
        OrderBook book(kPool);
        // Seed the book with a few hundred resting orders so price levels
        // exist when we time the insert loop.
        for (std::size_t i = 0; i < 200; ++i) {
            book.submit(i, Side::Buy,  OrderType::Limit, 1000 + px(rng), 1, 0, noop);
            book.submit(i + 1'000'000, Side::Sell, OrderType::Limit, 1100 + px(rng), 1, 0, noop);
        }
        state.ResumeTiming();

        for (std::size_t i = 0; i < n; ++i) {
            const Side s = (i & 1) ? Side::Buy : Side::Sell;
            const Price p = (s == Side::Buy ? 1000 : 1100) + px(rng);
            book.submit(2'000'000 + i, s, OrderType::Limit, p, 1, 0, noop);
        }
    }
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_InsertLimit)->Arg(1'000)->Arg(10'000)->Arg(100'000);

// Aggressive crossing flow: each new order takes liquidity from the other
// side. Measures matching-path throughput.
static void BM_MatchAggressive(benchmark::State& state) {
    const std::size_t n = static_cast<std::size_t>(state.range(0));
    auto noop = [](const Trade&) {};

    for (auto _ : state) {
        state.PauseTiming();
        OrderBook book(kPool);
        // Stack a deep ask book.
        for (std::size_t i = 0; i < n; ++i) {
            book.submit(i + 1, Side::Sell, OrderType::Limit, 1000, 1, 0, noop);
        }
        state.ResumeTiming();

        // Sweep it.
        for (std::size_t i = 0; i < n; ++i) {
            book.submit(1'000'000 + i, Side::Buy, OrderType::IOC, 1000, 1, 0, noop);
        }
    }
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_MatchAggressive)->Arg(1'000)->Arg(10'000)->Arg(100'000);

// Cancel-heavy flow — common in real markets where most orders never match.
static void BM_InsertCancel(benchmark::State& state) {
    const std::size_t n = static_cast<std::size_t>(state.range(0));
    auto noop = [](const Trade&) {};

    for (auto _ : state) {
        state.PauseTiming();
        OrderBook book(kPool);
        state.ResumeTiming();

        for (std::size_t i = 0; i < n; ++i) {
            book.submit(i + 1, Side::Buy, OrderType::Limit, 1000, 1, 0, noop);
        }
        for (std::size_t i = 0; i < n; ++i) {
            book.cancel(i + 1);
        }
    }
    state.SetItemsProcessed(state.iterations() * n * 2);
}
BENCHMARK(BM_InsertCancel)->Arg(1'000)->Arg(10'000)->Arg(100'000);

BENCHMARK_MAIN();
