#pragma once

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace OrderMatcher {

// High-resolution nanosecond timestamp
inline uint64_t nowNs() {
    return static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
}

// Fixed-size HDR-style latency histogram (lock-free, single-writer)
// Tracks latencies from 0 to ~4 billion nanoseconds (~4 seconds) in 1ns resolution
// Uses log-linear bucketing for memory efficiency
class LatencyTracker {
public:
    static constexpr size_t NUM_BUCKETS = 4096;
    static constexpr uint64_t MAX_TRACKABLE_NS = 4'000'000'000ULL; // 4 seconds

    LatencyTracker() { reset(); }

    void record(uint64_t latencyNs) {
        size_t bucket = toBucket(latencyNs);
        buckets_[bucket]++;
        count_++;
        sum_ += latencyNs;
        if (latencyNs < min_) min_ = latencyNs;
        if (latencyNs > max_) max_ = latencyNs;
    }

    // Record using start/end timestamps.
    //
    // A SUB-TICK OPERATION IS COUNTED, NOT DISCARDED. This used to read
    // `if (endNs > startNs) record(...)`, so anything completing inside one
    // clock tick left no sample, which biases every percentile upward — the
    // fastest operations are precisely the ones that disappear. On a coarse
    // clock that is not a rounding detail: the same defect in the benchmark
    // harness was dropping 26-32% of cancel samples.
    //
    // 0 is recorded because it is the only thing known: the true duration is
    // somewhere in [0, tick). The value matters little — sub-tick samples take
    // the lowest ranks regardless, so percentiles above the band are correctly
    // ranked either way. The COUNT is what was broken.
    //
    // endNs < startNs is still not recorded. nowNs() is monotonic, so that is a
    // broken measurement rather than a fast one; it is counted separately.
    void recordInterval(uint64_t startNs, uint64_t endNs) {
        if (endNs > startNs) { record(endNs - startNs); return; }
        if (endNs == startNs) { ++subTick_; record(0); return; }
        ++clockAnomalies_;
    }

    // Samples completing within one clock tick — counted, value unresolvable.
    uint64_t getSubTickCount() const { return subTick_; }
    // Intervals where the clock ran backwards; should be 0.
    uint64_t getClockAnomalies() const { return clockAnomalies_; }

    // RAII scope timer
    class ScopeTimer {
    public:
        ScopeTimer(LatencyTracker& tracker) : tracker_(tracker), start_(nowNs()) {}
        ~ScopeTimer() { tracker_.recordInterval(start_, nowNs()); }
    private:
        LatencyTracker& tracker_;
        uint64_t start_;
    };

    ScopeTimer scope() { return ScopeTimer(*this); }

    // Percentile queries
    uint64_t getPercentile(double p) const {
        if (count_ == 0) return 0;
        uint64_t target = static_cast<uint64_t>(p * count_);
        uint64_t cumulative = 0;
        for (size_t i = 0; i < NUM_BUCKETS; i++) {
            cumulative += buckets_[i];
            if (cumulative > target) {
                return fromBucket(i);
            }
        }
        return max_;
    }

    uint64_t getP50() const { return getPercentile(0.50); }
    uint64_t getP90() const { return getPercentile(0.90); }
    uint64_t getP99() const { return getPercentile(0.99); }
    uint64_t getP999() const { return getPercentile(0.999); }
    uint64_t getMin() const { return count_ > 0 ? min_ : 0; }
    uint64_t getMax() const { return max_; }
    uint64_t getCount() const { return count_; }

    double getMean() const {
        return count_ > 0 ? static_cast<double>(sum_) / count_ : 0.0;
    }

    void mergeFrom(const LatencyTracker& other) {
        if (other.count_ == 0) {
            return;
        }

        for (size_t i = 0; i < NUM_BUCKETS; ++i) {
            buckets_[i] += other.buckets_[i];
        }

        count_ += other.count_;
        sum_ += other.sum_;
        min_ = std::min(min_, other.min_);
        max_ = std::max(max_, other.max_);
        subTick_ += other.subTick_;
        clockAnomalies_ += other.clockAnomalies_;
    }

    // This tracker MINUS an earlier snapshot of itself — the distribution of
    // whatever was recorded between the two.
    //
    // WHY THIS EXISTS. The engine's per-thread trackers are never reset, so
    // getAggregateE2ELatency() always returns the distribution since process
    // start. Sampling it once per window and calling the result "this window"
    // is wrong in a way that looks plausible: the reported percentiles decay
    // monotonically across a run as the cumulative population grows, which
    // reads like the system warming up when it is really just an average over
    // an ever-longer history. SustainedLoadTest did exactly this.
    //
    // Bucket counts subtract cleanly, so count and every percentile on the
    // result are exact for the interval.
    //
    // min/max DO NOT subtract — knowing the all-time min tells you nothing
    // about the window's min — so they are deliberately left cleared rather
    // than carrying a misleading value through. Use the percentiles and the
    // count; getMin()/getMax() on a delta are meaningless and return 0.
    LatencyTracker deltaFrom(const LatencyTracker& earlier) const {
        LatencyTracker d;
        for (size_t i = 0; i < NUM_BUCKETS; ++i) {
            // Guard rather than assume: `earlier` must be a snapshot of THIS
            // tracker, and if a caller passes something else, saturate at zero
            // instead of wrapping the unsigned subtraction into a huge count.
            d.buckets_[i] = buckets_[i] >= earlier.buckets_[i]
                                ? buckets_[i] - earlier.buckets_[i] : 0;
            d.count_ += d.buckets_[i];
        }
        d.sum_ = sum_ >= earlier.sum_ ? sum_ - earlier.sum_ : 0;
        d.subTick_ = subTick_ >= earlier.subTick_ ? subTick_ - earlier.subTick_ : 0;
        d.clockAnomalies_ = clockAnomalies_ >= earlier.clockAnomalies_
                                ? clockAnomalies_ - earlier.clockAnomalies_ : 0;
        d.min_ = 0;   // not recoverable by subtraction — see above
        d.max_ = 0;
        return d;
    }

    void reset() {
        std::memset(buckets_, 0, sizeof(buckets_));
        count_ = 0;
        sum_ = 0;
        min_ = UINT64_MAX;
        max_ = 0;
        subTick_ = 0;
        clockAnomalies_ = 0;
    }

private:
    // Log-linear bucketing: linear up to 1024ns, then log2-based
    static size_t toBucket(uint64_t ns) {
        if (ns < 1024) return ns;
        // Log2-based bucketing for larger values
        size_t msb = 63 - __builtin_clzll(ns | 1);
        size_t base = 1024 + (msb - 10) * 128;
        size_t fraction = (ns >> (msb - 7)) & 0x7F;
        size_t bucket = base + fraction;
        return std::min(bucket, NUM_BUCKETS - 1);
    }

    static uint64_t fromBucket(size_t bucket) {
        if (bucket < 1024) return bucket;
        size_t group = (bucket - 1024) / 128;
        size_t fraction = (bucket - 1024) % 128;
        size_t msb = group + 10;
        return (1ULL << msb) | (static_cast<uint64_t>(fraction) << (msb - 7));
    }

    uint64_t buckets_[NUM_BUCKETS];
    uint64_t count_;
    uint64_t sum_;
    uint64_t min_;
    uint64_t max_;
    uint64_t subTick_{0};
    uint64_t clockAnomalies_{0};
};

} // namespace OrderMatcher
