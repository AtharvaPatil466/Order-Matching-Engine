// TestExecutionAnalytics.cpp — GoogleTest suite for include/ExecutionAnalytics.h.
// Covers IS decomposition, VWAP/TWAP benchmarks, spread capture, reset, and
// empty-state safety.

#include "ExecutionAnalytics.h"

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

using namespace OrderMatcher;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static FillRecord makeBuy(double price, double qty, uint64_t seq = 0) {
    return FillRecord{price, qty, seq};
}

static FillRecord makeSell(double price, double qty, uint64_t seq = 0) {
    return FillRecord{price, -qty, seq};  // negative qty = sell
}

// Build a flat ReferenceSeries: every mid is the same value.
static ReferenceSeries flatRef(double arrival, double mid, std::size_t n) {
    ReferenceSeries ref;
    ref.arrivalPrice = arrival;
    ref.midAtFill.assign(n, mid);
    ref.postTradeMid.assign(n, mid);
    return ref;
}

// Build a ReferenceSeries with per-fill mids.
static ReferenceSeries makeRef(double arrival,
                               std::vector<double> mids,
                               std::vector<double> postMids = {}) {
    ReferenceSeries ref;
    ref.arrivalPrice = arrival;
    ref.midAtFill    = std::move(mids);
    ref.postTradeMid = std::move(postMids);
    return ref;
}

// ===========================================================================
// 1. Single buy fill exactly at arrival price → IS = 0
// ===========================================================================

TEST(ExecutionAnalytics, SingleBuyAtArrivalPriceIsZero) {
    ExecutionAnalytics ea;
    ea.record(makeBuy(100.0, 500.0, 1));
    ea.setReference(flatRef(100.0, 100.0, 1));

    EXPECT_NEAR(ea.implementationShortfall(), 0.0, 1e-9);
    EXPECT_NEAR(ea.realizedImpact(),          0.0, 1e-9);
}

// ===========================================================================
// 2. Buy fill above arrival price → positive IS
// ===========================================================================

TEST(ExecutionAnalytics, BuyAboveArrivalPositiveIS) {
    ExecutionAnalytics ea;
    // Single fill at 101; arrival was 100.
    ea.record(makeBuy(101.0, 1000.0, 1));
    ea.setReference(flatRef(100.0, 100.0, 1));

    // IS = (101 - 100) * +1 = 1.0
    EXPECT_NEAR(ea.implementationShortfall(), 1.0, 1e-9);
    EXPECT_GT(ea.implementationShortfall(), 0.0);
}

// ===========================================================================
// 3. VWAP of fills matches hand calculation
// ===========================================================================

TEST(ExecutionAnalytics, ParentVWAPMatchesHandCalc) {
    ExecutionAnalytics ea;
    // Three buys: 200 @ 100, 300 @ 102, 500 @ 104
    // VWAP = (200*100 + 300*102 + 500*104) / 1000
    //      = (20000 + 30600 + 52000) / 1000 = 102.6
    ea.record(makeBuy(100.0, 200.0, 1));
    ea.record(makeBuy(102.0, 300.0, 2));
    ea.record(makeBuy(104.0, 500.0, 3));

    EXPECT_NEAR(ea.parentVWAP(), 102.6, 1e-9);
}

// ===========================================================================
// 4. vwapSlippage: fills at market VWAP → slippage = 0
// ===========================================================================

TEST(ExecutionAnalytics, VwapSlippageZeroWhenFilledAtMarket) {
    ExecutionAnalytics ea;
    // Two fills matching the market mid exactly.
    ea.record(makeBuy(101.0, 100.0, 1));
    ea.record(makeBuy(103.0, 100.0, 2));

    // Market mids match fill prices → marketVWAP = (101+103)/2 = 102 = parentVWAP
    ea.setReference(makeRef(100.0, {101.0, 103.0}));

    EXPECT_NEAR(ea.vwapSlippage(), 0.0, 1e-9);
}

// ===========================================================================
// 5. Timing cost vs market impact decomposition: IS = timing + impact
// ===========================================================================

TEST(ExecutionAnalytics, ISDecomposesIntoTimingPlusMarketImpact) {
    ExecutionAnalytics ea;
    // Buy program: 3 fills
    ea.record(makeBuy(100.5, 400.0, 1));
    ea.record(makeBuy(101.0, 300.0, 2));
    ea.record(makeBuy(101.8, 300.0, 3));

    // arrival = 100; mids drift upward during the order
    ea.setReference(makeRef(100.0, {100.2, 100.6, 101.0}));

    double is     = ea.implementationShortfall();
    double timing = ea.timingCost();
    double impact = ea.marketImpact();

    // IS = timingCost + marketImpact (within floating-point precision)
    EXPECT_NEAR(is, timing + impact, 1e-9);
}

// ===========================================================================
// 6. TWAP slippage: fills on flat mid series → 0
// ===========================================================================

TEST(ExecutionAnalytics, TwapSlippageZeroOnFlatMidSeries) {
    ExecutionAnalytics ea;
    // Flat mid of 50; fill prices also all at 50.
    for (int i = 0; i < 5; ++i) {
        ea.record(makeBuy(50.0, 100.0, static_cast<uint64_t>(i + 1)));
    }
    ea.setReference(flatRef(50.0, 50.0, 5));

    EXPECT_NEAR(ea.twap(),         50.0, 1e-9);
    EXPECT_NEAR(ea.twapSlippage(),  0.0, 1e-9);
}

// ===========================================================================
// 7. meanSpreadCapture: sign convention tests
//
// Formula: spreadCapture_i = (fill_price - midAtFill) * sign(qty)
//   Aggressive buy  (crosses above mid): fill > mid, sign = +1 → positive
//   Resting buy     (rests below mid):   fill < mid, sign = +1 → negative
//   Resting sell    (rests above mid):   fill > mid, sign = -1 → negative
//   Aggressive sell (crosses below mid): fill < mid, sign = -1 → positive
//
// The meanSpreadCapture > 0 therefore signals that, on net, the order
// received fills whose prices were "on the right side" of the mid:
// buys filled above mid (aggressive) or sells filled below mid (aggressive).
// ===========================================================================

TEST(ExecutionAnalytics, SpreadCaptureSignConvention) {
    // Aggressive buy: fill above mid → positive spread capture
    {
        ExecutionAnalytics ea;
        ea.record(makeBuy(100.2, 100.0, 1));   // 0.2 above mid
        ea.setReference(makeRef(100.0, {100.0}));
        // (100.2 - 100.0) * +1 = +0.2
        EXPECT_NEAR(ea.meanSpreadCapture(), +0.2, 1e-9);
        EXPECT_GT(ea.meanSpreadCapture(), 0.0);
    }

    // Resting buy: fill below mid → negative spread capture
    {
        ExecutionAnalytics ea;
        ea.record(makeBuy(99.8, 100.0, 1));    // 0.2 below mid
        ea.setReference(makeRef(100.0, {100.0}));
        // (99.8 - 100.0) * +1 = -0.2
        EXPECT_NEAR(ea.meanSpreadCapture(), -0.2, 1e-9);
        EXPECT_LT(ea.meanSpreadCapture(), 0.0);
    }

    // Resting sell: fill above mid → negative spread capture
    {
        ExecutionAnalytics ea;
        ea.record(makeSell(100.2, 100.0, 1));  // 0.2 above mid
        ea.setReference(makeRef(100.0, {100.0}));
        // (100.2 - 100.0) * -1 = -0.2
        EXPECT_NEAR(ea.meanSpreadCapture(), -0.2, 1e-9);
        EXPECT_LT(ea.meanSpreadCapture(), 0.0);
    }

    // Aggressive sell: fill below mid → positive spread capture
    {
        ExecutionAnalytics ea;
        ea.record(makeSell(99.8, 100.0, 1));   // 0.2 below mid
        ea.setReference(makeRef(100.0, {100.0}));
        // (99.8 - 100.0) * -1 = +0.2
        EXPECT_NEAR(ea.meanSpreadCapture(), +0.2, 1e-9);
        EXPECT_GT(ea.meanSpreadCapture(), 0.0);
    }

    // Mixed fills: mean of +0.3 and -0.1 = +0.1
    {
        ExecutionAnalytics ea;
        ea.record(makeBuy(100.3, 100.0, 1));   // +0.3
        ea.record(makeBuy( 99.9, 100.0, 2));   // -0.1
        ea.setReference(makeRef(100.0, {100.0, 100.0}));
        EXPECT_NEAR(ea.meanSpreadCapture(), 0.1, 1e-9);
    }
}

// ===========================================================================
// 8. reset() clears state
// ===========================================================================

TEST(ExecutionAnalytics, ResetClearsAllState) {
    ExecutionAnalytics ea;
    ea.record(makeBuy(105.0, 1000.0, 1));
    ea.setReference(flatRef(100.0, 102.0, 1));

    // Pre-reset: non-zero metrics
    EXPECT_GT(ea.fillCount(), 0u);
    EXPECT_GT(ea.totalQty(),  0.0);

    ea.reset();

    EXPECT_EQ(ea.fillCount(), 0u);
    EXPECT_NEAR(ea.totalQty(),                0.0, 1e-9);
    EXPECT_NEAR(ea.avgFillPrice(),            0.0, 1e-9);
    EXPECT_NEAR(ea.parentVWAP(),              0.0, 1e-9);
    EXPECT_NEAR(ea.implementationShortfall(), 0.0, 1e-9);
    EXPECT_NEAR(ea.realizedImpact(),          0.0, 1e-9);
    EXPECT_NEAR(ea.timingCost(),              0.0, 1e-9);
    EXPECT_NEAR(ea.marketImpact(),            0.0, 1e-9);
    EXPECT_NEAR(ea.vwapSlippage(),            0.0, 1e-9);
    EXPECT_NEAR(ea.twapSlippage(),            0.0, 1e-9);
    EXPECT_NEAR(ea.meanSpreadCapture(),       0.0, 1e-9);

    // Usable again after reset
    ea.record(makeBuy(50.0, 100.0, 1));
    EXPECT_EQ(ea.fillCount(), 1u);
    EXPECT_NEAR(ea.parentVWAP(), 50.0, 1e-9);
}

// ===========================================================================
// 9. No fills → all metrics return 0, no crash
// ===========================================================================

TEST(ExecutionAnalytics, NoFillsAllMetricsReturnZero) {
    ExecutionAnalytics ea;

    EXPECT_EQ(ea.fillCount(), 0u);
    EXPECT_NEAR(ea.totalQty(),                0.0, 1e-9);
    EXPECT_NEAR(ea.avgFillPrice(),            0.0, 1e-9);
    EXPECT_NEAR(ea.parentVWAP(),              0.0, 1e-9);
    EXPECT_NEAR(ea.marketVWAP(),              0.0, 1e-9);
    EXPECT_NEAR(ea.vwapSlippage(),            0.0, 1e-9);
    EXPECT_NEAR(ea.twap(),                    0.0, 1e-9);
    EXPECT_NEAR(ea.twapSlippage(),            0.0, 1e-9);
    EXPECT_NEAR(ea.implementationShortfall(), 0.0, 1e-9);
    EXPECT_NEAR(ea.realizedImpact(),          0.0, 1e-9);
    EXPECT_NEAR(ea.timingCost(),              0.0, 1e-9);
    EXPECT_NEAR(ea.marketImpact(),            0.0, 1e-9);
    EXPECT_NEAR(ea.meanSpreadCapture(),       0.0, 1e-9);

    // With a reference set but no fills — still all zero, no crash.
    ea.setReference(flatRef(100.0, 100.0, 0));
    EXPECT_NEAR(ea.implementationShortfall(), 0.0, 1e-9);
    EXPECT_NEAR(ea.vwapSlippage(),            0.0, 1e-9);
    EXPECT_NEAR(ea.meanSpreadCapture(),       0.0, 1e-9);
}

// ===========================================================================
// Bonus: sell program direction and IS sign
// ===========================================================================

TEST(ExecutionAnalytics, SellProgramBelowArrivalPositiveIS) {
    ExecutionAnalytics ea;
    // Sell program: fill below arrival → IS > 0 (also a cost to the seller)
    ea.record(makeSell(99.0, 1000.0, 1));
    ea.setReference(flatRef(100.0, 100.0, 1));

    // direction = -1; parentVWAP = 99; IS = (99 - 100) * -1 = 1.0
    EXPECT_NEAR(ea.implementationShortfall(), 1.0, 1e-9);
}

TEST(ExecutionAnalytics, MultipleFilledVwapSlippage) {
    ExecutionAnalytics ea;
    // 4 buys; market mids are different from fill prices.
    ea.record(makeBuy(101.0, 100.0, 1));
    ea.record(makeBuy(103.0, 200.0, 2));
    ea.record(makeBuy(102.0, 150.0, 3));
    ea.record(makeBuy(104.0, 50.0,  4));

    // parentVWAP = (100*101 + 200*103 + 150*102 + 50*104) / 500
    //           = (10100 + 20600 + 15300 + 5200) / 500
    //           = 51200 / 500 = 102.4
    const double pVWAP = (100*101.0 + 200*103.0 + 150*102.0 + 50*104.0) / 500.0;

    // Market mids: weight by same fill sizes
    ea.setReference(makeRef(100.0, {100.5, 102.5, 101.5, 103.5}));
    // marketVWAP = (100*100.5 + 200*102.5 + 150*101.5 + 50*103.5) / 500
    const double mVWAP = (100*100.5 + 200*102.5 + 150*101.5 + 50*103.5) / 500.0;

    EXPECT_NEAR(ea.parentVWAP(), pVWAP, 1e-9);
    EXPECT_NEAR(ea.marketVWAP(), mVWAP, 1e-9);
    EXPECT_NEAR(ea.vwapSlippage(), pVWAP - mVWAP, 1e-9);
}
