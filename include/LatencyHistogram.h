#pragma once

// LatencyHistogram — standalone HdrHistogram-style latency histogram with
// coordinated-omission correction.
//
// Tier-1 upgrade: the engine's existing LatencyTracker reports
// percentiles that hide tail latency under *coordinated omission* (the
// Gil-Tene problem) — when a single operation stalls the measurement
// loop, the samples that "should" have been taken during the stall are
// never recorded, so p99/p99.9 look artificially good. This histogram
// fixes that by supporting an expected-interval correction that
// synthesizes the missed samples.
//
// Design goals:
//   - Engine-agnostic: depends only on Types.h + the standard library.
//   - Zero allocation on the record path: the bucket array is sized once
//     in the constructor; recordValue/recordValueWithExpectedInterval
//     never allocate.
//   - Standard HDR log-linear bucketing: a linear sub-bucket array per
//     power-of-two magnitude, sized from the requested significant
//     digits of precision. This bounds the relative quantization error
//     to <= 10^-sigDigits across the whole [1, maxValue] range.
//
// House style follows include/SelfTradeProtection.h: header-only,
// self-contained, OrderMatcher namespace.

#include "Types.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace OrderMatcher {

class LatencyHistogram {
public:
    // maxValue   = highest trackable value (clamped to >= 2). Values above
    //              this are clamped to maxValue on the record path.
    // sigDigits  = number of significant decimal digits of precision
    //              (1..3 typical, clamped to [1,5]). Higher precision uses
    //              more memory (more sub-buckets per magnitude).
    explicit LatencyHistogram(uint64_t maxValue, int sigDigits = 3)
        : highestTrackableValue_(maxValue < 2 ? 2 : maxValue) {
        if (sigDigits < 1) sigDigits = 1;
        if (sigDigits > 5) sigDigits = 5;
        significantDigits_ = sigDigits;

        // subBucketCount must be a power of two >= 2 * 10^sigDigits so that
        // the largest value in the smallest magnitude band still resolves
        // to the requested precision. subBucketHalfCount is the number of
        // distinct sub-buckets added by each successive power-of-two band.
        uint64_t largestValueWithSingleUnitResolution =
            2 * pow10(static_cast<uint32_t>(sigDigits));
        int subBucketCountMagnitude =
            ceilingLog2(largestValueWithSingleUnitResolution);
        subBucketHalfCountMagnitude_ =
            subBucketCountMagnitude > 1 ? subBucketCountMagnitude - 1 : 0;

        subBucketCount_ = 1u << (subBucketHalfCountMagnitude_ + 1);
        subBucketHalfCount_ = subBucketCount_ >> 1;
        subBucketMask_ = static_cast<uint64_t>(subBucketCount_) - 1;

        // Number of power-of-two magnitude bands needed to cover
        // highestTrackableValue_. Each band beyond the first adds
        // subBucketHalfCount_ counts.
        bucketCount_ = 1;
        uint64_t smallestUntrackable =
            static_cast<uint64_t>(subBucketCount_);
        while (smallestUntrackable <= highestTrackableValue_) {
            if (smallestUntrackable > (UINT64_MAX / 2)) {
                ++bucketCount_;
                break;
            }
            smallestUntrackable <<= 1;
            ++bucketCount_;
        }

        countsLen_ = (bucketCount_ + 1) * subBucketHalfCount_;
        counts_.assign(countsLen_, 0);
        reset();
    }

    // Record a single observed value. Clamped to [1, maxValue].
    void recordValue(uint64_t v) {
        recordValueWithCount(v, 1);
    }

    // Coordinated-omission correction. Records v, then — if v is larger
    // than expectedInterval — ALSO synthesizes the samples that were
    // "missed" while the system was stalled: v-expectedInterval,
    // v-2*expectedInterval, ... down to the last value strictly > 0. This
    // makes percentiles reflect the stall instead of hiding it.
    //
    // When expectedInterval is 0 (or v <= expectedInterval) this behaves
    // exactly like recordValue.
    void recordValueWithExpectedInterval(uint64_t v, uint64_t expectedInterval) {
        recordValueWithCount(v, 1);
        if (expectedInterval == 0 || v <= expectedInterval) {
            return;
        }
        // Backfill the omitted samples. Each missed measurement would have
        // observed a progressively smaller latency as the stall unwound.
        for (uint64_t missing = v - expectedInterval;
             missing >= expectedInterval && missing > 0;
             missing -= expectedInterval) {
            recordValueWithCount(missing, 1);
            if (missing < expectedInterval) break;  // guard underflow
        }
    }

    // Value at the given percentile p in [0,100]. Walks cumulative counts
    // and returns the highest-equivalent value of the containing bucket so
    // the result is stable across values that share a bucket.
    uint64_t valueAtPercentile(double p) const {
        if (totalCount_ == 0) return 0;
        if (p < 0.0) p = 0.0;
        if (p > 100.0) p = 100.0;

        // Number of samples at or below the target percentile. Rounding up
        // matches the HdrHistogram convention and keeps p=100 -> max.
        double requested = (p / 100.0) * static_cast<double>(totalCount_);
        uint64_t countAtPercentile =
            static_cast<uint64_t>(requested + 0.5);
        if (countAtPercentile == 0) countAtPercentile = 1;
        if (countAtPercentile > totalCount_) countAtPercentile = totalCount_;

        uint64_t cumulative = 0;
        for (size_t i = 0; i < countsLen_; ++i) {
            cumulative += counts_[i];
            if (cumulative >= countAtPercentile) {
                uint64_t valueAtIndex = valueFromIndex(i);
                return highestEquivalentValue(valueAtIndex);
            }
        }
        return max_;
    }

    uint64_t count() const { return totalCount_; }
    uint64_t min() const { return totalCount_ > 0 ? min_ : 0; }
    uint64_t max() const { return totalCount_ > 0 ? max_ : 0; }

    double mean() const {
        if (totalCount_ == 0) return 0.0;
        // Weight each occupied bucket by its representative (median-of-band)
        // value. Using the bucket midpoint rather than the raw recorded
        // value keeps mean() consistent with the quantization used for
        // percentiles.
        double total = 0.0;
        for (size_t i = 0; i < countsLen_; ++i) {
            if (counts_[i] != 0) {
                total += static_cast<double>(counts_[i]) *
                         static_cast<double>(medianEquivalentValue(valueFromIndex(i)));
            }
        }
        return total / static_cast<double>(totalCount_);
    }

    // Merge another histogram with identical configuration into this one.
    void merge(const LatencyHistogram& other) {
        if (other.countsLen_ != countsLen_ ||
            other.subBucketCount_ != subBucketCount_ ||
            other.bucketCount_ != bucketCount_) {
            // Mismatched configuration: re-quantize via value to stay safe.
            for (size_t i = 0; i < other.countsLen_; ++i) {
                if (other.counts_[i] != 0) {
                    recordValueWithCount(other.valueFromIndex(i),
                                         other.counts_[i]);
                }
            }
            return;
        }
        if (other.totalCount_ == 0) return;
        for (size_t i = 0; i < countsLen_; ++i) {
            counts_[i] += other.counts_[i];
        }
        totalCount_ += other.totalCount_;
        if (other.min_ < min_) min_ = other.min_;
        if (other.max_ > max_) max_ = other.max_;
    }

    void reset() {
        std::fill(counts_.begin(), counts_.end(), uint64_t{0});
        totalCount_ = 0;
        min_ = UINT64_MAX;
        max_ = 0;
    }

    // --- Introspection (handy for tests / diagnostics) ---
    uint64_t highestTrackableValue() const { return highestTrackableValue_; }
    int significantDigits() const { return significantDigits_; }
    size_t bucketCount() const { return countsLen_; }

private:
    // Core record. count is the multiplicity (1 for a single sample, N for
    // a merged or backfilled batch). Zero-allocation: only an array index
    // increment.
    void recordValueWithCount(uint64_t v, uint64_t count) {
        if (count == 0) return;
        if (v < 1) v = 1;
        if (v > highestTrackableValue_) v = highestTrackableValue_;

        size_t idx = countsIndexFor(v);
        counts_[idx] += count;
        totalCount_ += count;
        if (v < min_) min_ = v;
        if (v > max_) max_ = v;
    }

    // --- HDR index arithmetic ---

    int bucketIndexFor(uint64_t value) const {
        // Leading-zero count gives the power-of-two magnitude band. The
        // first subBucketHalfCountMagnitude_+1 bits live in band 0.
        int pow2ceiling = 64 - countLeadingZeros(value | subBucketMask_);
        return pow2ceiling - (static_cast<int>(subBucketHalfCountMagnitude_) + 1);
    }

    int subBucketIndexFor(uint64_t value, int bucketIndex) const {
        return static_cast<int>(value >> bucketIndex);
    }

    size_t countsIndexFor(uint64_t value) const {
        int bucketIndex = bucketIndexFor(value);
        int subBucketIndex = subBucketIndexFor(value, bucketIndex);
        return countsIndex(bucketIndex, subBucketIndex);
    }

    size_t countsIndex(int bucketIndex, int subBucketIndex) const {
        // Band 0 occupies [0, subBucketCount_); every later band contributes
        // its upper half (subBucketHalfCount_ entries) appended after band 0.
        int bucketBaseIndex =
            (bucketIndex + 1) << subBucketHalfCountMagnitude_;
        int offsetInBucket = subBucketIndex - static_cast<int>(subBucketHalfCount_);
        return static_cast<size_t>(bucketBaseIndex + offsetInBucket);
    }

    // Inverse: lowest value represented by a flat counts[] index.
    uint64_t valueFromIndex(size_t index) const {
        int bucketIndex =
            static_cast<int>(index >> subBucketHalfCountMagnitude_) - 1;
        int subBucketIndex =
            static_cast<int>(index & (subBucketHalfCount_ - 1)) +
            static_cast<int>(subBucketHalfCount_);
        if (bucketIndex < 0) {
            // Band 0 is special-cased: its sub-bucket index is the index.
            subBucketIndex -= static_cast<int>(subBucketHalfCount_);
            bucketIndex = 0;
        }
        return valueFromSubBucket(bucketIndex, subBucketIndex);
    }

    uint64_t valueFromSubBucket(int bucketIndex, int subBucketIndex) const {
        return static_cast<uint64_t>(subBucketIndex)
               << bucketIndex;
    }

    // Size of the quantization band that contains value.
    uint64_t sizeOfEquivalentValueRange(uint64_t value) const {
        int bucketIndex = bucketIndexFor(value);
        int subBucketIndex = subBucketIndexFor(value, bucketIndex);
        int adjustedBucket =
            (subBucketIndex >= static_cast<int>(subBucketCount_)) ? bucketIndex + 1
                                                                  : bucketIndex;
        return uint64_t{1} << adjustedBucket;
    }

    uint64_t lowestEquivalentValue(uint64_t value) const {
        int bucketIndex = bucketIndexFor(value);
        int subBucketIndex = subBucketIndexFor(value, bucketIndex);
        return valueFromSubBucket(bucketIndex, subBucketIndex);
    }

    uint64_t highestEquivalentValue(uint64_t value) const {
        return lowestEquivalentValue(value) +
               sizeOfEquivalentValueRange(value) - 1;
    }

    uint64_t medianEquivalentValue(uint64_t value) const {
        return lowestEquivalentValue(value) +
               (sizeOfEquivalentValueRange(value) >> 1);
    }

    // --- small numeric helpers ---

    static int countLeadingZeros(uint64_t v) {
        if (v == 0) return 64;
        return __builtin_clzll(v);
    }

    static int ceilingLog2(uint64_t v) {
        int n = 0;
        uint64_t p = 1;
        while (p < v) {
            p <<= 1;
            ++n;
        }
        return n;
    }

    static uint64_t pow10(uint32_t exp) {
        uint64_t r = 1;
        for (uint32_t i = 0; i < exp; ++i) r *= 10;
        return r;
    }

    // Configuration (set once in the constructor).
    uint64_t highestTrackableValue_;
    int significantDigits_;
    int subBucketHalfCountMagnitude_;
    uint32_t subBucketCount_;
    uint32_t subBucketHalfCount_;
    uint64_t subBucketMask_;
    size_t bucketCount_;
    size_t countsLen_;

    // State.
    std::vector<uint64_t> counts_;
    uint64_t totalCount_;
    uint64_t min_;
    uint64_t max_;
};

}  // namespace OrderMatcher
