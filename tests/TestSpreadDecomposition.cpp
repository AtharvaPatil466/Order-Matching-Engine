// TestSpreadDecomposition.cpp — GoogleTest suite for include/SpreadDecomposition.h.
//
// Tests cover:
//   1.  buildSpreadObservations: positive qty → direction = +1
//   2.  buildSpreadObservations: size mismatch → empty (NDEBUG) or skipped (debug)
//   3.  fitGlostenhHarris: fully informed flow → lambda > 0, ASF ≈ 1
//   4.  fitGlostenhHarris: pure processing cost → lambda ≈ 0, theta > 0
//   5.  fitGlostenhHarris: fewer than 5 obs → valid = false
//   6.  fitHuangStoll: no adverse selection → alpha ≈ 0, phi ≈ 1
//   7.  fitHuangStoll: full adverse selection → alpha close to 1
//   8.  fitHuangStoll: fewer than 5 obs → valid = false
//   9.  adverseSelectionFraction ∈ [0, 1] for any valid input
//   10. alpha + psi + phi ≈ 1.0 for valid Huang-Stoll result
//   11. all-zero midpoint changes → lambda = 0, no NaN/crash

#include "SpreadDecomposition.h"

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

using namespace OrderMatcher;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build a uniform sequence of alternating buy/sell observations.
// halfSpreadAmt : |tradePrice - midBefore| for every trade
// midJump       : midAfter - midBefore for every trade (quote revision)
static std::vector<SpreadObservation> makeObs(
    std::size_t n,
    double      halfSpreadAmt,
    double      midJump)
{
    std::vector<SpreadObservation> obs;
    obs.reserve(n);
    double mid = 100.0;
    for (std::size_t i = 0; i < n; ++i) {
        SpreadObservation o{};
        int dir      = (i % 2 == 0) ? +1 : -1;
        o.direction  = dir;
        o.midBefore  = mid;
        o.tradePrice = mid + dir * halfSpreadAmt;
        o.midAfter   = mid + dir * midJump;
        mid          = o.midAfter;
        obs.push_back(o);
    }
    return obs;
}

// ===========================================================================
// 1. buildSpreadObservations — positive qty → direction = +1
// ===========================================================================
TEST(BuildSpreadObservations, PositiveQtyGivesBuyDirection) {
    std::vector<TradeRecord> trades{ {100.05, +500.0} };
    std::vector<double>      mids{ 100.0, 100.05 };

    auto obs = buildSpreadObservations(trades, mids);
    ASSERT_EQ(obs.size(), 1u);
    EXPECT_EQ(obs[0].direction, +1);
    EXPECT_NEAR(obs[0].tradePrice, 100.05, 1e-9);
    EXPECT_NEAR(obs[0].midBefore,  100.0,  1e-9);
    EXPECT_NEAR(obs[0].midAfter,   100.05, 1e-9);
}

TEST(BuildSpreadObservations, NegativeQtyGivesSellDirection) {
    std::vector<TradeRecord> trades{ {99.95, -300.0} };
    std::vector<double>      mids{ 100.0, 99.95 };

    auto obs = buildSpreadObservations(trades, mids);
    ASSERT_EQ(obs.size(), 1u);
    EXPECT_EQ(obs[0].direction, -1);
}

// ===========================================================================
// 2. buildSpreadObservations — size mismatch
// ===========================================================================
TEST(BuildSpreadObservations, SizeMismatchDefensiveReturn) {
#ifdef NDEBUG
    std::vector<TradeRecord> trades{ {100.0, +100.0}, {100.1, -100.0} };
    std::vector<double>      mids{ 100.0, 100.05 };  // needs 3, only 2 provided
    auto obs = buildSpreadObservations(trades, mids);
    EXPECT_TRUE(obs.empty());
#else
    GTEST_SKIP() << "Size-mismatch triggers assert() in debug build; skipped.";
#endif
}

// ===========================================================================
// 3. fitGlostenhHarris — fully informed flow → lambda > 0, ASF ≈ 1
// ===========================================================================
TEST(FitGlostenhHarris, FullyInformedFlow) {
    // Pure adverse selection: midpoint moves full half-spread, trade AT mid
    // (halfSpreadAmt=0), so ΔP = 0 → theta = 0, lambda = midJump → ASF ≈ 1.
    const double hs = 0.05;
    auto obs = makeObs(20, 0.0, hs);

    auto r = fitGlostenhHarris(obs);
    ASSERT_TRUE(r.valid);
    EXPECT_GT(r.lambda, 0.0);
    EXPECT_NEAR(r.adverseSelectionFraction, 1.0, 0.05);
}

// ===========================================================================
// 4. fitGlostenhHarris — pure processing cost → lambda ≈ 0, theta > 0
// ===========================================================================
TEST(FitGlostenhHarris, PureProcessingCost) {
    // Midpoint never moves (midJump = 0).  Trade price deviates from mid
    // by halfSpreadAmt in the direction of the trade.
    // lambda = 0 (no quote revision), theta = halfSpreadAmt (price vs mid).
    const double hs = 0.05;
    auto obs = makeObs(20, hs, 0.0);

    auto r = fitGlostenhHarris(obs);
    ASSERT_TRUE(r.valid);
    EXPECT_NEAR(r.lambda, 0.0, 1e-9);
    EXPECT_GT(r.theta, 0.0);
    EXPECT_NEAR(r.adverseSelectionFraction, 0.0, 0.05);
}

// ===========================================================================
// 5. fitGlostenhHarris — fewer than 5 obs → valid = false
// ===========================================================================
TEST(FitGlostenhHarris, TooFewObservationsInvalid) {
    auto obs = makeObs(4, 0.05, 0.02);
    auto r   = fitGlostenhHarris(obs);
    EXPECT_FALSE(r.valid);
}

// ===========================================================================
// 6. fitHuangStoll — no adverse selection → alpha ≈ 0, phi ≈ 1
// ===========================================================================
TEST(FitHuangStoll, NoAdverseSelection) {
    // Midpoints never move → ΔM_t = 0 for all t → β = 0 → alpha = 0.
    const double hs = 0.05;
    auto obs = makeObs(20, hs, 0.0);

    auto r = fitHuangStoll(obs);
    ASSERT_TRUE(r.valid);
    EXPECT_NEAR(r.alpha, 0.0, 1e-9);
    EXPECT_NEAR(r.phi,   1.0, 1e-9);
}

// ===========================================================================
// 7. fitHuangStoll — full adverse selection → alpha close to 1
// ===========================================================================
TEST(FitHuangStoll, FullAdverseSelection) {
    // Grouped buys then sells so consecutive Q_{t-1}·ΔM_t terms are same sign.
    // midJump >> halfSpread so β/halfSpread >> 1 → alpha clamped to 1.
    // 10 consecutive buys: mid drifts up by 0.05 each trade.
    // 10 consecutive sells: mid drifts down by 0.05 each trade.
    const double halfSpread = 0.001;
    const double midJump    = 0.05;
    std::vector<SpreadObservation> obs;
    double mid = 100.0;
    for (int i = 0; i < 10; ++i) {
        SpreadObservation o{};
        o.direction  = +1;
        o.midBefore  = mid;
        o.tradePrice = mid + halfSpread;
        o.midAfter   = mid + midJump;
        mid          = o.midAfter;
        obs.push_back(o);
    }
    for (int i = 0; i < 10; ++i) {
        SpreadObservation o{};
        o.direction  = -1;
        o.midBefore  = mid;
        o.tradePrice = mid - halfSpread;
        o.midAfter   = mid - midJump;
        mid          = o.midAfter;
        obs.push_back(o);
    }

    auto r = fitHuangStoll(obs);
    ASSERT_TRUE(r.valid);
    EXPECT_NEAR(r.alpha, 1.0, 0.05);
    EXPECT_NEAR(r.phi,   0.0, 0.05);
}

// ===========================================================================
// 8. fitHuangStoll — fewer than 5 obs → valid = false
// ===========================================================================
TEST(FitHuangStoll, TooFewObservationsInvalid) {
    auto obs = makeObs(4, 0.05, 0.02);
    auto r   = fitHuangStoll(obs);
    EXPECT_FALSE(r.valid);
}

// ===========================================================================
// 9. adverseSelectionFraction ∈ [0, 1] for any valid input
// ===========================================================================
TEST(FitGlostenhHarris, AdverseSelectionFractionBounded) {
    // Use mixed signal: partial quote revision.
    auto obs = makeObs(12, 0.05, 0.02);
    auto r   = fitGlostenhHarris(obs);
    ASSERT_TRUE(r.valid);
    EXPECT_GE(r.adverseSelectionFraction, 0.0);
    EXPECT_LE(r.adverseSelectionFraction, 1.0);
}

// ===========================================================================
// 10. alpha + psi + phi ≈ 1.0 for valid Huang-Stoll result
// ===========================================================================
TEST(FitHuangStoll, ComponentsSumToOne) {
    auto obs = makeObs(14, 0.05, 0.02);
    auto r   = fitHuangStoll(obs);
    ASSERT_TRUE(r.valid);
    EXPECT_NEAR(r.alpha + r.psi + r.phi, 1.0, 1e-9);
}

// ===========================================================================
// 11. All-zero midpoint changes → lambda = 0, no NaN/crash
// ===========================================================================
TEST(FitGlostenhHarris, AllZeroMidpointChanges) {
    // midAfter == midBefore for every trade → ΔM = 0 → lambda = 0.
    // Also verify no NaN or Inf.
    const double hs = 0.05;
    auto obs = makeObs(10, hs, 0.0);

    auto r = fitGlostenhHarris(obs);
    ASSERT_TRUE(r.valid);
    EXPECT_NEAR(r.lambda, 0.0, 1e-9);
    EXPECT_FALSE(std::isnan(r.lambda));
    EXPECT_FALSE(std::isinf(r.lambda));
    EXPECT_FALSE(std::isnan(r.theta));
    EXPECT_FALSE(std::isinf(r.theta));
    EXPECT_FALSE(std::isnan(r.adverseSelectionFraction));
    EXPECT_FALSE(std::isinf(r.adverseSelectionFraction));
}
