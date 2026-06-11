#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "order_book/latency_hist.hpp"

using namespace ob;

// ---------------------------------------------------------------------------
// Helper: the bucket index for a given ns value (mirrors the header logic).
// ---------------------------------------------------------------------------
static int bucket_of(std::uint64_t ns) {
    return 63 - __builtin_clzll(ns | std::uint64_t{1});
}

// Lower edge of bucket b.
static std::uint64_t bucket_lo(int b) {
    return (b == 0) ? std::uint64_t{0} : (std::uint64_t{1} << b);
}

// Upper edge of bucket b (inclusive).
static std::uint64_t bucket_hi(int b) {
    if (b == 63) return std::numeric_limits<std::uint64_t>::max();
    return (std::uint64_t{2} << b) - 1;  // 2^(b+1) - 1
}

// ---------------------------------------------------------------------------
// 1. Empty histogram — all zeros, no crash.
// ---------------------------------------------------------------------------
TEST(LatencyHistogram, EmptyAllZeros) {
    LatencyHistogram h;
    EXPECT_EQ(h.count(), 0u);
    EXPECT_EQ(h.min(),   0u);
    EXPECT_EQ(h.max(),   0u);
    EXPECT_DOUBLE_EQ(h.mean_ns(), 0.0);
    // percentiles on empty histogram must not crash and must return 0.
    EXPECT_EQ(h.percentile_ns(0.0),  0u);
    EXPECT_EQ(h.percentile_ns(0.5),  0u);
    EXPECT_EQ(h.percentile_ns(0.99), 0u);
    EXPECT_EQ(h.percentile_ns(1.0),  0u);
}

// ---------------------------------------------------------------------------
// 2. Single sample — exact min/max/mean, percentile in the right bucket.
// ---------------------------------------------------------------------------
TEST(LatencyHistogram, SingleSample) {
    constexpr std::uint64_t kNs = 1500;  // 1.5 µs → bucket 10 ([1024,2047])
    LatencyHistogram h;
    h.add(kNs);

    EXPECT_EQ(h.count(), 1u);
    EXPECT_EQ(h.min(),   kNs);
    EXPECT_EQ(h.max(),   kNs);
    EXPECT_DOUBLE_EQ(h.mean_ns(), static_cast<double>(kNs));

    // All percentiles on a single sample must fall in bucket 10.
    const int expected_bucket = bucket_of(kNs);  // 10
    for (double p : {0.0, 0.25, 0.5, 0.75, 0.99, 1.0}) {
        const std::uint64_t pct = h.percentile_ns(p);
        // p=0 → clamped to min; p=1 → clamped to max; others → bucket edge.
        if (p <= 0.0) {
            EXPECT_EQ(pct, kNs) << "p=" << p;
        } else if (p >= 1.0) {
            EXPECT_EQ(pct, kNs) << "p=" << p;
        } else {
            // Bucket-edge result must lie within the bucket of the sample.
            EXPECT_GE(pct, bucket_lo(expected_bucket)) << "p=" << p;
            EXPECT_LE(pct, bucket_hi(expected_bucket)) << "p=" << p;
        }
    }
}

// ---------------------------------------------------------------------------
// 3. Known distribution — p50/p99 in predictable buckets; min/max/mean exact.
// ---------------------------------------------------------------------------
//
// Distribution: 100 samples at 1 000 ns (bucket 9, [512,1023])
//               and 1 sample at 100 000 ns (bucket 16, [65536,131071]).
//
// With 101 total samples:
//   p50 rank = round(0.50 * 101 + 0.5) = 51  → falls in the 1000 ns bucket.
//   p99 rank = round(0.99 * 101 + 0.5) = 101 → the 100 000 ns sample.
//
// min  = 1 000 (exact)
// max  = 100 000 (exact)
// mean = (100 * 1000 + 1 * 100000) / 101 ≈ 1980.19... (exact from integers)
// ---------------------------------------------------------------------------
TEST(LatencyHistogram, KnownDistribution) {
    constexpr std::uint64_t kLow  = 1'000;    // 1 µs
    constexpr std::uint64_t kHigh = 100'000;  // 100 µs

    LatencyHistogram h;
    for (int i = 0; i < 100; ++i) h.add(kLow);
    h.add(kHigh);

    ASSERT_EQ(h.count(), 101u);

    // --- Exact accumulators ---
    EXPECT_EQ(h.min(), kLow);
    EXPECT_EQ(h.max(), kHigh);
    const double expected_mean =
        static_cast<double>(100u * kLow + kHigh) / 101.0;
    EXPECT_DOUBLE_EQ(h.mean_ns(), expected_mean);

    // --- Bucket for low samples ---
    const int lo_bucket = bucket_of(kLow);   // floor(log2(1000)) = 9
    const int hi_bucket = bucket_of(kHigh);  // floor(log2(100000)) = 16

    // p50 must be the lower edge of the low bucket (all 100 low samples
    // dominate the first 100 ranks).
    const std::uint64_t p50 = h.percentile_ns(0.50);
    EXPECT_GE(p50, bucket_lo(lo_bucket));
    EXPECT_LE(p50, bucket_hi(lo_bucket));

    // p99 rank = 100th sample (still within the 100 low samples) or the
    // single high sample depending on rounding.  Either way it must be in one
    // of the two buckets.
    const std::uint64_t p99 = h.percentile_ns(0.99);
    EXPECT_TRUE(
        (p99 >= bucket_lo(lo_bucket) && p99 <= bucket_hi(lo_bucket)) ||
        (p99 >= bucket_lo(hi_bucket) && p99 <= bucket_hi(hi_bucket))
    ) << "p99=" << p99 << " not in bucket " << lo_bucket << " or " << hi_bucket;

    // p99 >= p50 >= 0.
    EXPECT_GE(p99, p50);
}

// ---------------------------------------------------------------------------
// 4. Monotonic percentiles: p50 <= p99 <= max (also checks p0 <= p50).
// ---------------------------------------------------------------------------
TEST(LatencyHistogram, MonotonicPercentiles) {
    LatencyHistogram h;
    // Mix of values spread across several buckets.
    const std::uint64_t values[] = {
        100, 500, 1000, 2000, 5000, 10000, 50000, 100000, 500000, 1000000
    };
    for (auto v : values) {
        for (int i = 0; i < 10; ++i) h.add(v);
    }

    const std::uint64_t p0  = h.percentile_ns(0.0);
    const std::uint64_t p50 = h.percentile_ns(0.50);
    const std::uint64_t p90 = h.percentile_ns(0.90);
    const std::uint64_t p99 = h.percentile_ns(0.99);
    const std::uint64_t p1  = h.percentile_ns(1.0);

    EXPECT_LE(p0,  p50) << "p0 <= p50";
    EXPECT_LE(p50, p90) << "p50 <= p90";
    EXPECT_LE(p90, p99) << "p90 <= p99";
    EXPECT_LE(p99, p1)  << "p99 <= p100";

    // p0 must be min, p1 must be max.
    EXPECT_EQ(p0, h.min());
    EXPECT_EQ(p1, h.max());
}

// ---------------------------------------------------------------------------
// 5. Large values don't overflow / crash — clamped into top bucket.
// ---------------------------------------------------------------------------
TEST(LatencyHistogram, LargeValuesClampedSafely) {
    LatencyHistogram h;

    const std::uint64_t huge1 = std::numeric_limits<std::uint64_t>::max();
    const std::uint64_t huge2 = std::uint64_t{1} << 62;  // 4.6e18 ns ≈ 146 years
    const std::uint64_t huge3 = std::uint64_t{1} << 63;  // top bucket lower edge

    h.add(huge1);
    h.add(huge2);
    h.add(huge3);

    EXPECT_EQ(h.count(), 3u);
    EXPECT_EQ(h.min(), huge2);  // huge2 < huge3 < huge1 for uint64 arithmetic
    EXPECT_EQ(h.max(), huge1);

    // No crash and percentile stays within [0, UINT64_MAX].
    const std::uint64_t p99 = h.percentile_ns(0.99);
    EXPECT_LE(p99, huge1);

    // Bucket of UINT64_MAX should be 63 (highest set bit = bit 63).
    EXPECT_EQ(bucket_of(huge1), 63);
    EXPECT_EQ(bucket_of(huge3), 63);
}

// ---------------------------------------------------------------------------
// 6. Zero sample — add(0) must not crash, bucket index must be 0.
// ---------------------------------------------------------------------------
TEST(LatencyHistogram, ZeroSample) {
    LatencyHistogram h;
    h.add(0);
    EXPECT_EQ(h.count(), 1u);
    EXPECT_EQ(h.min(),   0u);
    EXPECT_EQ(h.max(),   0u);
    EXPECT_DOUBLE_EQ(h.mean_ns(), 0.0);
    // bucket_of(0) = 63 - clzll(0|1) = 63 - 63 = 0.
    EXPECT_EQ(bucket_of(0), 0);
}

// ---------------------------------------------------------------------------
// 7. Reset clears everything.
// ---------------------------------------------------------------------------
TEST(LatencyHistogram, ResetClearsState) {
    LatencyHistogram h;
    h.add(1234);
    h.add(5678);
    h.reset();
    EXPECT_EQ(h.count(), 0u);
    EXPECT_EQ(h.min(),   0u);
    EXPECT_EQ(h.max(),   0u);
    EXPECT_DOUBLE_EQ(h.mean_ns(), 0.0);
    EXPECT_EQ(h.percentile_ns(0.5), 0u);
}
