// RiskControlsTest — hot-path pre-trade risk controls (P2-8 … P2-11).
//
// Exercised in SYNC engine mode: submitOrder / submitCancel run inline on the
// calling thread, so the check → book → accrual sequence is deterministic and
// every fill fires the OcoBookListener::onTrade accrual hook synchronously.
//
//   P2-8  Kill switch    — blocks new orders + cancels all resting orders.
//   P2-9  Position limit  — net-exposure cap; reserve on submit, accrue taker
//                           fills via onTrade, release on cancel.
//   P2-10 Fat-finger      — max qty / ±price band / max notional, per instrument.
//   P2-11 OTR throttle     — per-participant order-to-trade ratio, rolling window.

#include "MatchingEngine.h"
#include "OrderBook.h"
#include "Types.h"

#include <gtest/gtest.h>

using namespace OrderMatcher;

namespace {

constexpr SymbolId kSym = 1;
constexpr Price kRef = 1'000'000;  // 100.0000 in fixed point

// Fresh running engine with a single registered symbol.
struct RiskFixture : public ::testing::Test {
    MatchingEngine engine;
    void SetUp() override {
        engine.addSymbol(kSym);
        engine.start();
    }
    SubmitResult buy(OrderId id, ParticipantId pid, Price px, Quantity qty,
                     OrderType t = OrderType::Limit) {
        return engine.submitOrder(kSym, id, pid, Side::Buy, px, qty, t);
    }
    SubmitResult sell(OrderId id, ParticipantId pid, Price px, Quantity qty,
                      OrderType t = OrderType::Limit) {
        return engine.submitOrder(kSym, id, pid, Side::Sell, px, qty, t);
    }
    bool resting(OrderId id) { return engine.getOrderBook(kSym)->getOrder(id) != nullptr; }
};

}  // namespace

// ─── P2-8 KILL SWITCH ───────────────────────────────────────────────────────

TEST_F(RiskFixture, KillSwitchBlocksNewOrders) {
    EXPECT_TRUE(buy(1, 1, kRef, 10).isAccepted());

    engine.setKillSwitch(true);
    EXPECT_TRUE(engine.isKillSwitchActive());

    auto r = buy(2, 1, kRef, 10);
    EXPECT_FALSE(r.isAccepted());
    EXPECT_EQ(r.rejectReason, RejectReason::KillSwitchActive);
    EXPECT_EQ(engine.getKillSwitchRejectCount(), 1u);
}

TEST_F(RiskFixture, KillSwitchCancelsRestingOrders) {
    // Two resting orders that do not cross.
    ASSERT_TRUE(buy(10, 1, 990'000, 5).isAccepted());
    ASSERT_TRUE(sell(11, 2, 1'010'000, 5).isAccepted());
    ASSERT_TRUE(resting(10));
    ASSERT_TRUE(resting(11));

    engine.setKillSwitch(true);

    EXPECT_FALSE(resting(10));
    EXPECT_FALSE(resting(11));
}

TEST_F(RiskFixture, KillSwitchReleaseRestoresSubmissions) {
    engine.setKillSwitch(true);
    ASSERT_FALSE(buy(1, 1, kRef, 10).isAccepted());

    engine.setKillSwitch(false);
    EXPECT_FALSE(engine.isKillSwitchActive());
    EXPECT_TRUE(buy(2, 1, kRef, 10).isAccepted());
}

// ─── P2-9 POSITION LIMITS ───────────────────────────────────────────────────

TEST_F(RiskFixture, PositionAcceptsBelowLimit) {
    engine.setPositionLimit(1, 100);
    EXPECT_TRUE(buy(1, 1, 990'000, 50).isAccepted());  // rests → reserves 50
    EXPECT_EQ(engine.getPosition(1), 50);
}

TEST_F(RiskFixture, PositionRejectsOverLimitLong) {
    engine.setPositionLimit(1, 100);
    auto r = buy(1, 1, 990'000, 150);
    EXPECT_FALSE(r.isAccepted());
    EXPECT_EQ(r.rejectReason, RejectReason::PositionLimitExceeded);
    EXPECT_GE(engine.getPositionRejectCount(), 1u);
    EXPECT_EQ(engine.getPosition(1), 0);  // rejected order reserved nothing
}

TEST_F(RiskFixture, PositionRejectsOverLimitShort) {
    engine.setPositionLimit(1, 100);
    auto r = sell(1, 1, 1'010'000, 150);  // projected -150 < -100
    EXPECT_FALSE(r.isAccepted());
    EXPECT_EQ(r.rejectReason, RejectReason::PositionLimitExceeded);
}

TEST_F(RiskFixture, PositionFillIncrementsViaOnTrade) {
    engine.setPositionLimit(1, 100);           // taker limited
    ASSERT_TRUE(sell(20, 2, kRef, 80).isAccepted());  // maker resting sell
    ASSERT_TRUE(buy(21, 1, kRef, 80).isAccepted());   // taker fully fills

    EXPECT_EQ(engine.getPosition(1), 80);      // accrued by onTrade (taker)
    EXPECT_EQ(engine.getPosition(2), -80);     // maker reserved short at submit

    // Now 80 + 30 = 110 > 100 → reject.
    auto r = buy(22, 1, kRef, 30);
    EXPECT_FALSE(r.isAccepted());
    EXPECT_EQ(r.rejectReason, RejectReason::PositionLimitExceeded);
}

TEST_F(RiskFixture, PositionCancelReleasesExposure) {
    engine.setPositionLimit(1, 100);
    ASSERT_TRUE(buy(30, 1, 990'000, 80).isAccepted());  // rests → reserves 80
    ASSERT_EQ(engine.getPosition(1), 80);

    // 80 + 30 = 110 > 100 → reject while the working order holds exposure.
    EXPECT_EQ(buy(31, 1, 990'000, 30).rejectReason, RejectReason::PositionLimitExceeded);

    engine.cancelOrder(kSym, 30);              // releases the reserved 80
    EXPECT_EQ(engine.getPosition(1), 0);

    EXPECT_TRUE(buy(32, 1, 990'000, 30).isAccepted());
    EXPECT_EQ(engine.getPosition(1), 30);
}

// ─── P2-10 FAT-FINGER ───────────────────────────────────────────────────────

TEST_F(RiskFixture, FatFingerRejectsExcessiveQuantity) {
    engine.setFatFingerLimits(kSym, /*maxQty=*/1000, /*devPct=*/0.0, /*maxNotional=*/0);
    auto r = buy(1, 1, 990'000, 2000);
    EXPECT_FALSE(r.isAccepted());
    EXPECT_EQ(r.rejectReason, RejectReason::FatFingerReject);
    EXPECT_TRUE(buy(2, 1, 990'000, 500).isAccepted());
    EXPECT_GE(engine.getFatFingerRejectCount(), 1u);
}

TEST_F(RiskFixture, FatFingerRejectsPriceDeviation) {
    engine.setReferencePrice(kSym, kRef);
    engine.setFatFingerLimits(kSym, /*maxQty=*/1'000'000, /*devPct=*/0.10, /*maxNotional=*/0);

    auto tooFar = buy(1, 1, 1'200'000, 10);  // +20% > 10%
    EXPECT_FALSE(tooFar.isAccepted());
    EXPECT_EQ(tooFar.rejectReason, RejectReason::FatFingerReject);

    EXPECT_TRUE(buy(2, 1, 1'050'000, 10).isAccepted());  // +5% within band
}

TEST_F(RiskFixture, FatFingerRejectsExcessiveNotional) {
    // notional = price * qty. maxNotional between the two cases below.
    engine.setFatFingerLimits(kSym, /*maxQty=*/1'000'000, /*devPct=*/0.0,
                              /*maxNotional=*/50'000'000);

    auto big = buy(1, 1, kRef, 100);  // 1e6 * 100 = 1e8 > 5e7
    EXPECT_FALSE(big.isAccepted());
    EXPECT_EQ(big.rejectReason, RejectReason::FatFingerReject);

    EXPECT_TRUE(buy(2, 1, kRef, 10).isAccepted());  // 1e7 < 5e7
}

// ─── P2-11 OTR THROTTLE ─────────────────────────────────────────────────────

TEST_F(RiskFixture, OtrEngagesThenReleasesAfterWindow) {
    uint64_t clk = 5'000'000'000ull;          // injected virtual clock (ns)
    engine.setRiskClock([&clk] { return clk; });
    engine.setOtrLimit(/*maxRatio=*/5.0, /*windowMs=*/1000, /*minOrders=*/3);

    // No trades occur (resting buys never cross), so trades == 0 in the window.
    EXPECT_TRUE(buy(1, 1, 990'000, 1).isAccepted());   // orders=1 < minOrders
    EXPECT_TRUE(buy(2, 1, 990'000, 1).isAccepted());   // orders=2 < minOrders

    auto engaged = buy(3, 1, 990'000, 1);              // orders=3 ≥ 3, trades=0
    EXPECT_FALSE(engaged.isAccepted());
    EXPECT_EQ(engaged.rejectReason, RejectReason::OrderToTradeRatioExceeded);
    EXPECT_GE(engine.getOtrRejectCount(), 1u);

    // Still throttled inside the same window.
    EXPECT_FALSE(buy(4, 1, 990'000, 1).isAccepted());

    // Advance past the 1s window → counters roll → submissions resume.
    clk += 2'000'000'000ull;
    EXPECT_TRUE(buy(5, 1, 990'000, 1).isAccepted());
}

// ─── Baseline: controls are inert until configured ──────────────────────────

TEST_F(RiskFixture, RiskControlsInertByDefault) {
    // No controls configured: a large order that would trip every guard is fine.
    EXPECT_TRUE(buy(1, 7, kRef, 1'000'000).isAccepted());
    EXPECT_FALSE(engine.isKillSwitchActive());
    EXPECT_EQ(engine.getPosition(7), 0);
    EXPECT_EQ(engine.getFatFingerRejectCount(), 0u);
    EXPECT_EQ(engine.getOtrRejectCount(), 0u);
}
