// TestOptimalExecution.cpp — GoogleTest suite for include/OptimalExecution.h.
// Tests cover: twapSchedule, almgrenChrissSchedule, vwapSchedule,
// computeExpectedCost, computeCostVariance, and edge cases.

#include "OptimalExecution.h"

#include <cmath>
#include <numeric>
#include <vector>

#include <gtest/gtest.h>

using namespace OrderMatcher;

// ---------------------------------------------------------------------------
// Helper: default params with moderate risk-aversion.
// ---------------------------------------------------------------------------
static ACParams makeParams(double eta = 0.1, double gamma = 0.05,
                            double sigma = 0.02, double lambda = 1e-5) {
    return ACParams{eta, gamma, sigma, lambda};
}

// ===========================================================================
// Test 1 — TWAP: all trades equal X/N, holdings decrease linearly.
// ===========================================================================
TEST(TWAP, TradesAreUniform) {
    const double X = 10000.0;
    const int    N = 5;
    const double tau = 1.0 / 252.0;
    auto p = makeParams();
    auto s = twapSchedule(X, N, tau, p);

    ASSERT_EQ(static_cast<int>(s.trades.size()), N);
    const double expected_trade = X / static_cast<double>(N);
    for (double dxj : s.trades) {
        EXPECT_NEAR(dxj, expected_trade, 1e-9);
    }
}

TEST(TWAP, HoldingsDecreaseLinearly) {
    const double X = 10000.0;
    const int    N = 5;
    const double tau = 1.0 / 252.0;
    auto p = makeParams();
    auto s = twapSchedule(X, N, tau, p);

    ASSERT_EQ(static_cast<int>(s.holdings.size()), N + 1);
    EXPECT_NEAR(s.holdings[0], X, 1e-9);
    EXPECT_NEAR(s.holdings[N], 0.0, 1e-9);

    // Each step drops by X/N.
    const double step = X / static_cast<double>(N);
    for (int j = 0; j < N; ++j) {
        double expected = X - static_cast<double>(j) * step;
        EXPECT_NEAR(s.holdings[j], expected, 1e-9);
    }
}

// ===========================================================================
// Test 2 — TWAP expectedCost matches computeExpectedCost (within 1e-9).
// ===========================================================================
TEST(TWAP, ExpectedCostMatchesComputeHelper) {
    const double X = 8000.0;
    const int    N = 10;
    const double tau = 1.0 / 252.0;
    auto p = makeParams();
    auto s = twapSchedule(X, N, tau, p);

    double manualCost = computeExpectedCost(s.trades, tau, p);
    EXPECT_NEAR(s.expectedCost, manualCost, 1e-9);
}

// ===========================================================================
// Test 3 — AC schedule: holdings[0] == X, holdings[N] == 0 (full liquidation).
// ===========================================================================
TEST(AlmgrenChriss, FullLiquidationBoundaries) {
    const double X = 50000.0;
    const int    N = 20;
    const double tau = 1.0 / 252.0;
    auto p = makeParams(0.1, 0.05, 0.02, 1e-5);
    auto s = almgrenChrissSchedule(X, N, tau, p);

    ASSERT_EQ(static_cast<int>(s.holdings.size()), N + 1);
    EXPECT_NEAR(s.holdings[0], X,   1e-9);
    EXPECT_NEAR(s.holdings[N], 0.0, 1e-9);
}

// ===========================================================================
// Test 4 — AC trades sum to X (within 1e-9).
// ===========================================================================
TEST(AlmgrenChriss, TradesSumToX) {
    const double X = 12345.0;
    const int    N = 15;
    const double tau = 1.0 / 252.0;
    auto p = makeParams(0.1, 0.05, 0.02, 1e-5);
    auto s = almgrenChrissSchedule(X, N, tau, p);

    double total = std::accumulate(s.trades.begin(), s.trades.end(), 0.0);
    EXPECT_NEAR(total, X, 1e-9);
}

// ===========================================================================
// Test 5 — AC schedule front-loads trades when lambda is high.
// (first trade > last trade for positive X, risk-averse regime)
// ===========================================================================
TEST(AlmgrenChriss, FrontLoadedWhenHighRiskAversion) {
    const double X = 10000.0;
    const int    N = 10;
    const double tau = 1.0 / 252.0;
    // High lambda: strongly front-load to reduce variance exposure.
    auto p = makeParams(0.1, 0.05, 0.02, 1e-3);
    auto s = almgrenChrissSchedule(X, N, tau, p);

    ASSERT_GE(static_cast<int>(s.trades.size()), 2);
    EXPECT_GT(s.trades.front(), s.trades.back());
}

// ===========================================================================
// Test 6 — AC schedule is TWAP when lambda=0 (risk-neutral limit).
// (holdings are linear; falls back to TWAP internally via kappa≈0)
// ===========================================================================
TEST(AlmgrenChriss, DegeneratesTo_TWAP_WhenLambdaZero) {
    const double X = 10000.0;
    const int    N = 10;
    const double tau = 1.0 / 252.0;
    auto p = makeParams(0.1, 0.05, 0.02, 0.0); // lambda = 0

    auto ac   = almgrenChrissSchedule(X, N, tau, p);
    auto twap = twapSchedule(X, N, tau, p);

    ASSERT_EQ(ac.holdings.size(), twap.holdings.size());
    for (std::size_t j = 0; j < ac.holdings.size(); ++j) {
        EXPECT_NEAR(ac.holdings[j], twap.holdings[j], 1e-6);
    }
}

// ===========================================================================
// Test 7 — VWAP with uniform curve == TWAP.
// ===========================================================================
TEST(VWAP, UniformCurveEqualsTWAP) {
    const double X = 6000.0;
    const int    N = 6;
    const double tau = 1.0 / 252.0;
    auto p = makeParams();

    std::vector<double> uniform(static_cast<std::size_t>(N),
                                 1.0 / static_cast<double>(N));
    auto vwap = vwapSchedule(X, N, tau, p, uniform);
    auto twap = twapSchedule(X, N, tau, p);

    ASSERT_EQ(vwap.trades.size(), twap.trades.size());
    for (std::size_t j = 0; j < vwap.trades.size(); ++j) {
        EXPECT_NEAR(vwap.trades[j], twap.trades[j], 1e-9);
    }
}

// ===========================================================================
// Test 8 — VWAP with front-loaded curve: first trade > last trade.
// ===========================================================================
TEST(VWAP, FrontLoadedCurveGivesFrontLoadedTrades) {
    const double X = 10000.0;
    const int    N = 4;
    const double tau = 1.0 / 252.0;
    auto p = makeParams();

    // Curve: 50%, 30%, 15%, 5% — heavily front-loaded.
    std::vector<double> curve = {0.50, 0.30, 0.15, 0.05};
    auto s = vwapSchedule(X, N, tau, p, curve);

    ASSERT_EQ(static_cast<int>(s.trades.size()), N);
    EXPECT_GT(s.trades.front(), s.trades.back());
}

// ===========================================================================
// Test 9 — computeExpectedCost with all-zero trades = gamma/2 * X^2.
// (only permanent impact; no temporary impact from trading)
// ===========================================================================
TEST(ComputeCost, ZeroTradesGivesOnlyPermanentCost) {
    // With zero trades the only contribution is the permanent impact term:
    // gamma/2 * X^2, where X = sum(trades) = 0.
    // Hence expectedCost = 0.
    // But if we want to check the formula for a specific X that was
    // liquidated entirely in "zero" incremental trades (conceptually,
    // one large sweep already happened), we call the helper directly
    // with a single-trade vector that equals X and check the formula.
    //
    // The spec: "computeExpectedCost with all-zero trades = gamma/2 * X²"
    // means X = sum(trades) = 0, so the result is 0.
    // We verify this plus the pure permanent case with a single trade.
    const ACParams p{0.1, 0.05, 0.02, 1e-5};
    const double tau = 1.0 / 252.0;

    // All-zero trades: X = 0, cost = 0.
    std::vector<double> zero_trades(5, 0.0);
    double cost_zero = computeExpectedCost(zero_trades, tau, p);
    EXPECT_NEAR(cost_zero, 0.0, 1e-9);

    // Single trade of X: gamma/2 * X^2 + eta * (X/tau)^2 * tau
    //                   = gamma/2 * X^2 + eta * X^2 / tau
    const double X2 = 1000.0;
    std::vector<double> single{X2};
    double cost_single = computeExpectedCost(single, tau, p);
    double expected    = p.gamma / 2.0 * X2 * X2 + p.eta * (X2 * X2 / tau);
    EXPECT_NEAR(cost_single, expected, 1e-6);
}

// ===========================================================================
// Test 10 — N=1 edge case: single trade = X.
// ===========================================================================
TEST(AlmgrenChriss, SingleStepTradeEqualsX) {
    const double X = 777.0;
    const int    N = 1;
    const double tau = 1.0;
    auto p = makeParams();
    auto s = almgrenChrissSchedule(X, N, tau, p);

    ASSERT_EQ(static_cast<int>(s.trades.size()), 1);
    EXPECT_NEAR(s.trades[0], X, 1e-9);
    EXPECT_NEAR(s.holdings[0], X,   1e-9);
    EXPECT_NEAR(s.holdings[1], 0.0, 1e-9);
}

TEST(TWAP, SingleStepTradeEqualsX) {
    const double X = 500.0;
    const int    N = 1;
    const double tau = 1.0;
    auto p = makeParams();
    auto s = twapSchedule(X, N, tau, p);

    ASSERT_EQ(static_cast<int>(s.trades.size()), 1);
    EXPECT_NEAR(s.trades[0], X, 1e-9);
}

// ===========================================================================
// Test 11 — X=0: all trades are 0, expected cost is 0.
// ===========================================================================
TEST(AlmgrenChriss, ZeroPositionAllTradesZero) {
    const double X = 0.0;
    const int    N = 10;
    const double tau = 1.0 / 252.0;
    auto p = makeParams();
    auto s = almgrenChrissSchedule(X, N, tau, p);

    for (double dxj : s.trades) {
        EXPECT_NEAR(dxj, 0.0, 1e-9);
    }
    EXPECT_NEAR(s.expectedCost, 0.0, 1e-9);
    EXPECT_NEAR(s.costVariance, 0.0, 1e-9);
}

TEST(TWAP, ZeroPositionAllTradesZero) {
    const double X = 0.0;
    const int    N = 5;
    const double tau = 1.0 / 252.0;
    auto p = makeParams();
    auto s = twapSchedule(X, N, tau, p);

    for (double dxj : s.trades) {
        EXPECT_NEAR(dxj, 0.0, 1e-9);
    }
    EXPECT_NEAR(s.expectedCost, 0.0, 1e-9);
}

// ===========================================================================
// Additional sanity checks
// ===========================================================================

// costVariance is 0 when sigma = 0 (no price risk).
TEST(ComputeCost, ZeroSigmaGivesZeroVariance) {
    ACParams p{0.1, 0.05, 0.0, 1e-5}; // sigma = 0
    const double tau = 1.0 / 252.0;
    std::vector<double> trades = {1000.0, 2000.0, 3000.0};
    EXPECT_NEAR(computeCostVariance(trades, tau, p), 0.0, 1e-15);
}

// sharpeRatio is 0 when costVariance is 0.
TEST(AlmgrenChriss, SharpeRatioZeroWhenVarianceZero) {
    ACParams p{0.1, 0.05, 0.0, 1e-5}; // sigma = 0 → variance = 0
    auto s = almgrenChrissSchedule(1000.0, 5, 1.0 / 252.0, p);
    EXPECT_NEAR(s.sharpeRatio, 0.0, 1e-15);
}

// tradeRates[j] == trades[j] / tau.
TEST(TWAP, TradeRatesConsistentWithTrades) {
    const double X = 3000.0;
    const int    N = 6;
    const double tau = 1.0 / 252.0;
    auto p = makeParams();
    auto s = twapSchedule(X, N, tau, p);

    ASSERT_EQ(s.tradeRates.size(), s.trades.size());
    for (std::size_t j = 0; j < s.trades.size(); ++j) {
        EXPECT_NEAR(s.tradeRates[j], s.trades[j] / tau, 1e-9);
    }
}

// VWAP with empty curve falls back to TWAP.
TEST(VWAP, EmptyCurveFallsBackToTWAP) {
    const double X = 4000.0;
    const int    N = 4;
    const double tau = 1.0 / 252.0;
    auto p = makeParams();

    auto vwap = vwapSchedule(X, N, tau, p, {});
    auto twap = twapSchedule(X, N, tau, p);

    ASSERT_EQ(vwap.trades.size(), twap.trades.size());
    for (std::size_t j = 0; j < vwap.trades.size(); ++j) {
        EXPECT_NEAR(vwap.trades[j], twap.trades[j], 1e-9);
    }
}

// AC schedule for a short position (negative X): trades are negative,
// sum to X, and holdings[N] == 0.
TEST(AlmgrenChriss, ShortPositionFullLiquidation) {
    const double X = -5000.0;
    const int    N = 10;
    const double tau = 1.0 / 252.0;
    auto p = makeParams(0.1, 0.05, 0.02, 1e-5);
    auto s = almgrenChrissSchedule(X, N, tau, p);

    EXPECT_NEAR(s.holdings[0], X,   1e-9);
    EXPECT_NEAR(s.holdings[N], 0.0, 1e-9);

    double total = std::accumulate(s.trades.begin(), s.trades.end(), 0.0);
    EXPECT_NEAR(total, X, 1e-9);
}
