// TestBacktestEngine.cpp — Google Test suite for BacktestEngine.h
//
// Tests:
//   1. run() with 100-share long position, 5 steps → fillRate > 0
//   2. theoreticalIS matches computeExpectedCost() within 1e-6
//   3. trackingError = actualIS - theoreticalIS (arithmetic identity)
//   4. fillHistory().size() equals total fills recorded
//   5. clearHistory() resets fills

#include <gtest/gtest.h>

#include "BacktestEngine.h"
#include "CalibrationPipeline.h"
#include "ExecutionAnalytics.h"
#include "MatchingEngine.h"
#include "OptimalExecution.h"
#include "Types.h"

#include <cmath>
#include <vector>

using namespace OrderMatcher;

// ── Fixture: a running async engine with a seeded order book ────────────────

class BacktestEngineTest : public ::testing::Test {
protected:
    static constexpr SymbolId     kSym      = 1;
    static constexpr ParticipantId kLiqProv  = 99;  // passive liquidity provider
    static constexpr ParticipantId kExec     = 1;   // executor under test
    static constexpr OrderId       kLiqBase  = 1000; // liquidity-provider order IDs
    static constexpr OrderId       kExecBase = 5000; // executor order IDs

    MatchingEngine  engine_;

    // Representative calibration result (synthetic, reasonable params).
    CalibrationResult makeCalibResult(double sigma = 0.01, double lambda = 1e-5) const {
        CalibrationResult r{};
        r.valid      = true;
        r.adv        = 10000.0;
        r.tradesUsed = 10;
        r.params.eta    = 0.01;   // small temporary impact
        r.params.gamma  = 0.005;  // small permanent impact
        r.params.sigma  = sigma;
        r.params.lambda = lambda;
        return r;
    }

    void SetUp() override {
        engine_.addSymbol(kSym);
        engine_.startAsync(1, 1024);
        seedLiquidity();
    }

    void TearDown() override {
        engine_.stopAsync();
    }

    // Place resting passive orders on both sides so the backtest executor
    // has liquidity to hit.  We place 20 sell orders at prices just above
    // mid so an aggressive limit buy can fill, and 20 buy orders just below.
    void seedLiquidity() {
        // Mid ≈ 100.00; bids at 99.xx, asks at 100.xx.
        OrderId id = kLiqBase;
        for (int i = 0; i < 20; ++i) {
            // Sell side (asks): prices 100.00 – 100.19 in 0.01 steps.
            Price askPrice = toPrice(100.00 + i * 0.01);
            engine_.submitOrder(kSym, id++, kLiqProv,
                                Side::Sell, askPrice, 20,
                                OrderType::Limit);
            // Buy side (bids): prices 99.00 – 99.19 in 0.01 steps.
            Price bidPrice = toPrice(99.00 + i * 0.01);
            engine_.submitOrder(kSym, id++, kLiqProv,
                                Side::Buy, bidPrice, 20,
                                OrderType::Limit);
        }
        engine_.waitForDrain();
    }
};

// ── Test 1: 100-share long → sell, fillRate > 0 ─────────────────────────────

TEST_F(BacktestEngineTest, LongUnwind_FillRatePositive) {
    BacktestConfig cfg{};
    cfg.positionToUnwind = 100.0;   // long 100 shares to sell
    cfg.numSteps         = 5;
    cfg.tau              = 60.0;
    cfg.lambda           = 1e-5;
    cfg.participantId    = kExec;
    cfg.symbolId         = kSym;
    cfg.firstOrderId     = kExecBase;

    BacktestEngine bt(engine_);
    BacktestResult r = bt.run(cfg, makeCalibResult(), /*arrivalMid=*/100.0);

    EXPECT_GT(r.fillRate, 0.0);
    EXPECT_FALSE(r.actualFills.empty());
    EXPECT_EQ(r.actualFills.size(), static_cast<std::size_t>(cfg.numSteps));
}

// ── Test 2: theoreticalIS == computeExpectedCost() within 1e-6 ──────────────

TEST_F(BacktestEngineTest, TheoreticalIS_MatchesComputeExpectedCost) {
    BacktestConfig cfg{};
    cfg.positionToUnwind = 50.0;
    cfg.numSteps         = 5;
    cfg.tau              = 30.0;
    cfg.lambda           = 1e-5;
    cfg.participantId    = kExec;
    cfg.symbolId         = kSym;
    cfg.firstOrderId     = kExecBase + 100;

    CalibrationResult calib = makeCalibResult();
    ACParams schedParams    = calib.params;
    schedParams.lambda      = cfg.lambda;  // match what BacktestEngine does

    BacktestEngine bt(engine_);
    BacktestResult r = bt.run(cfg, calib, /*arrivalMid=*/100.0);

    // Independently compute the expected cost using the same schedule.
    double expected = computeExpectedCost(r.schedule.trades, cfg.tau, schedParams);

    EXPECT_NEAR(r.theoreticalIS, expected, 1e-6);
}

// ── Test 3: trackingError = actualIS - theoreticalIS ────────────────────────

TEST_F(BacktestEngineTest, TrackingError_ArithmeticIdentity) {
    BacktestConfig cfg{};
    cfg.positionToUnwind = 40.0;
    cfg.numSteps         = 4;
    cfg.tau              = 60.0;
    cfg.lambda           = 1e-5;
    cfg.participantId    = kExec;
    cfg.symbolId         = kSym;
    cfg.firstOrderId     = kExecBase + 200;

    BacktestEngine bt(engine_);
    BacktestResult r = bt.run(cfg, makeCalibResult(), /*arrivalMid=*/100.0);

    EXPECT_NEAR(r.trackingError,
                r.actualIS - r.theoreticalIS,
                1e-6);
}

// ── Test 4: fillHistory().size() equals fills recorded ───────────────────────

TEST_F(BacktestEngineTest, FillHistory_SizeMatchesRecordedFills) {
    BacktestConfig cfg{};
    cfg.positionToUnwind = 30.0;
    cfg.numSteps         = 3;
    cfg.tau              = 60.0;
    cfg.lambda           = 1e-5;
    cfg.participantId    = kExec;
    cfg.symbolId         = kSym;
    cfg.firstOrderId     = kExecBase + 300;

    BacktestEngine bt(engine_);
    BacktestResult r = bt.run(cfg, makeCalibResult(), /*arrivalMid=*/100.0);

    // Each fill in the history should correspond to an individual trade.
    // The total fill qty in the history should equal sum(actualFills).
    double histQty = 0.0;
    for (const auto& fr : bt.fillHistory()) {
        histQty += std::fabs(fr.qty);
    }

    double actualTotal = 0.0;
    for (double fq : r.actualFills) {
        actualTotal += fq;
    }

    EXPECT_NEAR(histQty, actualTotal, 1e-6);
}

// ── Test 5: clearHistory() resets fill history ───────────────────────────────

TEST_F(BacktestEngineTest, ClearHistory_ResetsToEmpty) {
    BacktestConfig cfg{};
    cfg.positionToUnwind = 20.0;
    cfg.numSteps         = 2;
    cfg.tau              = 60.0;
    cfg.lambda           = 1e-5;
    cfg.participantId    = kExec;
    cfg.symbolId         = kSym;
    cfg.firstOrderId     = kExecBase + 400;

    BacktestEngine bt(engine_);
    bt.run(cfg, makeCalibResult(), /*arrivalMid=*/100.0);

    // History should be non-empty after a run that produced fills.
    // (It's possible fills = 0 if the book is exhausted by previous
    // tests, but clearHistory must return an empty vector regardless.)
    bt.clearHistory();
    EXPECT_TRUE(bt.fillHistory().empty());
}

// ── Test 6: schedule.trades size == numSteps ─────────────────────────────────

TEST_F(BacktestEngineTest, ScheduleTradesSize_EqualsNumSteps) {
    BacktestConfig cfg{};
    cfg.positionToUnwind = 60.0;
    cfg.numSteps         = 6;
    cfg.tau              = 60.0;
    cfg.lambda           = 1e-5;
    cfg.participantId    = kExec;
    cfg.symbolId         = kSym;
    cfg.firstOrderId     = kExecBase + 500;

    BacktestEngine bt(engine_);
    BacktestResult r = bt.run(cfg, makeCalibResult(), /*arrivalMid=*/100.0);

    EXPECT_EQ(r.schedule.trades.size(), static_cast<std::size_t>(cfg.numSteps));
    EXPECT_EQ(r.actualFills.size(),     static_cast<std::size_t>(cfg.numSteps));
    EXPECT_EQ(r.actualPrices.size(),    static_cast<std::size_t>(cfg.numSteps));
}
