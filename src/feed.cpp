#include "order_book/feed.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

namespace ob {

Price to_price(double usd) noexcept {
    return static_cast<Price>(std::llround(usd * static_cast<double>(kPriceScale)));
}
Quantity to_qty(double btc) noexcept {
    return static_cast<Quantity>(std::llround(btc * static_cast<double>(kQuantityScale)));
}
double from_price(Price p) noexcept {
    return static_cast<double>(p) / static_cast<double>(kPriceScale);
}
double from_qty(Quantity q) noexcept {
    return static_cast<double>(q) / static_cast<double>(kQuantityScale);
}

namespace {

std::size_t curl_write_cb(char* data, std::size_t size, std::size_t nmemb, void* userp) {
    auto* buf = static_cast<std::string*>(userp);
    buf->append(data, size * nmemb);
    return size * nmemb;
}

Timestamp now_ns() {
    using namespace std::chrono;
    return static_cast<Timestamp>(
        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

}  // namespace

bool parse_ws_book_ticker(std::string_view payload, MarketEvent& out) noexcept {
    try {
        auto j = nlohmann::json::parse(payload);
        const double bid  = std::stod(j.at("b").get<std::string>());
        const double ask  = std::stod(j.at("a").get<std::string>());
        const double bqty = std::stod(j.at("B").get<std::string>());
        const double aqty = std::stod(j.at("A").get<std::string>());

        out.kind       = MarketEventKind::BookTicker;
        out.best_bid   = to_price(bid);
        out.best_ask   = to_price(ask);
        out.bid_qty    = to_qty(bqty);
        out.ask_qty    = to_qty(aqty);
        out.last_trade = (out.best_bid + out.best_ask) / 2;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

BinanceFeed::BinanceFeed(std::string symbol,
                         SPSCQueue<MarketEvent>& sink,
                         std::chrono::milliseconds poll_interval,
                         BinanceMarket market)
    : symbol_(std::move(symbol)),
      sink_(sink),
      interval_(poll_interval),
      market_(market) {
    static std::once_flag g_curl_init;
    std::call_once(g_curl_init, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
}

BinanceFeed::~BinanceFeed() { stop(); }

void BinanceFeed::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { run(); });
}

void BinanceFeed::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

bool BinanceFeed::fetch_once(MarketEvent& out, std::string& body) {
    body.clear();
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    const char* base = (market_ == BinanceMarket::Futures)
        ? "https://fapi.binance.com/fapi/v1/ticker/bookTicker?symbol="
        : "https://api.binance.com/api/v3/ticker/bookTicker?symbol=";
    const std::string url = std::string(base) + symbol_;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3000L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "orderBookC++/0.1");

    const CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK || http_code != 200) return false;

    try {
        auto j = nlohmann::json::parse(body);
        const double bid  = std::stod(j.at("bidPrice").get<std::string>());
        const double ask  = std::stod(j.at("askPrice").get<std::string>());
        const double bqty = std::stod(j.at("bidQty").get<std::string>());
        const double aqty = std::stod(j.at("askQty").get<std::string>());

        out.kind       = MarketEventKind::BookTicker;
        out.best_bid   = to_price(bid);
        out.best_ask   = to_price(ask);
        out.bid_qty    = to_qty(bqty);
        out.ask_qty    = to_qty(aqty);
        out.last_trade = (out.best_bid + out.best_ask) / 2;
        out.ts         = now_ns();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void BinanceFeed::run() {
    std::string body_buf;
    body_buf.reserve(8192);

    MarketEvent ev;
    while (running_.load(std::memory_order_acquire)) {
        const auto start = std::chrono::steady_clock::now();

        if (fetch_once(ev, body_buf)) {
            while (running_.load(std::memory_order_acquire) && !sink_.try_push(ev)) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }

        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed < interval_) {
            std::this_thread::sleep_for(interval_ - elapsed);
        }
    }
}

WebSocketBinanceFeed::WebSocketBinanceFeed(std::string symbol,
                                           SPSCQueue<MarketEvent>& sink,
                                           BinanceMarket market)
    : sink_(sink) {
    static std::once_flag g_net_init;
    std::call_once(g_net_init, []() { ix::initNetSystem(); });

    const std::string sym = lower(symbol);
    url_ = (market == BinanceMarket::Futures)
        ? "wss://fstream.binance.com/ws/" + sym + "@bookTicker"
        : "wss://stream.binance.com:9443/ws/" + sym + "@bookTicker";
}

WebSocketBinanceFeed::~WebSocketBinanceFeed() { stop(); }

void WebSocketBinanceFeed::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { run(); });
}

void WebSocketBinanceFeed::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

void WebSocketBinanceFeed::run() {
    ix::WebSocket ws;
    ws.setUrl(url_);
    ws.enableAutomaticReconnection();
    ws.setMinWaitBetweenReconnectionRetries(1000);    // 1s
    ws.setMaxWaitBetweenReconnectionRetries(30'000);
    ws.setPingInterval(180);

    ws.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        if (msg->type != ix::WebSocketMessageType::Message) return;  
        MarketEvent ev;
        if (!parse_ws_book_ticker(msg->str, ev)) return;
        ev.ts = now_ns();
        while (running_.load(std::memory_order_acquire) && !sink_.try_push(ev)) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });

    ws.start();
    while (running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ws.stop();
}

SyntheticFeed::SyntheticFeed(SPSCQueue<MarketEvent>& sink,
                             std::chrono::milliseconds tick_interval,
                             const Config& cfg)
    : sink_(sink), interval_(tick_interval), cfg_(cfg) {}

SyntheticFeed::~SyntheticFeed() { stop(); }

void SyntheticFeed::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { run(); });
}

void SyntheticFeed::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

void SyntheticFeed::run() {
    std::mt19937 rng(cfg_.seed);
    std::normal_distribution<double> step(cfg_.drift, cfg_.sigma);

    double mid = cfg_.initial_price;

    while (running_.load(std::memory_order_acquire)) {
        const auto start = std::chrono::steady_clock::now();

        mid += step(rng);
        if (mid < 1.0) mid = 1.0;  // floor; random walk can drift low over long runs

        const double bid = mid - cfg_.spread / 2.0;
        const double ask = mid + cfg_.spread / 2.0;

        MarketEvent ev;
        ev.kind       = MarketEventKind::BookTicker;
        ev.best_bid   = to_price(bid);
        ev.best_ask   = to_price(ask);
        ev.bid_qty    = to_qty(1.0);
        ev.ask_qty    = to_qty(1.0);
        ev.last_trade = to_price(mid);
        ev.ts         = now_ns();

        while (running_.load(std::memory_order_acquire) && !sink_.try_push(ev)) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }

        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed < interval_) {
            std::this_thread::sleep_for(interval_ - elapsed);
        }
    }
}

}  // namespace ob
