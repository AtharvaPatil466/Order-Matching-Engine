// TestLatencyHistogram — tests for the HdrHistogram-style latency
// histogram with coordinated-omission correction (include/LatencyHistogram.h).

#include "LatencyHistogram.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>

using namespace OrderMatcher;

static int passed = 0;
#define TEST(name) std::cout << "  " << #name << "..." << std::flush
#define PASS() do { ++passed; std::cout << " PASS\n"; } while(0)

// Percentiles on a known uniform distribution must land within the
// histogram's quantization precision.
void test_uniform_percentiles() {
    TEST(UniformPercentiles);

    // 1..10000 each recorded once. With sigDigits=3 the relative error is
    // bounded to 0.1% of the value, so a tiny absolute tolerance suffices.
    LatencyHistogram h(100000, 3);
    for (uint64_t v = 1; v <= 10000; ++v) {
        h.recordValue(v);
    }

    assert(h.count() == 10000);
    assert(h.min() == 1);
    assert(h.max() == 10000);

    // For a uniform 1..N distribution, the p-th percentile ~= p/100 * N.
    auto withinPrecision = [](uint64_t got, uint64_t expected) {
        // sigDigits=3 -> <=0.1% relative error; allow a small slack for the
        // discrete distribution and bucket-boundary rounding.
        double tol = 0.01 * static_cast<double>(expected) + 2.0;
        return std::fabs(static_cast<double>(got) -
                         static_cast<double>(expected)) <= tol;
    };

    assert(withinPrecision(h.valueAtPercentile(50.0), 5000));
    assert(withinPrecision(h.valueAtPercentile(90.0), 9000));
    assert(withinPrecision(h.valueAtPercentile(99.0), 9900));
    assert(withinPrecision(h.valueAtPercentile(99.9), 9990));
    assert(h.valueAtPercentile(100.0) >= 10000);
    assert(h.valueAtPercentile(0.0) >= 1);

    PASS();
}

// The quantization error bound: any recorded value read back at its
// percentile must be within sigDigits relative precision of the input.
void test_precision_bound() {
    TEST(PrecisionBound);

    for (int sig = 1; sig <= 3; ++sig) {
        LatencyHistogram h(1'000'000, sig);
        double maxRelErr = std::pow(10.0, -sig);
        const uint64_t testValues[] = {1, 7, 99, 1000, 12345, 999999};
        for (uint64_t v : testValues) {
            LatencyHistogram one(1'000'000, sig);
            one.recordValue(v);
            uint64_t readBack = one.valueAtPercentile(50.0);
            double relErr = std::fabs(static_cast<double>(readBack) -
                                      static_cast<double>(v)) /
                            static_cast<double>(v);
            assert(relErr <= maxRelErr);
        }
        (void)h;
    }

    PASS();
}

// recordValueWithExpectedInterval with a single large stall must produce a
// materially higher p99 than plain recordValue of the same samples.
void test_coordinated_omission() {
    TEST(CoordinatedOmission);

    const uint64_t expectedInterval = 100;     // steady-state cadence
    const uint64_t goodSamples = 10000;         // normal fast operations
    const uint64_t fast = 100;                  // typical good latency
    const uint64_t stall = 1'000'000;           // one big 1ms stall

    // Without correction: 10000 fast samples + one stall. The single stall
    // is buried far out in the tail; p99 stays at the fast value.
    LatencyHistogram plain(10'000'000, 3);
    for (uint64_t i = 0; i < goodSamples; ++i) plain.recordValue(fast);
    plain.recordValue(stall);

    // With correction: the same recorded values, but the stall is expanded
    // into the ~10000 samples that coordinated omission would have dropped,
    // dragging the tail up dramatically.
    LatencyHistogram corrected(10'000'000, 3);
    for (uint64_t i = 0; i < goodSamples; ++i) {
        corrected.recordValueWithExpectedInterval(fast, expectedInterval);
    }
    corrected.recordValueWithExpectedInterval(stall, expectedInterval);

    uint64_t plainP99 = plain.valueAtPercentile(99.0);
    uint64_t correctedP99 = corrected.valueAtPercentile(99.0);

    // Plain p99 is dominated by the fast samples (the lone stall is < 1%).
    assert(plainP99 <= fast * 2);

    // Correction backfills ~10000 synthetic samples spanning the stall, so
    // the tail rises by orders of magnitude.
    assert(correctedP99 > plainP99 * 100);

    // Sanity: the correction injected the missed samples (count grew well
    // beyond the explicitly recorded ones).
    assert(corrected.count() > plain.count() + 1000);

    // Correction must not move the max beyond the real worst observation.
    assert(corrected.max() <= stall + 1);
    assert(corrected.max() >= stall - (stall / 1000) - 1);

    PASS();
}

// recordValueWithExpectedInterval with no stall (v <= expectedInterval, or
// interval 0) behaves exactly like recordValue.
void test_no_stall_is_plain_record() {
    TEST(NoStallIsPlainRecord);

    LatencyHistogram a(100000, 3);
    LatencyHistogram b(100000, 3);
    for (uint64_t v = 1; v <= 1000; ++v) {
        a.recordValue(v);
        // v never exceeds the interval, so no backfill should happen.
        b.recordValueWithExpectedInterval(v, 2000);
    }
    assert(a.count() == b.count());
    assert(a.valueAtPercentile(99.0) == b.valueAtPercentile(99.0));

    // expectedInterval == 0 disables correction entirely.
    LatencyHistogram c(100000, 3);
    c.recordValueWithExpectedInterval(50000, 0);
    assert(c.count() == 1);

    PASS();
}

// min / max / mean / count basics.
void test_min_max_mean_count() {
    TEST(MinMaxMeanCount);

    LatencyHistogram h(100000, 3);
    assert(h.count() == 0);
    assert(h.min() == 0);   // empty -> 0
    assert(h.max() == 0);
    assert(h.mean() == 0.0);

    h.recordValue(10);
    h.recordValue(20);
    h.recordValue(30);
    h.recordValue(40);

    assert(h.count() == 4);
    assert(h.min() == 10);
    assert(h.max() == 40);
    // Mean of {10,20,30,40} = 25; quantization at these magnitudes is exact
    // for sigDigits=3, so the bucketed mean is very close to 25.
    assert(std::fabs(h.mean() - 25.0) <= 0.5);

    // Clamping: values above maxValue clamp to maxValue, below 1 clamp to 1.
    LatencyHistogram clamp(1000, 3);
    clamp.recordValue(0);          // -> 1
    clamp.recordValue(5000);       // -> 1000
    assert(clamp.min() == 1);
    assert(clamp.max() == 1000);
    assert(clamp.count() == 2);

    PASS();
}

// merge of two histograms sums counts and combines min/max.
void test_merge_sums_counts() {
    TEST(MergeSumsCounts);

    LatencyHistogram a(100000, 3);
    LatencyHistogram b(100000, 3);

    for (uint64_t v = 1; v <= 500; ++v) a.recordValue(v);
    for (uint64_t v = 501; v <= 1000; ++v) b.recordValue(v);

    uint64_t before = a.count();
    a.merge(b);

    assert(a.count() == before + b.count());
    assert(a.count() == 1000);
    assert(a.min() == 1);
    assert(a.max() == 1000);

    // Merged percentile must reflect the union, not either half alone.
    uint64_t p50 = a.valueAtPercentile(50.0);
    assert(p50 >= 480 && p50 <= 520);

    // Merging an empty histogram is a no-op.
    LatencyHistogram empty(100000, 3);
    uint64_t c = a.count();
    a.merge(empty);
    assert(a.count() == c);

    PASS();
}

// reset clears all state.
void test_reset_clears() {
    TEST(ResetClears);

    LatencyHistogram h(100000, 3);
    for (uint64_t v = 1; v <= 1000; ++v) h.recordValue(v);
    assert(h.count() == 1000);

    h.reset();
    assert(h.count() == 0);
    assert(h.min() == 0);
    assert(h.max() == 0);
    assert(h.mean() == 0.0);
    assert(h.valueAtPercentile(99.0) == 0);

    // Usable again after reset.
    h.recordValue(42);
    assert(h.count() == 1);
    assert(h.min() == 42);

    PASS();
}

int main() {
    std::cout << "\n=== LatencyHistogram Tests ===\n\n";

    test_uniform_percentiles();
    test_precision_bound();
    test_coordinated_omission();
    test_no_stall_is_plain_record();
    test_min_max_mean_count();
    test_merge_sums_counts();
    test_reset_clears();

    std::cout << "\n--- Results: " << passed << " passed ---\n";
    std::cout << "\nAll latency histogram tests passed.\n\n";
    return 0;
}
