// BenchLatencyRecorder — vendored, header-only latency histogram for benchmarks.
//
// ZERO external dependencies. Lives under benchmarks/ so the benchmark harness
// can report the full tail (P50/P90/P99/P99.9/P99.99/max) without pulling in
// HdrHistogram_c or any third-party library, and without depending on the
// engine's own include/LatencyTracker.h (which stops at P99.9).
//
// Design: log-linear ("HdrHistogram-lite") histogram.
//   • Values [0, LINEAR_LIMIT)   → 1 ns-resolution linear buckets. This gives
//     exact resolution for the hot path, where P50/P90/P99 typically land in
//     the tens-to-few-thousand ns range.
//   • Values >= LINEAR_LIMIT     → each power-of-two octave is split into
//     SUB_BUCKETS linear sub-buckets, bounding the relative error of any
//     reported percentile to <= 1 / SUB_BUCKETS (~0.78% here). This covers the
//     ns → ms → s range that fdatasync / cold-cache tails reach into.
//
// Recording is single-writer, allocation-free, and branch-light. Percentiles
// use nearest-rank over the cumulative bucket counts; min/max/mean are tracked
// exactly from the raw samples.
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace bench {

// High-resolution nanosecond clock. Self-contained so the recorder does not
// depend on the engine's nowNs(). Steady clock — monotonic, immune to wall
// clock adjustments during a run.
inline uint64_t nowNs() {
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

class BenchLatencyRecorder {
public:
    // Linear region: [0, 4096) ns at 1 ns resolution.
    static constexpr uint64_t LINEAR_BITS = 12;
    static constexpr uint64_t LINEAR_LIMIT = 1ULL << LINEAR_BITS; // 4096
    // Sub-buckets per octave above the linear region (128 → <=0.78% error).
    static constexpr uint64_t SUB_BITS = 7;
    static constexpr uint64_t SUB_BUCKETS = 1ULL << SUB_BITS; // 128
    // Octaves for msb in [LINEAR_BITS, 63]. 64 - 12 = 52 octaves covers any
    // uint64 nanosecond value (2^63 ns ≈ 292 years) without clamping.
    static constexpr uint64_t NUM_OCTAVES = 64 - LINEAR_BITS;
    static constexpr size_t NUM_BUCKETS =
        static_cast<size_t>(LINEAR_LIMIT + NUM_OCTAVES * SUB_BUCKETS);

    BenchLatencyRecorder() { reset(); }

    void record(uint64_t latencyNs) {
        buckets_[toBucket(latencyNs)]++;
        count_++;
        sum_ += latencyNs;
        if (latencyNs < min_) min_ = latencyNs;
        if (latencyNs > max_) max_ = latencyNs;
    }

    // Record an interval; ignores non-positive spans (clock went backwards or
    // sub-tick resolution).
    void recordInterval(uint64_t startNs, uint64_t endNs) {
        if (endNs > startNs) record(endNs - startNs);
    }

    // RAII scope timer for ad-hoc measurement.
    class ScopeTimer {
    public:
        explicit ScopeTimer(BenchLatencyRecorder& rec) : rec_(rec), start_(nowNs()) {}
        ~ScopeTimer() { rec_.recordInterval(start_, nowNs()); }
    private:
        BenchLatencyRecorder& rec_;
        uint64_t start_;
    };
    ScopeTimer scope() { return ScopeTimer(*this); }

    // Nearest-rank percentile. p in [0,1].
    uint64_t percentile(double p) const {
        if (count_ == 0) return 0;
        if (p <= 0.0) return min_;
        if (p >= 1.0) return max_;
        uint64_t rank = static_cast<uint64_t>(std::ceil(p * static_cast<double>(count_)));
        if (rank == 0) rank = 1;
        uint64_t cumulative = 0;
        for (size_t i = 0; i < NUM_BUCKETS; ++i) {
            cumulative += buckets_[i];
            if (cumulative >= rank) return fromBucket(i);
        }
        return max_;
    }

    uint64_t getP50()   const { return percentile(0.50); }
    uint64_t getP90()   const { return percentile(0.90); }
    uint64_t getP99()   const { return percentile(0.99); }
    uint64_t getP999()  const { return percentile(0.999); }
    uint64_t getP9999() const { return percentile(0.9999); }
    uint64_t getMin()   const { return count_ > 0 ? min_ : 0; }
    uint64_t getMax()   const { return max_; }
    uint64_t getCount() const { return count_; }
    double   getMean()  const {
        return count_ > 0 ? static_cast<double>(sum_) / static_cast<double>(count_) : 0.0;
    }

    void mergeFrom(const BenchLatencyRecorder& other) {
        if (other.count_ == 0) return;
        for (size_t i = 0; i < NUM_BUCKETS; ++i) buckets_[i] += other.buckets_[i];
        count_ += other.count_;
        sum_ += other.sum_;
        min_ = std::min(min_, other.min_);
        max_ = std::max(max_, other.max_);
    }

    void reset() {
        buckets_.fill(0);
        count_ = 0;
        sum_ = 0;
        min_ = UINT64_MAX;
        max_ = 0;
    }

    // Print the standard percentile table. `label` names the measured path.
    // Every benchmark path routes through here so the columns are identical
    // across the whole harness (P2-21).
    void printTable(const char* label) const {
        std::printf("  %-14s samples=%llu\n", label, (unsigned long long)count_);
        std::printf("    Min:     %10llu ns\n", (unsigned long long)getMin());
        std::printf("    P50:     %10llu ns\n", (unsigned long long)getP50());
        std::printf("    P90:     %10llu ns\n", (unsigned long long)getP90());
        std::printf("    P99:     %10llu ns\n", (unsigned long long)getP99());
        std::printf("    P99.9:   %10llu ns\n", (unsigned long long)getP999());
        std::printf("    P99.99:  %10llu ns\n", (unsigned long long)getP9999());
        std::printf("    Max:     %10llu ns\n", (unsigned long long)getMax());
        std::printf("    Mean:    %10.0f ns\n", getMean());
    }

private:
    // Map a nanosecond value to a bucket index.
    static size_t toBucket(uint64_t ns) {
        if (ns < LINEAR_LIMIT) return static_cast<size_t>(ns);
        uint64_t msb = 63 - static_cast<uint64_t>(__builtin_clzll(ns));
        uint64_t octave = msb - LINEAR_BITS;
        uint64_t sub = (ns >> (msb - SUB_BITS)) & (SUB_BUCKETS - 1);
        size_t bucket = static_cast<size_t>(LINEAR_LIMIT + octave * SUB_BUCKETS + sub);
        return std::min(bucket, NUM_BUCKETS - 1);
    }

    // Reconstruct the lower edge of a bucket (conservative representative).
    static uint64_t fromBucket(size_t bucket) {
        if (bucket < LINEAR_LIMIT) return static_cast<uint64_t>(bucket);
        uint64_t idx = static_cast<uint64_t>(bucket) - LINEAR_LIMIT;
        uint64_t octave = idx / SUB_BUCKETS;
        uint64_t sub = idx % SUB_BUCKETS;
        uint64_t msb = octave + LINEAR_BITS;
        return (1ULL << msb) | (sub << (msb - SUB_BITS));
    }

    std::array<uint64_t, NUM_BUCKETS> buckets_{};
    uint64_t count_{0};
    uint64_t sum_{0};
    uint64_t min_{UINT64_MAX};
    uint64_t max_{0};
};

} // namespace bench
