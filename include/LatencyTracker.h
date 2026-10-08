#pragma once

#include <atomic>
#include <cstdint>
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
//
// SINGLE WRITER, ANY NUMBER OF READERS (BENCH-8). Every field is an atomic
// written with relaxed load+store by the one thread that records — no RMW, so
// the hot path costs what plain increments did — and read with relaxed loads,
// so another thread may copy or merge a live tracker (the admin /metrics
// endpoint, a benchmark's sampling loop) without a data race. Such a read is a
// snapshot that may be a few samples behind in some buckets, never torn
// within one. Two threads must never record into the same tracker.
class LatencyTracker {
public:
    static constexpr size_t NUM_BUCKETS = 4096;
    static constexpr uint64_t MAX_TRACKABLE_NS = 4'000'000'000ULL; // 4 seconds

    LatencyTracker() { reset(); }
    LatencyTracker(const LatencyTracker& other) { copyFrom(other); }
    LatencyTracker& operator=(const LatencyTracker& other) {
        if (this != &other) copyFrom(other);
        return *this;
    }

    void record(uint64_t latencyNs) {
        bump(buckets_[toBucket(latencyNs)], 1);
        bump(count_, 1);
        bump(sum_, latencyNs);
        if (latencyNs < get(min_)) set(min_, latencyNs);
        if (latencyNs > get(max_)) set(max_, latencyNs);
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
        if (endNs == startNs) { bump(subTick_, 1); record(0); return; }
        bump(clockAnomalies_, 1);
    }

    // Samples completing within one clock tick — counted, value unresolvable.
    uint64_t getSubTickCount() const { return get(subTick_); }
    // Intervals where the clock ran backwards; should be 0.
    uint64_t getClockAnomalies() const { return get(clockAnomalies_); }

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
        const uint64_t count = get(count_);
        if (count == 0) return 0;
        uint64_t target = static_cast<uint64_t>(p * count);
        uint64_t cumulative = 0;
        for (size_t i = 0; i < NUM_BUCKETS; i++) {
            cumulative += get(buckets_[i]);
            if (cumulative > target) {
                return fromBucket(i);
            }
        }
        return get(max_);
    }

    uint64_t getP50() const { return getPercentile(0.50); }
    uint64_t getP90() const { return getPercentile(0.90); }
    uint64_t getP99() const { return getPercentile(0.99); }
    uint64_t getP999() const { return getPercentile(0.999); }
    uint64_t getMin() const { return get(count_) > 0 ? get(min_) : 0; }
    uint64_t getMax() const { return get(max_); }
    uint64_t getCount() const { return get(count_); }

    double getMean() const {
        const uint64_t count = get(count_);
        return count > 0 ? static_cast<double>(get(sum_)) / count : 0.0;
    }

    // `other` may be live (being recorded into by its owner); this tracker is
    // written, so it must be one this thread owns.
    void mergeFrom(const LatencyTracker& other) {
        if (get(other.count_) == 0) {
            return;
        }

        // Count from the buckets actually added, as copyFrom does.
        uint64_t added = 0;
        for (size_t i = 0; i < NUM_BUCKETS; ++i) {
            const uint64_t n = get(other.buckets_[i]);
            bump(buckets_[i], n);
            added += n;
        }

        bump(count_, added);
        bump(sum_, get(other.sum_));
        set(min_, std::min(get(min_), get(other.min_)));
        set(max_, std::max(get(max_), get(other.max_)));
        bump(subTick_, get(other.subTick_));
        bump(clockAnomalies_, get(other.clockAnomalies_));
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
        // Guard rather than assume: `earlier` must be a snapshot of THIS
        // tracker, and if a caller passes something else, saturate at zero
        // instead of wrapping the unsigned subtraction into a huge count.
        auto minus = [](const Counter& a, const Counter& b) {
            return get(a) >= get(b) ? get(a) - get(b) : 0;
        };
        LatencyTracker d;
        for (size_t i = 0; i < NUM_BUCKETS; ++i) {
            set(d.buckets_[i], minus(buckets_[i], earlier.buckets_[i]));
            bump(d.count_, get(d.buckets_[i]));
        }
        set(d.sum_, minus(sum_, earlier.sum_));
        set(d.subTick_, minus(subTick_, earlier.subTick_));
        set(d.clockAnomalies_, minus(clockAnomalies_, earlier.clockAnomalies_));
        set(d.min_, 0);   // not recoverable by subtraction — see above
        set(d.max_, 0);
        return d;
    }

    void reset() {
        for (Counter& b : buckets_) set(b, 0);
        set(count_, 0);
        set(sum_, 0);
        set(min_, UINT64_MAX);
        set(max_, 0);
        set(subTick_, 0);
        set(clockAnomalies_, 0);
    }

private:
    using Counter = std::atomic<uint64_t>;
    static uint64_t get(const Counter& c) { return c.load(std::memory_order_relaxed); }
    static void set(Counter& c, uint64_t v) { c.store(v, std::memory_order_relaxed); }
    // Load + store, not fetch_add: one writer per tracker, so no RMW is needed.
    static void bump(Counter& c, uint64_t by) { set(c, get(c) + by); }

    // A snapshot of `other`, which may be live. The count is re-derived from
    // the copied buckets so percentiles on the copy are self-consistent even
    // when the owner records in the middle of the copy.
    void copyFrom(const LatencyTracker& other) {
        uint64_t count = 0;
        for (size_t i = 0; i < NUM_BUCKETS; ++i) {
            set(buckets_[i], get(other.buckets_[i]));
            count += get(buckets_[i]);
        }
        set(count_, count);
        set(sum_, get(other.sum_));
        set(min_, get(other.min_));
        set(max_, get(other.max_));
        set(subTick_, get(other.subTick_));
        set(clockAnomalies_, get(other.clockAnomalies_));
    }

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

    Counter buckets_[NUM_BUCKETS];
    Counter count_;
    Counter sum_;
    Counter min_;
    Counter max_;
    Counter subTick_{0};
    Counter clockAnomalies_{0};
};

} // namespace OrderMatcher
