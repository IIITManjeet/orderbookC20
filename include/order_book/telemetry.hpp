#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "order_book/types.hpp"

namespace ob {

// A small, POD-ish telemetry record. on_fill / status producers fill one of
// these and hand it to TelemetryPublisher::publish — no formatting, no
// allocation on enqueue. All formatting happens on the publisher thread.
struct TelemetryRecord {
    enum class Kind : std::uint8_t { Meta, Status, Fill };

    Kind          kind{Kind::Status};
    std::uint64_t ts_ns{0};   // wall-clock (system_clock) nanoseconds
    char          sym[16]{};  // NUL-terminated symbol (status/fill)

    // Status fields.
    double        mid{0};
    double        bid{0};
    double        ask{0};
    double        pos_btc{0};
    double        cash{0};
    double        equity{0};
    std::uint64_t fills{0};

    // Fill fields.
    Side   side{Side::Buy};
    double px{0};
    double qty{0};
    double lat_us{0};

    // Meta fields. source/feed are short tags; symbols is a comma-joined list.
    char meta_source[16]{};   // "binance" | "synthetic"
    char meta_feed[8]{};      // "ws" | "rest"
    char meta_symbols[256]{}; // "BTCUSDT,ETHUSDT,..."
};

namespace detail {

inline std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

// Copy a string into a fixed char buffer, always NUL-terminated.
template <std::size_t N>
inline void set_field(char (&dst)[N], std::string_view src) noexcept {
    const std::size_t n = src.size() < (N - 1) ? src.size() : (N - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

}  // namespace detail

// Format one record into a single compact JSONL object (no trailing newline).
// Runs on the publisher (cold) thread, so allocation / printf is fine.
inline std::string to_json_line(const TelemetryRecord& r) {
    char buf[512];
    int n = 0;
    switch (r.kind) {
        case TelemetryRecord::Kind::Meta:
            n = std::snprintf(
                buf, sizeof(buf),
                R"({"t":"meta","ts_ns":%llu,"source":"%s","feed":"%s","symbols":[%s]})",
                static_cast<unsigned long long>(r.ts_ns), r.meta_source,
                r.meta_feed, r.meta_symbols);
            break;
        case TelemetryRecord::Kind::Status:
            n = std::snprintf(
                buf, sizeof(buf),
                R"({"t":"status","ts_ns":%llu,"sym":"%s","mid":%.2f,"bid":%.2f,)"
                R"("ask":%.2f,"pos_btc":%.6f,"cash":%.2f,"equity":%.2f,"fills":%llu})",
                static_cast<unsigned long long>(r.ts_ns), r.sym, r.mid, r.bid,
                r.ask, r.pos_btc, r.cash, r.equity,
                static_cast<unsigned long long>(r.fills));
            break;
        case TelemetryRecord::Kind::Fill:
            n = std::snprintf(
                buf, sizeof(buf),
                R"({"t":"fill","ts_ns":%llu,"sym":"%s","side":"%s","px":%.2f,)"
                R"("qty":%.6f,"lat_us":%.3f})",
                static_cast<unsigned long long>(r.ts_ns), r.sym,
                r.side == Side::Buy ? "BUY" : "SELL", r.px, r.qty, r.lat_us);
            break;
    }
    if (n < 0) return std::string{};
    return std::string(buf, static_cast<std::size_t>(n));
}

// A destination for formatted JSONL lines. A future WS-broadcast sink simply
// subclasses this — TelemetryPublisher fans out to every attached sink with no
// changes to this file.
struct TelemetrySink {
    virtual ~TelemetrySink() = default;
    virtual void write_line(std::string_view line) = 0;
    virtual void flush() {}
};

// Appends newline-delimited JSON to a file.
class FileSink : public TelemetrySink {
public:
    explicit FileSink(const std::string& path)
        : out_(path, std::ios::out | std::ios::trunc) {}

    bool good() const { return static_cast<bool>(out_); }

    void write_line(std::string_view line) override {
        out_.write(line.data(), static_cast<std::streamsize>(line.size()));
        out_.put('\n');
    }
    void flush() override { out_.flush(); }

private:
    std::ofstream out_;
};

// Multi-producer telemetry queue + background publisher thread. Producers call
// publish() (mutex + deque push + notify, no formatting). One background thread
// drains the deque, formats each record once via to_json_line, and fans out to
// every sink. Safe with zero sinks attached.
class TelemetryPublisher {
public:
    TelemetryPublisher() = default;
    ~TelemetryPublisher() { stop(); }

    TelemetryPublisher(const TelemetryPublisher&) = delete;
    TelemetryPublisher& operator=(const TelemetryPublisher&) = delete;

    void add_sink(std::unique_ptr<TelemetrySink> sink) {
        sinks_.push_back(std::move(sink));
    }

    // Multi-producer safe; no formatting here. Cheap no-op if not running.
    void publish(const TelemetryRecord& rec) {
        {
            std::lock_guard<std::mutex> g(mu_);
            queue_.push_back(rec);
        }
        cv_.notify_one();
    }

    void start() {
        if (running_.exchange(true)) return;
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
        // Drain anything left after the worker exited, then flush.
        drain_locked_batch();
        for (auto& s : sinks_) s->flush();
    }

    // Construction helpers — stamp ts_ns with system_clock and fill the POD.
    static TelemetryRecord make_meta(std::string_view source,
                                     std::string_view feed,
                                     std::string_view joined_symbols) {
        TelemetryRecord r;
        r.kind  = TelemetryRecord::Kind::Meta;
        r.ts_ns = detail::now_ns();
        detail::set_field(r.meta_source, source);
        detail::set_field(r.meta_feed, feed);
        detail::set_field(r.meta_symbols, joined_symbols);
        return r;
    }

    static TelemetryRecord make_status(std::string_view sym, double mid,
                                       double bid, double ask, double pos_btc,
                                       double cash, double equity,
                                       std::uint64_t fills) {
        TelemetryRecord r;
        r.kind  = TelemetryRecord::Kind::Status;
        r.ts_ns = detail::now_ns();
        detail::set_field(r.sym, sym);
        r.mid     = mid;
        r.bid     = bid;
        r.ask     = ask;
        r.pos_btc = pos_btc;
        r.cash    = cash;
        r.equity  = equity;
        r.fills   = fills;
        return r;
    }

    static TelemetryRecord make_fill(std::string_view sym, Side side, double px,
                                     double qty, double lat_us) {
        TelemetryRecord r;
        r.kind  = TelemetryRecord::Kind::Fill;
        r.ts_ns = detail::now_ns();
        detail::set_field(r.sym, sym);
        r.side   = side;
        r.px     = px;
        r.qty    = qty;
        r.lat_us = lat_us;
        return r;
    }

private:
    void run() {
        std::vector<TelemetryRecord> batch;
        while (true) {
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return !queue_.empty() || !running_.load(); });
                if (queue_.empty() && !running_.load()) return;
                batch.assign(queue_.begin(), queue_.end());
                queue_.clear();
            }
            emit(batch);
            for (auto& s : sinks_) s->flush();
        }
    }

    // Drain whatever remains under lock and emit (used during stop()).
    void drain_locked_batch() {
        std::vector<TelemetryRecord> batch;
        {
            std::lock_guard<std::mutex> g(mu_);
            batch.assign(queue_.begin(), queue_.end());
            queue_.clear();
        }
        emit(batch);
    }

    void emit(const std::vector<TelemetryRecord>& batch) {
        for (const auto& rec : batch) {
            const std::string line = to_json_line(rec);
            for (auto& s : sinks_) s->write_line(line);
        }
    }

    std::vector<std::unique_ptr<TelemetrySink>> sinks_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<TelemetryRecord> queue_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace ob
