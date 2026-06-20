// TestSignalGenerator.cpp — GoogleTest suite for include/SignalGenerator.h.
// Covers all 11 required test cases.

#include "SignalGenerator.h"

#include <cmath>
#include <gtest/gtest.h>

using namespace OrderMatcher;

// ---------------------------------------------------------------------------
// Helper: build a neutral snapshot (all zeros / sensible defaults).
// ---------------------------------------------------------------------------
static MicrostructureSnapshot makeSnap(
    double ofi        = 0.0,
    double qi         = 0.0,
    double vpin       = 0.0,
    double rollSpread = 0.01,
    double lambda     = 0.0,
    double mid        = 100.0,
    double prevMid    = 100.0)
{
    MicrostructureSnapshot s{};
    s.orderFlowImbalance = ofi;
    s.queueImbalance     = qi;
    s.vpin               = vpin;
    s.rollSpread         = rollSpread;
    s.kylesLambda        = lambda;
    s.midPrice           = mid;
    s.prevMidPrice       = prevMid;
    s.arrivalRate        = 0.0;
    s.timestampNs        = 0;
    return s;
}

// ===========================================================================
// 1. orderFlowPressure: OFI=1, qi=1, vpin=0 → value ≈ 1.0
// ===========================================================================
TEST(OrderFlowPressure, StrongBuyWhenFullImbalanceNoToxicity) {
    auto s   = makeSnap(/*ofi=*/1.0, /*qi=*/1.0, /*vpin=*/0.0);
    Signal r = SignalGenerator::orderFlowPressure(s);
    // (0.6*1 + 0.4*1) * (1-0) = 1.0
    EXPECT_NEAR(r.value, 1.0, 1e-9);
    EXPECT_GE(r.confidence, 0.0);
    EXPECT_LE(r.confidence, 1.0);
}

// ===========================================================================
// 2. orderFlowPressure: vpin=1.0 → value ≈ 0 (fully dampened)
// ===========================================================================
TEST(OrderFlowPressure, DampenedToZeroWhenVpinIsOne) {
    auto s   = makeSnap(/*ofi=*/1.0, /*qi=*/1.0, /*vpin=*/1.0);
    Signal r = SignalGenerator::orderFlowPressure(s);
    EXPECT_NEAR(r.value, 0.0, 1e-9);
    EXPECT_NEAR(r.confidence, 0.0, 1e-9);
}

// ===========================================================================
// 3. momentumSignal: price up 2× rollSpread → clamped to 1.0
// ===========================================================================
TEST(MomentumSignal, ClampedToOneWhenPriceMoveExceedsSpread) {
    // mid - prevMid = 2 * rollSpread → raw ratio = 2.0, clamped → 1.0
    auto s   = makeSnap(0.0, 0.0, 0.0, /*rollSpread=*/0.5,
                        0.0, /*mid=*/101.0, /*prevMid=*/100.0);
    Signal r = SignalGenerator::momentumSignal(s);
    // (101 - 100) / 0.5 = 2.0 → clamped to 1.0
    EXPECT_NEAR(r.value, 1.0, 1e-9);
    EXPECT_NEAR(r.confidence, 1.0, 1e-9);
}

// ===========================================================================
// 4. momentumSignal: rollSpread=0 → confidence = 0
// ===========================================================================
TEST(MomentumSignal, ZeroConfidenceWhenSpreadIsZero) {
    auto s   = makeSnap(0.0, 0.0, 0.0, /*rollSpread=*/0.0,
                        0.0, 100.1, 100.0);
    Signal r = SignalGenerator::momentumSignal(s);
    EXPECT_NEAR(r.confidence, 0.0, 1e-9);
    EXPECT_NEAR(r.value,      0.0, 1e-9);
}

// ===========================================================================
// 5. adverseSelectionSignal: positive OFI + high lambda → negative signal
// ===========================================================================
TEST(AdverseSelectionSignal, FadesPositiveOfiWhenHighLambda) {
    // lambda = referenceImpact → clamp(1.0/1.0) = 1.0, sign(ofi>0) = +1
    // value = -(+1) * 1.0 = -1.0
    auto s   = makeSnap(/*ofi=*/0.8, 0.0, 0.0, 0.01,
                        /*lambda=*/1e-4);
    Signal r = SignalGenerator::adverseSelectionSignal(s, /*refImpact=*/1e-4);
    EXPECT_LT(r.value, 0.0);
    EXPECT_NEAR(r.value, -1.0, 1e-9);
    EXPECT_NEAR(r.confidence, 1.0, 1e-9);
}

// ===========================================================================
// 6. adverseSelectionSignal: kylesLambda=0 → confidence = 0
// ===========================================================================
TEST(AdverseSelectionSignal, ZeroConfidenceWhenLambdaIsZero) {
    auto s   = makeSnap(0.5, 0.0, 0.0, 0.01, /*lambda=*/0.0);
    Signal r = SignalGenerator::adverseSelectionSignal(s);
    EXPECT_NEAR(r.confidence, 0.0, 1e-9);
    EXPECT_NEAR(r.value,      0.0, 1e-9);
}

// ===========================================================================
// 7. compositeSignal: value always in [-1, 1] for any snapshot
// ===========================================================================
TEST(CompositeSignal, ValueAlwaysInBounds) {
    // Several extreme snapshots.
    const std::vector<MicrostructureSnapshot> snaps = {
        makeSnap( 1.0,  1.0, 0.0, 0.01, 1e-3, 101.0, 100.0),
        makeSnap(-1.0, -1.0, 1.0, 0.01, 0.0,   99.0, 100.0),
        makeSnap( 0.0,  0.0, 0.5, 0.0,  0.0,  100.0, 100.0),
        makeSnap( 0.3, -0.7, 0.2, 0.05, 2e-4, 100.1, 100.0),
    };
    for (const auto& s : snaps) {
        Signal r = SignalGenerator::compositeSignal(s);
        EXPECT_GE(r.value, -1.0);
        EXPECT_LE(r.value,  1.0);
        EXPECT_GE(r.confidence, 0.0);
        EXPECT_LE(r.confidence, 1.0);
    }
}

// ===========================================================================
// 8. recordSnapshot + meanSignal: constant snapshots → mean ≈ that constant
// ===========================================================================
TEST(SignalHistory, MeanSignalEqualsConstantValue) {
    SignalGenerator gen;
    // Snapshot that produces orderFlowPressure = (0.6*1+0.4*1)*(1-0) = 1.0
    // but momentum = (100-100)/spread = 0 and adverse = 0 (lambda=0)
    // composite weights: ofp conf = |1.0|*(1-0)=1.0, mom conf=1.0, adv conf=0
    // wSum = 0.4*1 + 0.3*1 + 0.3*0 = 0.7
    // val  = (0.4*1*1 + 0.3*1*0 + 0.3*0*0) / 0.7 = 0.4/0.7
    auto s = makeSnap(1.0, 1.0, 0.0, /*rollSpread=*/0.5, 0.0, 100.0, 100.0);
    const int N = 10;
    for (int i = 0; i < N; ++i) gen.recordSnapshot(s);

    Signal expected = SignalGenerator::compositeSignal(s);
    EXPECT_NEAR(gen.meanSignal(), expected.value, 1e-9);
}

// ===========================================================================
// 9. sharpeSignal: constant signal → stdDev = 0, sharpe = 0
// ===========================================================================
TEST(SignalHistory, SharpeIsZeroWhenConstantSignal) {
    SignalGenerator gen;
    auto s = makeSnap(0.5, 0.5, 0.0, 0.01, 0.0, 100.0, 100.0);
    for (int i = 0; i < 15; ++i) gen.recordSnapshot(s);

    EXPECT_NEAR(gen.signalStdDev(), 0.0, 1e-9);
    EXPECT_NEAR(gen.sharpeSignal(), 0.0, 1e-9);
}

// ===========================================================================
// 10. reset() clears history
// ===========================================================================
TEST(SignalHistory, ResetClearsHistory) {
    SignalGenerator gen;
    auto s = makeSnap(0.5, 0.5, 0.0, 0.01, 0.0, 100.0, 100.0);
    for (int i = 0; i < 5; ++i) gen.recordSnapshot(s);

    gen.reset();

    EXPECT_NEAR(gen.meanSignal(), 0.0, 1e-9);
    EXPECT_NEAR(gen.signalStdDev(), 0.0, 1e-9);
    EXPECT_NEAR(gen.sharpeSignal(), 0.0, 1e-9);
}

// ===========================================================================
// 11. All individual signals have value ∈ [-1, 1] invariant
// ===========================================================================
TEST(AllSignals, ValuesAreInBoundsForExtremeInputs) {
    const double refImpact = 1e-4;
    std::vector<MicrostructureSnapshot> snaps = {
        makeSnap( 1.0,  1.0, 0.0, 0.001, 1e-3, 200.0, 100.0),
        makeSnap(-1.0, -1.0, 1.0, 100.0, 0.0,   50.0, 200.0),
        makeSnap( 0.0,  0.0, 0.5, 0.0,   0.0,  100.0, 100.0),
        makeSnap( 1.0, -1.0, 0.3, 0.01,  5e-4,  99.0, 100.0),
    };
    for (const auto& s : snaps) {
        auto ofp = SignalGenerator::orderFlowPressure(s);
        auto mom = SignalGenerator::momentumSignal(s);
        auto adv = SignalGenerator::adverseSelectionSignal(s, refImpact);
        auto cmp = SignalGenerator::compositeSignal(s, refImpact);

        for (const Signal* sig : { &ofp, &mom, &adv, &cmp }) {
            EXPECT_GE(sig->value, -1.0) << sig->name;
            EXPECT_LE(sig->value,  1.0) << sig->name;
            EXPECT_GE(sig->confidence, 0.0) << sig->name;
            EXPECT_LE(sig->confidence, 1.0) << sig->name;
        }
    }
}
