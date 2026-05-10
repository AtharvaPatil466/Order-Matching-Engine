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

    // Record using start/end timestamps
    void recordInterval(uint64_t startNs, uint64_t endNs) {
        if (endNs > startNs) record(endNs - startNs);
    }

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
    }

    void reset() {
        std::memset(buckets_, 0, sizeof(buckets_));
        count_ = 0;
        sum_ = 0;
        min_ = UINT64_MAX;
        max_ = 0;
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
};

} // namespace OrderMatcher
