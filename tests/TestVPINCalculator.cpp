// TestVPINCalculator.cpp — GoogleTest suite for include/VPINCalculator.h.
//
// Coverage:
//  1.  No trades → VPIN = 0, completedBuckets = 0
//  2.  Balanced flow (equal buy/sell, zero price change): VPIN ≈ 0 after many buckets
//  3.  One-sided flow (all buys, monotone price): VPIN ≈ 1 after many buckets
//  4.  Bucket boundary split: single large trade crossing a boundary → correct count
//  5.  Phi(0) = 0.5: zero price change → 50/50 split
//  6.  Phi(large positive z) ≈ 1.0: strong price up → nearly all buy volume
//  7.  Phi(large negative z) ≈ 0.0: strong price down → nearly all sell volume
//  8.  sigmaCurrent() updates with each trade
//  9.  reset() clears all state
//  10. windowBuckets constraint: vpin() = 0 until windowBuckets buckets close
//  11. VPIN ∈ [0, 1]: never outside range for any input

#include "VPINCalculator.h"

#include <cmath>
#include <gtest/gtest.h>

using namespace OrderMatcher;

// ─── 1. No trades ────────────────────────────────────────────────────────────

TEST(VPINCalculator, NoTradesReturnsZero) {
    VPINCalculator calc(1000.0, 5, 1.0);
    EXPECT_DOUBLE_EQ(calc.vpin(), 0.0);
    EXPECT_EQ(calc.completedBuckets(), 0);
}

// ─── 2. Balanced flow ────────────────────────────────────────────────────────
// Alternating price changes of equal magnitude → after many buckets each
// bucket has V_buy ≈ V_sell → OI ≈ 0 → VPIN ≈ 0.

TEST(VPINCalculator, BalancedFlowVPINNearZero) {
    const double bucketVol  = 1000.0;
    const int    tau        = 10;
    const double sigma      = 1.0;
    VPINCalculator calc(bucketVol, tau, sigma);

    // Trades of 100 each; alternating +1 / -1 price change so Phi(+1) and
    // Phi(-1) average to 0.5 each → equal buy and sell volume per trade.
    double price    = 100.0;
    double prevPrice;
    for (int i = 0; i < 500; ++i) {
        prevPrice = price;
        price    += (i % 2 == 0) ? +1.0 : -1.0;
        calc.recordTrade(price, 100.0, prevPrice);
    }

    EXPECT_GE(calc.completedBuckets(), tau);
    EXPECT_NEAR(calc.vpin(), 0.0, 0.05);
}

// ─── 3. One-sided flow ───────────────────────────────────────────────────────
// Monotone price increases → Phi(z >> 0) ≈ 1 → V_buy ≈ V → OI ≈ V → VPIN ≈ 1.

TEST(VPINCalculator, OneSidedFlowVPINNearOne) {
    const double bucketVol = 1000.0;
    const int    tau       = 10;
    const double sigma     = 0.1;    // small sigma so z = 1/0.1 = 10 → Phi ≈ 1
    VPINCalculator calc(bucketVol, tau, sigma);

    double price = 100.0;
    for (int i = 0; i < 500; ++i) {
        double prev = price;
        price += 1.0;
        calc.recordTrade(price, 100.0, prev);
    }

    EXPECT_GE(calc.completedBuckets(), tau);
    EXPECT_NEAR(calc.vpin(), 1.0, 0.05);
}

// ─── 4. Bucket boundary split ────────────────────────────────────────────────
// A single trade of 1500 with bucketVolume=1000 should close one full bucket
// and leave 500 in the next.

TEST(VPINCalculator, BucketBoundarySplitCorrectCount) {
    VPINCalculator calc(1000.0, 5, 1.0);
    // One trade of 1500 at zero price change (Phi=0.5 → 750 buy, 750 sell).
    calc.recordTrade(100.0, 1500.0, 100.0);

    // Should have closed exactly 1 bucket (1000 vol), with 500 remaining.
    EXPECT_EQ(calc.completedBuckets(), 1);

    // Send another 500 to close the second bucket.
    calc.recordTrade(100.0, 500.0, 100.0);
    EXPECT_EQ(calc.completedBuckets(), 2);
}

// ─── 5. Phi(0) = 0.5 ─────────────────────────────────────────────────────────
// Zero price change → z = 0 → Phi(0) = 0.5 → V_buy = V_sell = 0.5 * qty.

TEST(VPINCalculator, PhiZeroPriceDeltaEqualSplit) {
    // Use a bucket that is exactly filled by a single trade (qty == bucketVolume).
    VPINCalculator calc(1000.0, 1, 1.0);
    calc.recordTrade(100.0, 1000.0, 100.0);  // zero price delta

    EXPECT_EQ(calc.completedBuckets(), 1);
    // OI = |500 - 500| = 0  →  VPIN = 0 / 1000 = 0.0
    EXPECT_NEAR(calc.vpin(), 0.0, 1e-9);
}

// ─── 6. Phi(large positive z) ≈ 1.0 ─────────────────────────────────────────

TEST(VPINCalculator, PhiLargePositiveZAllBuyVolume) {
    // sigma = 0.01, price delta = 10 → z = 1000 → Phi ≈ 1.0
    VPINCalculator calc(1000.0, 1, 0.01);
    calc.recordTrade(110.0, 1000.0, 100.0);  // delta = +10

    EXPECT_EQ(calc.completedBuckets(), 1);
    // OI = |V_buy - V_sell| ≈ |1000 - 0| = 1000 → VPIN ≈ 1.0
    EXPECT_NEAR(calc.vpin(), 1.0, 1e-6);
}

// ─── 7. Phi(large negative z) ≈ 0.0 ─────────────────────────────────────────

TEST(VPINCalculator, PhiLargeNegativeZAllSellVolume) {
    // sigma = 0.01, price delta = -10 → z = -1000 → Phi ≈ 0.0
    VPINCalculator calc(1000.0, 1, 0.01);
    calc.recordTrade(90.0, 1000.0, 100.0);   // delta = -10

    EXPECT_EQ(calc.completedBuckets(), 1);
    // OI = |V_buy - V_sell| ≈ |0 - 1000| = 1000 → VPIN ≈ 1.0
    EXPECT_NEAR(calc.vpin(), 1.0, 1e-6);
}

// ─── 8. sigmaCurrent() updates with each trade ───────────────────────────────

TEST(VPINCalculator, SigmaCurrentUpdatesOnTrades) {
    VPINCalculator calc(1000.0, 5, 0.0);  // sigma=0 → use running estimate

    // Before any trade: sigma = 0.
    EXPECT_DOUBLE_EQ(calc.sigmaCurrent(), 0.0);

    // After 1 trade: still 0 (need at least 2 observations for Welford variance).
    calc.recordTrade(101.0, 10.0, 100.0);
    EXPECT_DOUBLE_EQ(calc.sigmaCurrent(), 0.0);

    // After 2 trades with different |delta| values (1 and 2): sigma > 0.
    calc.recordTrade(103.0, 10.0, 101.0);
    EXPECT_GT(calc.sigmaCurrent(), 0.0);

    // Sigma should be finite and non-negative.
    EXPECT_FALSE(std::isnan(calc.sigmaCurrent()));
    EXPECT_FALSE(std::isinf(calc.sigmaCurrent()));
}

// ─── 9. reset() clears all state ─────────────────────────────────────────────

TEST(VPINCalculator, ResetClearsAllState) {
    VPINCalculator calc(1000.0, 5, 1.0);

    // Drive several buckets and some partial state.
    double price = 100.0;
    for (int i = 0; i < 200; ++i) {
        double prev = price;
        price += 1.0;
        calc.recordTrade(price, 100.0, prev);
    }
    EXPECT_GT(calc.completedBuckets(), 0);

    calc.reset();

    EXPECT_EQ(calc.completedBuckets(), 0);
    EXPECT_DOUBLE_EQ(calc.vpin(), 0.0);
    EXPECT_DOUBLE_EQ(calc.sigmaCurrent(), 0.0);

    // A single bucket-sized trade after reset should behave as fresh.
    calc.recordTrade(100.0, 1000.0, 100.0);
    EXPECT_EQ(calc.completedBuckets(), 1);
}

// ─── 10. windowBuckets constraint ────────────────────────────────────────────
// vpin() must return 0 until exactly windowBuckets_ buckets have completed.

TEST(VPINCalculator, VPINZeroUntilWindowFull) {
    const int tau = 5;
    VPINCalculator calc(1000.0, tau, 1.0);

    // Close tau-1 buckets; vpin() should still be 0.
    for (int b = 0; b < tau - 1; ++b) {
        calc.recordTrade(101.0, 1000.0, 100.0);
        EXPECT_EQ(calc.completedBuckets(), b + 1);
        EXPECT_DOUBLE_EQ(calc.vpin(), 0.0)
            << "Expected vpin()=0 after " << (b + 1) << " bucket(s)";
    }

    // Close the tau-th bucket; now vpin() must be non-negative.
    calc.recordTrade(101.0, 1000.0, 100.0);
    EXPECT_EQ(calc.completedBuckets(), tau);
    EXPECT_GE(calc.vpin(), 0.0);
}

// ─── 11. VPIN ∈ [0, 1] for any input ─────────────────────────────────────────

TEST(VPINCalculator, VPINAlwaysInUnitInterval) {
    // Mixed scenario: random-ish alternating and monotone phases.
    VPINCalculator calc(500.0, 10, 1.0);

    double price = 100.0;
    // Phase 1: monotone up (VPIN should approach 1).
    for (int i = 0; i < 300; ++i) {
        double prev = price;
        price += 2.0;
        calc.recordTrade(price, 50.0, prev);
        double v = calc.vpin();
        EXPECT_GE(v, 0.0) << "VPIN below 0 at step " << i;
        EXPECT_LE(v, 1.0) << "VPIN above 1 at step " << i;
    }
    // Phase 2: balanced (VPIN should approach 0).
    for (int i = 0; i < 300; ++i) {
        double prev = price;
        price += (i % 2 == 0) ? +1.0 : -1.0;
        calc.recordTrade(price, 50.0, prev);
        double v = calc.vpin();
        EXPECT_GE(v, 0.0) << "VPIN below 0 at step " << i;
        EXPECT_LE(v, 1.0) << "VPIN above 1 at step " << i;
    }
}
