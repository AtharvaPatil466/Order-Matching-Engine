// TestImpactModel.cpp — GoogleTest suite for include/ImpactModel.h.
// Tests cover: buildObservations, fitSqrtLaw, fitAlmgrenChriss,
// predict*, edge-cases (zero impact, <2 observations, size mismatch).

#include "ImpactModel.h"

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

using namespace OrderMatcher;

// ---------------------------------------------------------------------------
// Helper: construct a minimal Trade with the given quantity and aggressor side.
// ---------------------------------------------------------------------------
static Trade makeTrade(Quantity qty, Side side) {
    Trade t{};
    t.quantity      = qty;
    t.aggressorSide = side;
    return t;
}

// ---------------------------------------------------------------------------
// Helper: generate observations that follow the sqrt-law model exactly.
//   impact_i = c * sign(q_i) * sqrt(|q_i| / adv)
// ---------------------------------------------------------------------------
static std::vector<TradeObservation> makeSqrtLawData(
    double c, double adv, const std::vector<double>& qtys)
{
    std::vector<TradeObservation> obs;
    for (double q : qtys) {
        TradeObservation o{};
        o.tradeQty    = q;
        o.adv         = adv;
        double sign   = (q >= 0.0) ? 1.0 : -1.0;
        o.priceImpact = c * sign * std::sqrt(std::fabs(q) / adv);
        obs.push_back(o);
    }
    return obs;
}

// ---------------------------------------------------------------------------
// Helper: generate observations that follow the Almgren-Chriss model exactly.
//   tempImpact_i = eta   * v_i^0.6,   v_i = |q_i| / adv
//   permImpact_i = gamma * v_i
// ---------------------------------------------------------------------------
static std::vector<TradeObservation> makeACData(
    double eta, double gamma, double adv,
    const std::vector<double>& qtys)
{
    std::vector<TradeObservation> obs;
    for (double q : qtys) {
        TradeObservation o{};
        o.tradeQty    = q;
        o.adv         = adv;
        double v      = std::fabs(q) / adv;
        o.tempImpact  = eta   * std::pow(v, 0.6);
        o.permImpact  = gamma * v;
        o.priceImpact = o.tempImpact + o.permImpact;
        obs.push_back(o);
    }
    return obs;
}

// ===========================================================================
// buildObservations
// ===========================================================================

TEST(BuildObservations, BuyAggressorGivesPositiveQty) {
    std::vector<Trade>  trades{ makeTrade(500, Side::Buy) };
    std::vector<double> mids{ 100.0, 100.1 };
    auto obs = buildObservations(trades, mids, 10000.0);
    ASSERT_EQ(obs.size(), 1u);
    EXPECT_NEAR(obs[0].tradeQty, 500.0, 1e-9);
}

TEST(BuildObservations, SellAggressorGivesNegativeQty) {
    std::vector<Trade>  trades{ makeTrade(300, Side::Sell) };
    std::vector<double> mids{ 100.0, 99.9 };
    auto obs = buildObservations(trades, mids, 10000.0);
    ASSERT_EQ(obs.size(), 1u);
    EXPECT_NEAR(obs[0].tradeQty, -300.0, 1e-9);
}

TEST(BuildObservations, PriceImpactIsCorrect) {
    // (101 - 100) / 100 = 0.01
    std::vector<Trade>  trades{ makeTrade(100, Side::Buy) };
    std::vector<double> mids{ 100.0, 101.0 };
    auto obs = buildObservations(trades, mids, 5000.0);
    ASSERT_EQ(obs.size(), 1u);
    EXPECT_NEAR(obs[0].priceImpact, 0.01, 1e-9);
}

TEST(BuildObservations, NegativePriceImpactFromSellAggressor) {
    std::vector<Trade>  trades{ makeTrade(200, Side::Sell) };
    std::vector<double> mids{ 100.0, 99.0 };
    auto obs = buildObservations(trades, mids, 5000.0);
    ASSERT_EQ(obs.size(), 1u);
    EXPECT_NEAR(obs[0].priceImpact, -0.01, 1e-9);
}

TEST(BuildObservations, MultipleTradesCorrectCount) {
    std::vector<Trade> trades{
        makeTrade(100, Side::Buy),
        makeTrade(200, Side::Sell),
        makeTrade(150, Side::Buy)
    };
    std::vector<double> mids{ 100.0, 100.05, 99.95, 100.02 };
    auto obs = buildObservations(trades, mids, 10000.0);
    ASSERT_EQ(obs.size(), 3u);
    EXPECT_GT(obs[0].tradeQty,  0.0);
    EXPECT_LT(obs[1].tradeQty,  0.0);
    EXPECT_GT(obs[2].tradeQty,  0.0);
}

TEST(BuildObservations, AdvIsPassedThrough) {
    std::vector<Trade>  trades{ makeTrade(500, Side::Buy) };
    std::vector<double> mids{ 100.0, 100.1 };
    auto obs = buildObservations(trades, mids, 12345.0);
    ASSERT_EQ(obs.size(), 1u);
    EXPECT_NEAR(obs[0].adv, 12345.0, 1e-9);
}

// Size-mismatch guard: in release (NDEBUG) the function returns empty;
// in debug the assert fires and we skip rather than crash the test run.
TEST(BuildObservations, SizeMismatchDefensiveReturn) {
#ifdef NDEBUG
    std::vector<Trade>  trades{ makeTrade(100, Side::Buy),
                                 makeTrade(200, Side::Sell) };
    std::vector<double> mids{ 100.0, 101.0 }; // needs 3, only 2 provided
    auto obs = buildObservations(trades, mids, 5000.0);
    EXPECT_TRUE(obs.empty());
#else
    GTEST_SKIP() << "Size-mismatch triggers assert() in debug build; skipped.";
#endif
}

// ===========================================================================
// fitSqrtLaw
// ===========================================================================

TEST(FitSqrtLaw, ExactFitGivesRSquaredOne) {
    const double c   = 0.314;
    const double adv = 1'000'000.0;
    auto obs = makeSqrtLawData(c, adv,
        { 10000, 50000, 200000, -30000, -80000, 120000 });
    auto p = fitSqrtLaw(obs);
    // sigma is fixed to 1.0 after fitting; c is stored in eta.
    EXPECT_NEAR(p.eta * p.sigma, c,   1e-9);
    EXPECT_NEAR(p.r2,             1.0, 1e-9);
}

TEST(FitSqrtLaw, ZeroImpactReturnsZeroEtaAndZeroR2) {
    std::vector<TradeObservation> obs(5);
    for (auto& o : obs) {
        o.tradeQty    = 1000.0;
        o.adv         = 1'000'000.0;
        o.priceImpact = 0.0;
    }
    auto p = fitSqrtLaw(obs);
    EXPECT_NEAR(p.eta, 0.0, 1e-15);
    // R² is 0/0 when SS_tot = 0; guard returns 0.
    EXPECT_NEAR(p.r2,  0.0, 1e-15);
    // No NaN/Inf
    EXPECT_FALSE(std::isnan(p.eta));
    EXPECT_FALSE(std::isnan(p.r2));
}

TEST(FitSqrtLaw, ZeroObservationsReturnsZeroParams) {
    auto p = fitSqrtLaw({});
    EXPECT_NEAR(p.eta,   0.0, 1e-15);
    EXPECT_NEAR(p.sigma, 0.0, 1e-15);
    EXPECT_NEAR(p.r2,    0.0, 1e-15);
}

TEST(FitSqrtLaw, OneObservationReturnsZeroParams) {
    TradeObservation single{};
    single.tradeQty    = 5000.0;
    single.adv         = 100000.0;
    single.priceImpact = 0.003;
    auto p = fitSqrtLaw({ single });
    EXPECT_NEAR(p.eta,   0.0, 1e-15);
    EXPECT_NEAR(p.sigma, 0.0, 1e-15);
    EXPECT_NEAR(p.r2,    0.0, 1e-15);
}

TEST(FitSqrtLaw, NaNAndInfFreeOutput) {
    auto obs = makeSqrtLawData(0.2, 500000.0, { 10000, 50000, -20000 });
    auto p   = fitSqrtLaw(obs);
    EXPECT_FALSE(std::isnan(p.eta));
    EXPECT_FALSE(std::isinf(p.eta));
    EXPECT_FALSE(std::isnan(p.r2));
    EXPECT_FALSE(std::isinf(p.r2));
}

// ===========================================================================
// predictSqrtLaw
// ===========================================================================

TEST(PredictSqrtLaw, DoublingQtyIncreasesBySquareRootTwo) {
    SqrtLawParams p{ 0.5, 1.0, 0.99 };
    double adv  = 1'000'000.0;
    double qty1 = 100'000.0;
    double qty2 = qty1 * 2.0;
    double imp1 = predictSqrtLaw(p, qty1, adv);
    double imp2 = predictSqrtLaw(p, qty2, adv);
    EXPECT_NEAR(imp2 / imp1, std::sqrt(2.0), 1e-9);
}

TEST(PredictSqrtLaw, SignFollowsQtySign) {
    SqrtLawParams p{ 0.3, 1.0, 0.9 };
    double adv = 500000.0;
    EXPECT_GT(predictSqrtLaw(p, +10000.0, adv), 0.0);
    EXPECT_LT(predictSqrtLaw(p, -10000.0, adv), 0.0);
}

TEST(PredictSqrtLaw, ZeroAdvReturnsZero) {
    SqrtLawParams p{ 0.3, 1.0, 0.9 };
    EXPECT_NEAR(predictSqrtLaw(p, 10000.0, 0.0), 0.0, 1e-15);
}

TEST(PredictSqrtLaw, ConsistentWithFitParams) {
    const double c   = 0.25;
    const double adv = 800000.0;
    auto obs = makeSqrtLawData(c, adv, { 10000, 40000, 160000, -20000, -90000 });
    auto p   = fitSqrtLaw(obs);

    EXPECT_NEAR(predictSqrtLaw(p, 10000.0, adv),
                c * std::sqrt(10000.0 / adv), 1e-9);
    EXPECT_NEAR(predictSqrtLaw(p, -50000.0, adv),
                -c * std::sqrt(50000.0 / adv), 1e-9);
}

// ===========================================================================
// fitAlmgrenChriss
// ===========================================================================

TEST(FitAlmgrenChriss, ExactFitGivesRSquaredOne) {
    const double eta   = 0.142;
    const double gamma = 0.071;
    const double adv   = 2'000'000.0;
    auto obs = makeACData(eta, gamma, adv,
        { 20000, 80000, 300000, 50000, 150000, 600000 });
    auto p = fitAlmgrenChriss(obs);
    EXPECT_NEAR(p.eta,    eta,   1e-9);
    EXPECT_NEAR(p.gamma,  gamma, 1e-9);
    EXPECT_NEAR(p.r2Temp, 1.0,   1e-9);
    EXPECT_NEAR(p.r2Perm, 1.0,   1e-9);
}

TEST(FitAlmgrenChriss, ZeroObservationsReturnsZeroParams) {
    auto p = fitAlmgrenChriss({});
    EXPECT_NEAR(p.eta,    0.0, 1e-15);
    EXPECT_NEAR(p.gamma,  0.0, 1e-15);
    EXPECT_NEAR(p.r2Temp, 0.0, 1e-15);
    EXPECT_NEAR(p.r2Perm, 0.0, 1e-15);
}

TEST(FitAlmgrenChriss, OneObservationReturnsZeroParams) {
    TradeObservation single{};
    single.tradeQty   = 5000.0;
    single.adv        = 100000.0;
    single.tempImpact = 0.001;
    single.permImpact = 0.0005;
    auto p = fitAlmgrenChriss({ single });
    EXPECT_NEAR(p.eta,   0.0, 1e-15);
    EXPECT_NEAR(p.gamma, 0.0, 1e-15);
}

TEST(FitAlmgrenChriss, NaNAndInfFreeOutput) {
    auto obs = makeACData(0.1, 0.05, 1000000.0, { 10000, 50000, -20000 });
    auto p   = fitAlmgrenChriss(obs);
    EXPECT_FALSE(std::isnan(p.eta));
    EXPECT_FALSE(std::isinf(p.eta));
    EXPECT_FALSE(std::isnan(p.gamma));
    EXPECT_FALSE(std::isinf(p.gamma));
    EXPECT_FALSE(std::isnan(p.r2Temp));
    EXPECT_FALSE(std::isinf(p.r2Temp));
    EXPECT_FALSE(std::isnan(p.r2Perm));
    EXPECT_FALSE(std::isinf(p.r2Perm));
}

// ===========================================================================
// predictTempImpact / predictPermImpact
// ===========================================================================

TEST(PredictACImpact, TempAndPermConsistentWithFitParams) {
    const double eta   = 0.18;
    const double gamma = 0.09;
    const double adv   = 1'500'000.0;
    auto obs = makeACData(eta, gamma, adv,
        { 30000, 90000, 270000, 810000, 60000 });
    auto p = fitAlmgrenChriss(obs);

    double qty     = 120000.0;
    double v       = qty / adv;
    double expTemp = eta   * std::pow(v, 0.6);
    double expPerm = gamma * v;

    EXPECT_NEAR(predictTempImpact(p, qty, adv), expTemp, 1e-9);
    EXPECT_NEAR(predictPermImpact(p, qty, adv), expPerm, 1e-9);
}

TEST(PredictACImpact, ZeroAdvReturnsZero) {
    AlmgrenChrisParams p{ 0.1, 0.05, 0.99, 0.99 };
    EXPECT_NEAR(predictTempImpact(p, 10000.0, 0.0), 0.0, 1e-15);
    EXPECT_NEAR(predictPermImpact(p, 10000.0, 0.0), 0.0, 1e-15);
}

TEST(PredictACImpact, TempImpactIsAlwaysNonNegative) {
    // v^0.6 is always >= 0; eta >= 0 by construction of our fit.
    AlmgrenChrisParams p{ 0.2, 0.1, 0.95, 0.95 };
    double adv = 1000000.0;
    EXPECT_GE(predictTempImpact(p,  50000.0, adv), 0.0);
    EXPECT_GE(predictTempImpact(p, -50000.0, adv), 0.0); // uses |qty|
}

TEST(PredictACImpact, PermImpactScalesLinearly) {
    AlmgrenChrisParams p{ 0.1, 0.05, 0.95, 0.95 };
    double adv = 1000000.0;
    double imp1 = predictPermImpact(p, 100000.0, adv);
    double imp2 = predictPermImpact(p, 200000.0, adv);
    // Linear in v = |q|/adv, so doubling qty doubles perm impact.
    EXPECT_NEAR(imp2 / imp1, 2.0, 1e-9);
}
