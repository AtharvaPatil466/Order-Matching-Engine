// TestMicrostructureMetrics — Google Test suite for MicrostructureMetrics.
//
// Test coverage:
//  (a) effectiveSpread() calculation
//  (b) orderFlowImbalance()
//  (c) kylesLambda() with >= 10 events (and < 10 returns 0)
//  (d) realizedSpread()
//  (e) amihudRatio()
//  (f) sampleCount() and reset()
//  (g) circular buffer wrapping (> 100 events)
//  (h) edge cases: empty / single event

#include <gtest/gtest.h>

#include "MicrostructureMetrics.h"
#include "Types.h"

#include <cmath>

using namespace OrderMatcher;

// ── Helpers ──────────────────────────────────────────────────────────────────

static MicrostructureMetrics::TradeEvent makeEvent(
        double price, Side side, Quantity qty, double mid,
        uint64_t ts = 0) {
    return {toPrice(price), side, qty, toPrice(mid), ts};
}

// ── Tests ────────────────────────────────────────────────────────────────────

// (h) Empty metrics return zero / neutral values.
TEST(MicrostructureMetrics, EmptyReturnsZero) {
    MicrostructureMetrics m;
    EXPECT_EQ(m.sampleCount(), 0u);
    EXPECT_DOUBLE_EQ(m.effectiveSpread(),    0.0);
    EXPECT_DOUBLE_EQ(m.realizedSpread(),     0.0);
    EXPECT_DOUBLE_EQ(m.kylesLambda(),        0.0);
    EXPECT_DOUBLE_EQ(m.amihudRatio(),        0.0);
    EXPECT_DOUBLE_EQ(m.orderFlowImbalance(), 0.0);
}

// (h) Single event.
TEST(MicrostructureMetrics, SingleEvent) {
    MicrostructureMetrics m;
    m.record(makeEvent(100.05, Side::Buy, 100, 100.0));

    EXPECT_EQ(m.sampleCount(), 1u);
    // effective spread = 2 * |100.05 - 100.0| = 0.10 (in price units / PRICE_PRECISION)
    double expected = 2.0 * std::abs(toPrice(100.05) - toPrice(100.0));
    EXPECT_NEAR(m.effectiveSpread(), expected, 1e-6);
    // OFI: only buys → 1.0
    EXPECT_DOUBLE_EQ(m.orderFlowImbalance(), 1.0);
    // Not enough for Kyle's lambda or realized spread.
    EXPECT_DOUBLE_EQ(m.kylesLambda(), 0.0);
    EXPECT_DOUBLE_EQ(m.realizedSpread(), 0.0);
}

// (a) Effective spread — known values.
//
// Two buy trades at price = mid + 5 (in integer price units).
// effectiveSpread = mean(2*5, 2*5) = 10.
TEST(MicrostructureMetrics, EffectiveSpreadKnownValues) {
    MicrostructureMetrics m;
    // mid = 1000000 (100.0000), price = 1000050 (100.0050) → |diff| = 50
    Price mid   = 1000000;
    Price price = 1000050;  // 5 ticks above mid
    m.record({price, Side::Buy,  100, mid, 1});
    m.record({price, Side::Sell, 50,  mid, 2});

    // Each event: 2 * 50 = 100.
    EXPECT_DOUBLE_EQ(m.effectiveSpread(), 100.0);
}

// (a) Zero effective spread when all trades occur exactly at mid.
TEST(MicrostructureMetrics, EffectiveSpreadZeroAtMid) {
    MicrostructureMetrics m;
    for (int i = 0; i < 10; ++i) {
        m.record({toPrice(50.0), Side::Buy, 10, toPrice(50.0), 0});
    }
    EXPECT_DOUBLE_EQ(m.effectiveSpread(), 0.0);
}

// (b) Order flow imbalance — all buys → +1.
TEST(MicrostructureMetrics, OFIAllBuys) {
    MicrostructureMetrics m;
    for (int i = 0; i < 5; ++i)
        m.record({toPrice(100.0), Side::Buy, 10, toPrice(100.0), 0});
    EXPECT_DOUBLE_EQ(m.orderFlowImbalance(), 1.0);
}

// (b) Order flow imbalance — all sells → -1.
TEST(MicrostructureMetrics, OFIAllSells) {
    MicrostructureMetrics m;
    for (int i = 0; i < 5; ++i)
        m.record({toPrice(100.0), Side::Sell, 10, toPrice(100.0), 0});
    EXPECT_DOUBLE_EQ(m.orderFlowImbalance(), -1.0);
}

// (b) Order flow imbalance — balanced buys/sells → 0.
TEST(MicrostructureMetrics, OFIBalanced) {
    MicrostructureMetrics m;
    for (int i = 0; i < 4; ++i) {
        m.record({toPrice(100.0), Side::Buy,  10, toPrice(100.0), 0});
        m.record({toPrice(100.0), Side::Sell, 10, toPrice(100.0), 0});
    }
    EXPECT_DOUBLE_EQ(m.orderFlowImbalance(), 0.0);
}

// (b) Order flow imbalance — unequal volumes.
// buy_vol = 30, sell_vol = 10 → OFI = (30-10)/(30+10) = 0.5
TEST(MicrostructureMetrics, OFIUnequal) {
    MicrostructureMetrics m;
    m.record({toPrice(100.0), Side::Buy,  30, toPrice(100.0), 0});
    m.record({toPrice(100.0), Side::Sell, 10, toPrice(100.0), 0});
    EXPECT_DOUBLE_EQ(m.orderFlowImbalance(), 0.5);
}

// (c) Kyle's lambda returns 0 for fewer than 10 events.
TEST(MicrostructureMetrics, KylesLambdaInsufficient) {
    MicrostructureMetrics m;
    for (int i = 0; i < 9; ++i)
        m.record({toPrice(100.0), Side::Buy, 10, toPrice(100.0), 0});
    EXPECT_DOUBLE_EQ(m.kylesLambda(), 0.0);
}

// (c) Kyle's lambda: OLS with known synthetic data.
//
// Construct events so that Δmid = lambda * signed_flow + noise=0.
// Use lambda = 0.01 (per unit of quantity in Price ticks).
//
// signed_flow_i ∈ {-10, +10} alternating.
// mid_{i+1} = mid_i + lambda * signed_flow_i.
// With PRICE_PRECISION=10000: lambda (in Price units) = 1 tick per 100 qty.
TEST(MicrostructureMetrics, KylesLambdaPositiveSlope) {
    MicrostructureMetrics m;
    // Lambda in price-integer units: 1 tick (= 1 integer unit) per 100 qty.
    const double lambdaTrue = 1.0 / 100.0;
    Price mid = 1000000;  // 100.0000
    for (int i = 0; i < 20; ++i) {
        double signedFlow = (i % 2 == 0) ? 100.0 : -100.0;
        Side side = (signedFlow > 0) ? Side::Buy : Side::Sell;
        Quantity qty = 100;
        m.record({mid, side, qty, mid, static_cast<uint64_t>(i)});
        // Advance mid by lambda * signedFlow.
        mid += static_cast<Price>(lambdaTrue * signedFlow);
    }

    double lambda = m.kylesLambda();
    // The slope should be close to lambdaTrue.
    EXPECT_NEAR(lambda, lambdaTrue, std::abs(lambdaTrue) * 0.20);
}

// (c) Kyle's lambda: zero flow variance → returns 0 (guard against div/0).
TEST(MicrostructureMetrics, KylesLambdaZeroVariance) {
    MicrostructureMetrics m;
    // All buys with same qty → zero variance in x.
    Price mid = 1000000;
    for (int i = 0; i < 15; ++i) {
        m.record({mid, Side::Buy, 100, mid, static_cast<uint64_t>(i)});
        mid += 0;  // mid doesn't change either → y=0 also, but x-variance = 0
    }
    // With zero variance in x the formula must not divide by zero.
    EXPECT_DOUBLE_EQ(m.kylesLambda(), 0.0);
}

// (d) Realized spread: with forwardWindow=1, check a known case.
//
// Three events: trade at +10 ticks above mid; one period later mid returns
// to original. D=+1 (buy).
// RS = 2 * (+1) * (price - mid_t+1)
//    = 2 * (price - original_mid)   [since mid_t+1 = original_mid]
TEST(MicrostructureMetrics, RealizedSpreadKnownValue) {
    MicrostructureMetrics m;
    // Event 0: buy at mid+10, mid = 1000000.
    Price mid0   = 1000000;
    Price price0 = mid0 + 10;
    // Event 1: mid snaps back to 1000000 (post-trade mid for event 0).
    Price mid1   = 1000000;
    Price price1 = mid1;
    m.record({price0, Side::Buy, 100, mid0, 0});
    m.record({price1, Side::Buy, 100, mid1, 1});

    // RS(forwardWindow=1): only event 0 qualifies.
    // RS = 2 * 1 * (price0 - mid1) = 2 * (mid0+10 - mid0) = 20.
    double rs = m.realizedSpread(/*forwardWindow=*/1);
    EXPECT_NEAR(rs, 20.0, 1e-6);
}

// (d) Realized spread returns 0 when forwardWindow >= count.
TEST(MicrostructureMetrics, RealizedSpreadInsufficientEvents) {
    MicrostructureMetrics m;
    m.record({toPrice(100.0), Side::Buy, 10, toPrice(100.0), 0});
    m.record({toPrice(100.0), Side::Buy, 10, toPrice(100.0), 1});
    // forwardWindow=5 but only 2 events.
    EXPECT_DOUBLE_EQ(m.realizedSpread(5), 0.0);
}

// (d) Realized spread: sells have negative D, so a sell at mid+10 with
// mid snapping back later gives RS < 0.
TEST(MicrostructureMetrics, RealizedSpreadSellNegative) {
    MicrostructureMetrics m;
    Price mid0   = 1000000;
    Price price0 = mid0 + 10;  // sell above mid (unusual but valid test)
    Price mid1   = 1000000;
    Price price1 = mid1;
    m.record({price0, Side::Sell, 100, mid0, 0});
    m.record({price1, Side::Sell, 100, mid1, 1});

    // RS = 2 * (-1) * (price0 - mid1) = 2 * (-1) * 10 = -20.
    double rs = m.realizedSpread(1);
    EXPECT_NEAR(rs, -20.0, 1e-6);
}

// (e) Amihud illiquidity ratio.
//
// Two events: mid moves by 100 ticks, volume = 100. Ratio = 100/100 = 1.0.
TEST(MicrostructureMetrics, AmihudRatioKnownValue) {
    MicrostructureMetrics m;
    m.record({toPrice(100.0), Side::Buy, 100, toPrice(100.00), 0});
    m.record({toPrice(101.0), Side::Buy, 100, toPrice(100.01), 1});

    // |Δmid| = |toPrice(100.01) - toPrice(100.00)| = 100 ticks.
    // vol = 100. Ratio = 100/100 = 1.0.
    double ratio = m.amihudRatio();
    EXPECT_NEAR(ratio, 1.0, 1e-6);
}

// (e) Amihud ratio returns 0 with fewer than 2 events.
TEST(MicrostructureMetrics, AmihudRatioInsufficientEvents) {
    MicrostructureMetrics m;
    m.record({toPrice(100.0), Side::Buy, 10, toPrice(100.0), 0});
    EXPECT_DOUBLE_EQ(m.amihudRatio(), 0.0);
}

// (f) sampleCount() tracks total events even past the buffer capacity.
TEST(MicrostructureMetrics, SampleCountBeyondBuffer) {
    MicrostructureMetrics m;
    for (int i = 0; i < 150; ++i) {
        m.record({toPrice(100.0), Side::Buy, 10, toPrice(100.0), 0});
    }
    EXPECT_EQ(m.sampleCount(), 150u);
}

// (f) reset() clears state.
TEST(MicrostructureMetrics, ResetClearsState) {
    MicrostructureMetrics m;
    for (int i = 0; i < 20; ++i)
        m.record({toPrice(100.0), Side::Buy, 10, toPrice(100.0), 0});

    m.reset();
    EXPECT_EQ(m.sampleCount(), 0u);
    EXPECT_DOUBLE_EQ(m.effectiveSpread(),    0.0);
    EXPECT_DOUBLE_EQ(m.orderFlowImbalance(), 0.0);
    EXPECT_DOUBLE_EQ(m.kylesLambda(),        0.0);
}

// (g) Circular buffer wrapping: fill past 100 events and verify that
// only the last 100 are considered for OFI.
//
// First 105 events: sell; last 5 events: buy (qty=10 each).
// After wrapping, the buffer has 95 sells + 5 buys.
// buy_vol=50, sell_vol=950, OFI=(50-950)/(50+950)=-900/1000=-0.9.
TEST(MicrostructureMetrics, CircularBufferWrapping) {
    MicrostructureMetrics m;
    for (int i = 0; i < 95; ++i)
        m.record({toPrice(100.0), Side::Sell, 10, toPrice(100.0), 0});
    for (int i = 0; i < 5; ++i)
        m.record({toPrice(100.0), Side::Buy, 10, toPrice(100.0), 0});

    // Total so far = 100; buffer exactly full: 95 sells + 5 buys.
    double ofi100 = m.orderFlowImbalance();
    EXPECT_NEAR(ofi100, (50.0 - 950.0) / (50.0 + 950.0), 1e-9);

    // Add 10 more sells. The buffer now holds the last 100: 85 sells + 5 buys + 10 sells = 95 sells + 5 buys? No:
    // Let's recount. We have 100 events in buf. We add 10 sells. After adding each sell the oldest event
    // (also a sell) is evicted. Net: buffer still has 95 sells + 5 buys.
    for (int i = 0; i < 10; ++i)
        m.record({toPrice(100.0), Side::Sell, 10, toPrice(100.0), 0});

    // Now oldest 10 entries (sells) are evicted, replaced by 10 more sells.
    // Still 95 sells + 5 buys → OFI unchanged.
    EXPECT_NEAR(m.orderFlowImbalance(), ofi100, 1e-9);
    EXPECT_EQ(m.sampleCount(), 110u);  // total count across all records
}

// (g) After filling 100 events (all sells) and adding 1 buy, the oldest
// sell is evicted. Buffer: 99 sells + 1 buy. OFI = (10-990)/(10+990).
TEST(MicrostructureMetrics, CircularBufferEvictsOldest) {
    MicrostructureMetrics m;
    for (int i = 0; i < 100; ++i)
        m.record({toPrice(100.0), Side::Sell, 10, toPrice(100.0), 0});
    m.record({toPrice(100.0), Side::Buy, 10, toPrice(100.0), 0});

    // Oldest (a sell) evicted; buffer has 99 sells + 1 buy.
    double expected = (10.0 - 990.0) / (10.0 + 990.0);
    EXPECT_NEAR(m.orderFlowImbalance(), expected, 1e-9);
}
