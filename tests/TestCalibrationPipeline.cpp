// TestCalibrationPipeline.cpp — Google Test suite for CalibrationPipeline.h
//
// Tests:
//   1. calibrate() with 10 synthetic observations → valid=true, params finite
//   2. calibrate() with 4 observations → valid=false
//   3. calibrateFromJournal(): engine + 20 crossing orders → valid=true, tradesUsed >= 5
//   4. calibrate() with zero-impact observations → params non-negative, no crash

#include <gtest/gtest.h>

#include "CalibrationPipeline.h"
#include "ImpactModel.h"
#include "Journal.h"
#include "MatchingEngine.h"
#include "Types.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace OrderMatcher;

// ── Helper: build a synthetic TradeObservation vector ───────────────────────

static std::vector<TradeObservation> makeSyntheticObs(std::size_t n,
                                                       double advVal    = 1000.0,
                                                       double impactVal = 0.001)
{
    std::vector<TradeObservation> obs;
    obs.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        TradeObservation o;
        // Alternate buy/sell aggressors for variety.
        double sign    = ((i % 2) == 0) ? 1.0 : -1.0;
        double qty     = static_cast<double>(10 + i);
        o.tradeQty     = sign * qty;
        o.adv          = advVal;
        o.priceImpact  = sign * impactVal * qty / advVal;
        // Give non-trivial temp/perm components so OLS has something to fit.
        o.tempImpact   = 0.6 * o.priceImpact;
        o.permImpact   = 0.4 * o.priceImpact;
        obs.push_back(o);
    }
    return obs;
}

// ── Test 1: 10 observations → valid=true, all params finite ─────────────────

TEST(CalibrationPipelineTest, TenObservations_ValidResult) {
    auto obs = makeSyntheticObs(10);
    CalibrationResult r = calibrate(obs);

    EXPECT_TRUE(r.valid);
    EXPECT_EQ(r.tradesUsed, 10u);

    EXPECT_TRUE(std::isfinite(r.params.eta));
    EXPECT_TRUE(std::isfinite(r.params.gamma));
    EXPECT_TRUE(std::isfinite(r.params.sigma));
    EXPECT_TRUE(std::isfinite(r.params.lambda));

    // params must be non-negative (physical constraint).
    EXPECT_GE(r.params.eta,   0.0);
    EXPECT_GE(r.params.gamma, 0.0);
    EXPECT_GE(r.params.sigma, 0.0);
    EXPECT_GE(r.params.lambda, 0.0);
}

// ── Test 2: 4 observations → valid=false ────────────────────────────────────

TEST(CalibrationPipelineTest, FourObservations_NotValid) {
    auto obs = makeSyntheticObs(4);
    CalibrationResult r = calibrate(obs);

    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.tradesUsed, 4u);
}

// ── Test 3: calibrateFromJournal with a temp engine ─────────────────────────

class CalibrationJournalTest : public ::testing::Test {
protected:
    static constexpr SymbolId     kSym    = 1;
    static constexpr ParticipantId kBuyer  = 10;
    static constexpr ParticipantId kSeller = 20;

    MatchingEngine engine_;
    std::string    journalPath_;

    void SetUp() override {
        journalPath_ = (fs::temp_directory_path() /
                        ("calib_test_" + std::to_string(::getpid()) + "_" +
                         std::to_string(reinterpret_cast<uintptr_t>(this)) +
                         ".log")).string();
        fs::remove(journalPath_);

        engine_.addSymbol(kSym);
        engine_.start();
    }

    void TearDown() override {
        engine_.stop();
        fs::remove(journalPath_);
    }

    // Write 20 crossing pairs (10 sell+buy pairs = 10 trades) to the journal.
    // Each pair uses slightly different prices to generate mid movement.
    void writeJournal() {
        Journal j(journalPath_, Journal::SyncPolicy::Immediate, 1);
        OrderId id = 1;
        // We create resting liquidity first (10 sell orders at slightly
        // different prices to give us mid movement), then cross each one.
        for (int i = 0; i < 10; ++i) {
            // Use integer prices in fixed-point (price = ticks * PRICE_PRECISION)
            // Vary price slightly to create mid-price movement.
            Price sellPrice = toPrice(100.0 + i * 0.01);
            Price buyPrice  = sellPrice;  // aggressive buyer matches the ask

            j.logAddOrder(id++, kSeller, kSym, Side::Sell,
                          sellPrice, 10, OrderType::Limit);
            j.logAddOrder(id++, kBuyer,  kSym, Side::Buy,
                          buyPrice,  10, OrderType::Limit);
        }
        j.flush();
    }
};

TEST_F(CalibrationJournalTest, CalibrateFromJournal_ValidResult) {
    writeJournal();

    // Use adv=0 so the pipeline derives it from the replay volume.
    CalibrationResult r = calibrateFromJournal(engine_, kSym, journalPath_, 0.0);

    EXPECT_TRUE(r.valid);
    EXPECT_GE(r.tradesUsed, 5u);
    EXPECT_GT(r.adv, 0.0);

    EXPECT_TRUE(std::isfinite(r.params.eta));
    EXPECT_TRUE(std::isfinite(r.params.gamma));
    EXPECT_GE(r.params.eta,   0.0);
    EXPECT_GE(r.params.gamma, 0.0);
}

// ── Test 4: zero-impact observations → non-negative params, no crash ─────────

TEST(CalibrationPipelineTest, ZeroImpactObservations_NoNaNNoNegative) {
    // All impacts are exactly zero; OLS through origin → coefficients = 0.
    std::vector<TradeObservation> obs;
    for (int i = 0; i < 8; ++i) {
        TradeObservation o;
        o.tradeQty    = static_cast<double>(10 + i);
        o.adv         = 500.0;
        o.priceImpact = 0.0;
        o.tempImpact  = 0.0;
        o.permImpact  = 0.0;
        obs.push_back(o);
    }

    CalibrationResult r = calibrate(obs);

    EXPECT_TRUE(r.valid);

    EXPECT_TRUE(std::isfinite(r.params.eta));
    EXPECT_TRUE(std::isfinite(r.params.gamma));
    EXPECT_GE(r.params.eta,   0.0);
    EXPECT_GE(r.params.gamma, 0.0);
}

// ── Test 5: toACParams bridge sanity ─────────────────────────────────────────

TEST(CalibrationPipelineTest, ToACParams_Bridge_FieldMapping) {
    AlmgrenChrisParams ac{0.05, 0.02, 0.9, 0.8};
    ACParams p = toACParams(ac, 0.01, 1e-5);

    EXPECT_NEAR(p.eta,    0.05,  1e-12);
    EXPECT_NEAR(p.gamma,  0.02,  1e-12);
    EXPECT_NEAR(p.sigma,  0.01,  1e-12);
    EXPECT_NEAR(p.lambda, 1e-5,  1e-16);
}

// ── Test 6: exactly 5 observations → valid=true (boundary) ───────────────────

TEST(CalibrationPipelineTest, FiveObservations_BoundaryValid) {
    auto obs = makeSyntheticObs(5);
    CalibrationResult r = calibrate(obs);

    EXPECT_TRUE(r.valid);
    EXPECT_EQ(r.tradesUsed, 5u);
}
