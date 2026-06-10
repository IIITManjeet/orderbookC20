#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

#include "order_book/spsc_queue.hpp"
#include "order_book/types.hpp"

namespace ob {

enum class MarketEventKind : std::uint8_t { BookTicker, Error };

enum class BinanceMarket : std::uint8_t { Spot, Futures };

struct MarketEvent {
    MarketEventKind kind{MarketEventKind::BookTicker};
    Price     best_bid{0};
    Price     best_ask{0};
    Quantity  bid_qty{0};
    Quantity  ask_qty{0};
    Price     last_trade{0};
    Timestamp ts{0};
};

// Price scale: 1 unit = 0.01 USD. So 65000.42 USD => Price = 6'500'042.
inline constexpr std::int64_t kPriceScale    = 100;
// Quantity scale: 1 unit = 1e-6 BTC. So 0.012345 BTC => Quantity = 12'345.
inline constexpr std::int64_t kQuantityScale = 1'000'000;

Price    to_price(double usd) noexcept;
Quantity to_qty(double btc) noexcept;
double   from_price(Price p) noexcept;
double   from_qty(Quantity q) noexcept;
bool parse_ws_book_ticker(std::string_view payload, MarketEvent& out) noexcept;

class Feed {
public:
    virtual ~Feed() = default;
    virtual void start() = 0;
    virtual void stop() = 0;
};

class BinanceFeed final : public Feed {
public:
    BinanceFeed(std::string symbol,
                SPSCQueue<MarketEvent>& sink,
                std::chrono::milliseconds poll_interval = std::chrono::milliseconds(200),
                BinanceMarket market = BinanceMarket::Futures);
    ~BinanceFeed() override;

    BinanceFeed(const BinanceFeed&) = delete;
    BinanceFeed& operator=(const BinanceFeed&) = delete;

    void start() override;
    void stop() override;

private:
    void run();
    bool fetch_once(MarketEvent& out, std::string& body_buf);

    std::string symbol_;
    SPSCQueue<MarketEvent>& sink_;
    std::chrono::milliseconds interval_;
    BinanceMarket market_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};
class WebSocketBinanceFeed final : public Feed {
public:
    WebSocketBinanceFeed(std::string symbol,
                         SPSCQueue<MarketEvent>& sink,
                         BinanceMarket market = BinanceMarket::Futures);
    ~WebSocketBinanceFeed() override;

    WebSocketBinanceFeed(const WebSocketBinanceFeed&) = delete;
    WebSocketBinanceFeed& operator=(const WebSocketBinanceFeed&) = delete;

    void start() override;
    void stop() override;

private:
    void run();

    std::string url_;
    SPSCQueue<MarketEvent>& sink_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

// Generates random-walk prices locally; no network. Use for offline runs,
// reproducibility (fixed seed), or stress-testing the engine.
class SyntheticFeed final : public Feed {
public:
    struct Config {
        double initial_price = 80000.0;   // USD
        double spread        = 0.50;      // USD between best bid and best ask
        double sigma         = 5.0;       // USD one-step std dev of mid
        double drift         = 0.0;       // USD per step
        unsigned seed        = 42;
    };

    SyntheticFeed(SPSCQueue<MarketEvent>& sink,
                  std::chrono::milliseconds tick_interval,
                  const Config& cfg);
    ~SyntheticFeed() override;

    SyntheticFeed(const SyntheticFeed&) = delete;
    SyntheticFeed& operator=(const SyntheticFeed&) = delete;

    void start() override;
    void stop() override;

private:
    void run();

    SPSCQueue<MarketEvent>& sink_;
    std::chrono::milliseconds interval_;
    Config cfg_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace ob
