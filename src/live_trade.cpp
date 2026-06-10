#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "order_book/feed.hpp"
#include "order_book/spsc_queue.hpp"
#include "order_book/strategy.hpp"
#include "order_book/trading_engine.hpp"

namespace {
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

// One independent pipeline per symbol: its own queue, feed, strategy (owned by
// the engine), engine, and latency sample collection. Types here are
// non-copyable (and SPSCQueue is non-movable), so each pipeline is heap-owned
// and the latency container stays a plain std::vector<std::uint64_t> guarded by
// a per-symbol mutex — easy to swap for a histogram later.
struct Pipeline {
    std::string symbol;
    std::unique_ptr<ob::SPSCQueue<ob::MarketEvent>> queue;
    std::unique_ptr<ob::Feed> feed;
    std::unique_ptr<ob::TradingEngine> engine;

    std::vector<std::uint64_t> latencies_ns;
    std::mutex lat_mu;

    explicit Pipeline(std::string sym) : symbol(std::move(sym)) {}
};

// Split a comma-separated --symbol list into trimmed, validated entries.
std::vector<std::string> parse_symbols(const std::string& csv) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= csv.size()) {
        std::size_t comma = csv.find(',', start);
        std::size_t end = (comma == std::string::npos) ? csv.size() : comma;
        std::string tok = csv.substr(start, end - start);
        std::size_t b = tok.find_first_not_of(" \t");
        std::size_t e = tok.find_last_not_of(" \t");
        if (b != std::string::npos) out.push_back(tok.substr(b, e - b + 1));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}
}  // namespace

static void print_usage() {
    std::puts(
        "Usage: live_trade [options]\n"
        "  --source binance|synthetic   data source (default: synthetic)\n"
        "  --feed ws|rest               binance transport: WebSocket or REST poll (default: ws)\n"
        "  --market futures|spot        Binance market (default: futures)\n"
        "  --symbol BTCUSDT[,ETHUSDT…]  comma-separated symbol list (binance source only)\n"
        "  --poll-ms 250                REST feed tick interval in ms (rest feed only)\n"
        "  --seconds 0                  auto-stop after N seconds (0 = until Ctrl-C)\n"
        "  --seed 42                    RNG seed (synthetic source only)\n"
        "  --start-price 80000          starting price USD (synthetic)\n"
        "  --sigma 5.0                  per-tick stddev USD (synthetic)\n"
        "  --drift 0.0                  per-tick drift USD (synthetic)\n");
}

int main(int argc, char** argv) {
    using namespace ob;

    std::string source     = "synthetic";
    std::string feed_kind  = "ws";
    std::string market     = "futures";
    std::string symbol     = "BTCUSDT";
    int    poll_ms         = 250;
    int    run_seconds     = 0;
    unsigned seed          = 42;
    double start_price     = 80000.0;
    double sigma           = 5.0;
    double drift           = 0.0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing arg for %s\n", what); std::exit(1); }
            return argv[++i];
        };
        if      (a == "--source")      source = next("--source");
        else if (a == "--feed")        feed_kind = next("--feed");
        else if (a == "--market")      market = next("--market");
        else if (a == "--symbol")      symbol = next("--symbol");
        else if (a == "--poll-ms")     poll_ms = std::atoi(next("--poll-ms"));
        else if (a == "--seconds")     run_seconds = std::atoi(next("--seconds"));
        else if (a == "--seed")        seed = static_cast<unsigned>(std::atoi(next("--seed")));
        else if (a == "--start-price") start_price = std::atof(next("--start-price"));
        else if (a == "--sigma")       sigma = std::atof(next("--sigma"));
        else if (a == "--drift")       drift = std::atof(next("--drift"));
        else if (a == "-h" || a == "--help") { print_usage(); return 0; }
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); print_usage(); return 1; }
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    BinanceMarket bm = BinanceMarket::Futures;
    if (source == "binance") {
        if (market == "futures")   bm = BinanceMarket::Futures;
        else if (market == "spot") bm = BinanceMarket::Spot;
        else {
            std::fprintf(stderr, "unknown market: %s (use futures|spot)\n", market.c_str());
            return 1;
        }
        if (feed_kind != "ws" && feed_kind != "rest") {
            std::fprintf(stderr, "unknown feed: %s (use ws|rest)\n", feed_kind.c_str());
            return 1;
        }
    } else if (source != "synthetic") {
        std::fprintf(stderr, "unknown source: %s (use binance|synthetic)\n", source.c_str());
        return 1;
    }

    const std::vector<std::string> symbols = parse_symbols(symbol);
    if (symbols.empty()) {
        std::fprintf(stderr, "no valid symbols in --symbol %s\n", symbol.c_str());
        return 1;
    }

    MeanReversion::Config cfg;
    cfg.window    = 20;
    cfg.entry_bps = 0.5;
    cfg.trade_qty = to_qty(0.001);
    cfg.cool_down = 5;

    // Build one pipeline per symbol. Held by unique_ptr so the per-symbol mutex
    // and queue (non-movable) stay pinned in place while engines reference them.
    std::vector<std::unique_ptr<Pipeline>> pipelines;
    pipelines.reserve(symbols.size());
    for (const auto& sym : symbols) {
        auto pipe = std::make_unique<Pipeline>(sym);
        pipe->queue = std::make_unique<SPSCQueue<MarketEvent>>(1024);

        if (source == "binance") {
            if (feed_kind == "ws") {
                pipe->feed = std::make_unique<WebSocketBinanceFeed>(sym, *pipe->queue, bm);
            } else {
                pipe->feed = std::make_unique<BinanceFeed>(
                    sym, *pipe->queue, std::chrono::milliseconds(poll_ms), bm);
            }
        } else {
            SyntheticFeed::Config fcfg;
            fcfg.initial_price = start_price;
            fcfg.spread        = 0.5;
            fcfg.sigma         = sigma;
            fcfg.drift         = drift;
            fcfg.seed          = seed;
            pipe->feed = std::make_unique<SyntheticFeed>(
                *pipe->queue, std::chrono::milliseconds(poll_ms), fcfg);
        }

        auto strat = std::make_unique<MeanReversion>(cfg);

        Pipeline* self = pipe.get();
        auto on_fill = [self](const Fill& f) {
            const std::uint64_t lat_ns = (f.ts > f.event_ts) ? (f.ts - f.event_ts) : 0;
            {
                std::lock_guard<std::mutex> g(self->lat_mu);
                self->latencies_ns.push_back(lat_ns);
            }
            std::printf("[FILL %s] %s  px=$%.2f  qty=%.6f BTC  lat=%.1fµs\n",
                        self->symbol.c_str(),
                        f.side == Side::Buy ? "BUY " : "SELL",
                        from_price(f.price),
                        from_qty(f.qty),
                        static_cast<double>(lat_ns) / 1000.0);
        };

        pipe->engine = std::make_unique<TradingEngine>(*pipe->queue, std::move(strat), on_fill);
        pipelines.push_back(std::move(pipe));
    }

    for (auto& pipe : pipelines) {
        pipe->feed->start();
        pipe->engine->start();
    }

    if (source == "binance") {
        std::printf("source=binance feed=%s market=%s symbols=%s poll=%dms — Ctrl-C to stop.\n",
                    feed_kind.c_str(), market.c_str(), symbol.c_str(), poll_ms);
    } else {
        std::printf("source=synthetic symbols=%zu start=$%.2f sigma=$%.2f drift=$%.2f seed=%u poll=%dms — Ctrl-C to stop.\n",
                    pipelines.size(), start_price, sigma, drift, seed, poll_ms);
    }

    const auto t0 = std::chrono::steady_clock::now();
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        for (auto& pipe : pipelines) {
            const Position p     = pipe->engine->position();
            const Price    mid   = pipe->engine->last_mid();
            const std::int64_t eq = (mid > 0) ? pipe->engine->equity_at(mid) : p.cash;
            std::printf("[STATUS %s] mid=$%.2f  pos=%.6f BTC  cash=$%.2f  equity=$%.2f  fills=%zu\n",
                        pipe->symbol.c_str(),
                        from_price(mid),
                        static_cast<double>(p.btc_qty) / kQuantityScale,
                        static_cast<double>(p.cash) / kPriceScale,
                        static_cast<double>(eq) / kPriceScale,
                        p.fills);
        }

        if (run_seconds > 0 &&
            std::chrono::steady_clock::now() - t0 >= std::chrono::seconds(run_seconds)) {
            g_stop.store(true);
        }
    }

    for (auto& pipe : pipelines) {
        pipe->engine->stop();
        pipe->feed->stop();
    }

    for (auto& pipe : pipelines) {
        std::vector<std::uint64_t> samples;
        {
            std::lock_guard<std::mutex> g(pipe->lat_mu);
            samples = pipe->latencies_ns;
        }
        if (!samples.empty()) {
            std::sort(samples.begin(), samples.end());
            const auto pct = [&](double p) {
                const std::size_t idx = std::min(samples.size() - 1,
                                                 static_cast<std::size_t>(p * samples.size()));
                return samples[idx];
            };
            const auto to_us = [](std::uint64_t ns) { return static_cast<double>(ns) / 1000.0; };
            std::uint64_t sum = 0;
            for (auto v : samples) sum += v;
            const double mean_us = to_us(sum / samples.size());
            std::printf("[STATS %s] fills=%zu  event→fill latency µs: "
                        "min=%.1f  p50=%.1f  p99=%.1f  max=%.1f  mean=%.1f\n",
                        pipe->symbol.c_str(),
                        samples.size(),
                        to_us(samples.front()),
                        to_us(pct(0.50)),
                        to_us(pct(0.99)),
                        to_us(samples.back()),
                        mean_us);
        }
    }

    std::printf("stopped.\n");
    return 0;
}
