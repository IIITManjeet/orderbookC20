#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "order_book/telemetry.hpp"

using namespace ob;

namespace {

// A sink that just records every line in memory (no I/O), for round-trip tests.
struct VectorSink : public TelemetrySink {
    std::vector<std::string> lines;
    void write_line(std::string_view line) override {
        lines.emplace_back(line);
    }
};

std::filesystem::path temp_jsonl(const std::string& tag) {
    const auto pid = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return std::filesystem::temp_directory_path() /
           ("telemetry_" + tag + "_" + std::to_string(pid) + ".jsonl");
}

std::vector<std::string> read_lines(const std::filesystem::path& p) {
    std::vector<std::string> out;
    std::ifstream in(p);
    std::string line;
    while (std::getline(in, line)) out.push_back(line);
    return out;
}

}  // namespace

TEST(Telemetry, MetaJsonSchema) {
    TelemetryRecord r =
        TelemetryPublisher::make_meta("synthetic", "ws", R"("BTCUSDT","ETHUSDT")");
    const std::string line = to_json_line(r);

    EXPECT_NE(line.find(R"("t":"meta")"), std::string::npos);
    EXPECT_NE(line.find(R"("source":"synthetic")"), std::string::npos);
    EXPECT_NE(line.find(R"("feed":"ws")"), std::string::npos);
    EXPECT_NE(line.find(R"("symbols":["BTCUSDT","ETHUSDT"])"), std::string::npos);
    EXPECT_NE(line.find(R"("ts_ns":)"), std::string::npos);
    EXPECT_EQ(line.front(), '{');
    EXPECT_EQ(line.back(), '}');
    EXPECT_EQ(line.find('\n'), std::string::npos);
}

TEST(Telemetry, StatusJsonSchema) {
    TelemetryRecord r = TelemetryPublisher::make_status(
        "BTCUSDT", 80123.45, 80123.40, 80123.50, 0.001000, 99876.55, 100000.00, 7);
    const std::string line = to_json_line(r);

    EXPECT_NE(line.find(R"("t":"status")"), std::string::npos);
    EXPECT_NE(line.find(R"("sym":"BTCUSDT")"), std::string::npos);
    EXPECT_NE(line.find(R"("mid":80123.45)"), std::string::npos);
    EXPECT_NE(line.find(R"("bid":80123.40)"), std::string::npos);
    EXPECT_NE(line.find(R"("ask":80123.50)"), std::string::npos);
    EXPECT_NE(line.find(R"("pos_btc":0.001000)"), std::string::npos);
    EXPECT_NE(line.find(R"("cash":99876.55)"), std::string::npos);
    EXPECT_NE(line.find(R"("equity":100000.00)"), std::string::npos);
    EXPECT_NE(line.find(R"("fills":7)"), std::string::npos);
}

TEST(Telemetry, FillJsonSchema) {
    TelemetryRecord buy =
        TelemetryPublisher::make_fill("BTCUSDT", Side::Buy, 80123.45, 0.001000, 12.5);
    const std::string bline = to_json_line(buy);
    EXPECT_NE(bline.find(R"("t":"fill")"), std::string::npos);
    EXPECT_NE(bline.find(R"("sym":"BTCUSDT")"), std::string::npos);
    EXPECT_NE(bline.find(R"("side":"BUY")"), std::string::npos);
    EXPECT_NE(bline.find(R"("px":80123.45)"), std::string::npos);
    EXPECT_NE(bline.find(R"("qty":0.001000)"), std::string::npos);
    EXPECT_NE(bline.find(R"("lat_us":12.500)"), std::string::npos);

    TelemetryRecord sell =
        TelemetryPublisher::make_fill("ETHUSDT", Side::Sell, 3500.10, 0.5, 1.25);
    const std::string sline = to_json_line(sell);
    EXPECT_NE(sline.find(R"("side":"SELL")"), std::string::npos);
    EXPECT_NE(sline.find(R"("sym":"ETHUSDT")"), std::string::npos);
}

TEST(Telemetry, StampsWallClockTimestamp) {
    const auto before = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    TelemetryRecord r =
        TelemetryPublisher::make_fill("BTCUSDT", Side::Buy, 1.0, 1.0, 1.0);
    const auto after = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    EXPECT_GE(r.ts_ns, static_cast<std::uint64_t>(before));
    EXPECT_LE(r.ts_ns, static_cast<std::uint64_t>(after));
}

TEST(Telemetry, FileSinkPublisherRoundTrip) {
    const auto path = temp_jsonl("roundtrip");
    std::filesystem::remove(path);

    constexpr int kFills = 50;
    {
        TelemetryPublisher pub;
        auto sink = std::make_unique<FileSink>(path.string());
        ASSERT_TRUE(sink->good());
        pub.add_sink(std::move(sink));
        pub.start();

        pub.publish(TelemetryPublisher::make_meta("binance", "ws", R"("BTCUSDT")"));
        pub.publish(TelemetryPublisher::make_status(
            "BTCUSDT", 80000.0, 79999.0, 80001.0, 0.0, 100000.0, 100000.0, 0));
        for (int i = 0; i < kFills; ++i) {
            pub.publish(TelemetryPublisher::make_fill(
                "BTCUSDT", (i % 2 == 0) ? Side::Buy : Side::Sell, 80000.0 + i,
                0.001, static_cast<double>(i)));
        }
        pub.stop();  // drains and joins
    }

    const auto lines = read_lines(path);
    ASSERT_EQ(lines.size(), static_cast<std::size_t>(2 + kFills));
    EXPECT_NE(lines[0].find(R"("t":"meta")"), std::string::npos);
    EXPECT_NE(lines[1].find(R"("t":"status")"), std::string::npos);
    int fill_count = 0;
    for (std::size_t i = 2; i < lines.size(); ++i) {
        EXPECT_NE(lines[i].find(R"("t":"fill")"), std::string::npos);
        ++fill_count;
    }
    EXPECT_EQ(fill_count, kFills);

    std::filesystem::remove(path);
}

TEST(Telemetry, MultiProducerSafe) {
    const auto path = temp_jsonl("mp");
    std::filesystem::remove(path);

    constexpr int kThreads = 4;
    constexpr int kPerThread = 100;
    {
        TelemetryPublisher pub;
        auto sink = std::make_unique<FileSink>(path.string());
        ASSERT_TRUE(sink->good());
        pub.add_sink(std::move(sink));
        pub.start();

        std::vector<std::thread> producers;
        for (int t = 0; t < kThreads; ++t) {
            producers.emplace_back([&pub, t] {
                for (int i = 0; i < kPerThread; ++i) {
                    pub.publish(TelemetryPublisher::make_fill(
                        "BTCUSDT", Side::Buy, 80000.0,
                        0.001, static_cast<double>(t * 1000 + i)));
                }
            });
        }
        for (auto& p : producers) p.join();
        pub.stop();
    }

    const auto lines = read_lines(path);
    EXPECT_EQ(lines.size(), static_cast<std::size_t>(kThreads * kPerThread));

    std::filesystem::remove(path);
}

TEST(Telemetry, PublishBeforeStartAndAfterStopDoesNotCrash) {
    TelemetryPublisher pub;  // no sinks attached
    // Before start(): just queues, no worker — must not crash.
    pub.publish(TelemetryPublisher::make_status(
        "BTCUSDT", 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0));
    pub.start();
    pub.publish(TelemetryPublisher::make_fill("BTCUSDT", Side::Buy, 1.0, 1.0, 1.0));
    pub.stop();
    // After stop(): should be a safe no-op (just queues, never drained).
    pub.publish(TelemetryPublisher::make_fill("BTCUSDT", Side::Sell, 1.0, 1.0, 1.0));
    // Second stop is idempotent.
    pub.stop();
    SUCCEED();
}

TEST(Telemetry, ZeroSinksIsNoOp) {
    TelemetryPublisher pub;
    pub.start();
    for (int i = 0; i < 10; ++i) {
        pub.publish(TelemetryPublisher::make_fill(
            "BTCUSDT", Side::Buy, 1.0, 1.0, static_cast<double>(i)));
    }
    pub.stop();
    SUCCEED();
}
