#pragma once

// LatencyHistogram — fixed-memory, allocation-free latency histogram.
//
// Bucket scheme: power-of-two (log2) bucketing via the index of the highest
// set bit of the nanosecond value.  Specifically:
//
//   bucket(ns) = 63 - __builtin_clzll(ns | 1)
//              = floor(log2(max(ns, 1)))
//
// This gives 64 buckets (indices 0–63) covering:
//   bucket 0  : [0, 1] ns           (0 or 1 ns)
//   bucket 1  : [2, 3] ns
//   bucket 10 : [1 024, 2 047] ns   (~1 µs)
//   bucket 20 : [1 048 576, 2 097 151] ns (~1 ms)
//   bucket 30 : [1 073 741 824, …] ns (~1 s)
//   bucket 63 : [2^63, UINT64_MAX]  (clamped)
//
// Resolution / accuracy trade-off:
//   - Percentiles are bucket-edge approximate; the upper-edge error is at
//     most 2× (one power-of-two) within any bucket.
//   - For the common low-latency range (100 ns–100 µs, buckets 6–16) each
//     bucket spans only one power-of-two, so p50/p99 are accurate to within
//     the width of the nearest bucket (e.g. bucket 10 = ±1 µs).
//   - min, max, mean, and count are tracked exactly via separate accumulators.
//   - The hot path (add) is: one OR, one clzll, one array increment, three
//     integer comparisons/assignments, one 64-bit addition — no branches on
//     the fast path beyond what the CPU predicts trivially.
//   - Zero allocations; all storage is inline in the struct.

#include <cstdint>
#include <limits>

#include "order_book/cache.hpp"

namespace ob {

class LatencyHistogram {
public:
    static constexpr int kBuckets = 64;  // one per bit position of uint64_t

    // Reset to empty state.
    void reset() noexcept {
        for (auto& b : buckets_) b = 0;
        count_ = 0;
        sum_   = 0;
        min_   = std::numeric_limits<std::uint64_t>::max();
        max_   = 0;
    }

    LatencyHistogram() noexcept { reset(); }

    // Record one latency sample (nanoseconds).
    // O(1), no allocation, no floating-point.
    OB_ALWAYS_INLINE void add(std::uint64_t ns) noexcept {
        // bucket = floor(log2(max(ns,1))) via highest-set-bit index
        const int b = 63 - __builtin_clzll(ns | std::uint64_t{1});
        // b is always in [0, 63] — safe, no bounds check needed.
        ++buckets_[b];
        ++count_;
        sum_ += ns;
        if (OB_UNLIKELY(ns < min_)) min_ = ns;
        if (OB_UNLIKELY(ns > max_)) max_ = ns;
    }

    // Total number of samples recorded.
    std::uint64_t count() const noexcept { return count_; }

    // Exact minimum sample (0 if empty).
    std::uint64_t min() const noexcept {
        return (count_ == 0) ? 0 : min_;
    }

    // Exact maximum sample (0 if empty).
    std::uint64_t max() const noexcept {
        return (count_ == 0) ? 0 : max_;
    }

    // Arithmetic mean in nanoseconds (0.0 if empty).
    double mean_ns() const noexcept {
        if (count_ == 0) return 0.0;
        return static_cast<double>(sum_) / static_cast<double>(count_);
    }

    // Approximate p-th percentile in nanoseconds (p in [0.0, 1.0]).
    // Returns the lower edge of the bucket that contains the p-th rank.
    // Returns 0 if the histogram is empty.
    std::uint64_t percentile_ns(double p) const noexcept {
        if (count_ == 0) return 0;

        // Clamp p to [0, 1].
        if (p <= 0.0) return min_;
        if (p >= 1.0) return max_;

        // Target rank (1-based): we want the rank-th sample.
        // Use ceil so that p=0.99 on 100 samples gives rank 99.
        const std::uint64_t target =
            static_cast<std::uint64_t>(p * static_cast<double>(count_) + 0.5);
        const std::uint64_t clamped = (target == 0) ? 1
                                    : (target > count_) ? count_
                                    : target;

        std::uint64_t running = 0;
        for (int b = 0; b < kBuckets; ++b) {
            running += buckets_[b];
            if (running >= clamped) {
                // Lower edge of bucket b = 2^b (except bucket 0 which is 0–1).
                return (b == 0) ? std::uint64_t{0}
                                : (std::uint64_t{1} << b);
            }
        }
        // Unreachable if count_ > 0, but be safe.
        return max_;
    }

private:
    std::uint64_t buckets_[kBuckets]{};
    std::uint64_t count_{0};
    std::uint64_t sum_{0};
    std::uint64_t min_{std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t max_{0};
};

}  // namespace ob
