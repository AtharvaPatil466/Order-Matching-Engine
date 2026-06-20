// TestPaperTrader.cpp — GoogleTest suite for include/PaperTrader.h.
//
// Tests:
//  1. Neutral ticks: position stays 0, tradeCount == 0.
//  2. Strong buy signal: onTick returns a value > buyThreshold.
//  3. unrealizedPnl(mid) returns 0.0 when position == 0.
//  4. totalPnl == realizedPnl + unrealizedPnl.
//  5. reset() zeroes all state.
//  6. Position cap: |position| never exceeds maxPosition.

#include "PaperTrader.h"
#include "MatchingEngine.h"
#include "OrderBook.h"
#include "Types.h"
#include "SignalGenerator.h"

#include <gtest/gtest.h>
#include <cmath>
#include <memory>

using namespace OrderMatcher;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static constexpr SymbolId SYM = 0;

// Build a synchronous engine with symbol SYM seeded with resting orders
// so IOC orders can actually fill.
static std::unique_ptr<MatchingEngine> makeEngine() {
    auto eng = std::make_unique<MatchingEngine>();
    eng->addSymbol(SYM);
    eng->start();

    OrderBook* bk = eng->getOrderBook(SYM);
    if (bk) {
        bk->setCircuitBreakerThreshold(1e18);  // disable circuit breaker
    }

    // Seed the book: resting bids and asks around 100.00
    // Ask side: 10 levels at 100.01 - 100.10 (in fixed-point: 1000100 - 1001000)
    for (int i = 0; i < 10; ++i) {
        Price askPx = toPrice(100.01 + i * 0.01);
        eng->processOrder(SYM, static_cast<OrderId>(100 + i), /*pid=*/1,
                          Side::Sell, askPx, 500, OrderType::Limit);
    }
    // Bid side: 10 levels at 99.99 - 99.90
    for (int i = 0; i < 10; ++i) {
        Price bidPx = toPrice(99.99 - i * 0.01);
        eng->processOrder(SYM, static_cast<OrderId>(200 + i), /*pid=*/2,
                          Side::Buy, bidPx, 500, OrderType::Limit);
    }

    return eng;
}

// Build a neutral snapshot: all signals near zero, no momentum.
static MicrostructureSnapshot neutralSnap() {
    MicrostructureSnapshot s{};
    s.orderFlowImbalance = 0.0;
    s.queueImbalance     = 0.0;
    s.vpin               = 0.5;   // high toxicity dampens signal
    s.rollSpread         = 0.01;
    s.kylesLambda        = 0.0;
    s.midPrice           = 100.0;
    s.prevMidPrice       = 100.0;
    s.arrivalRate        = 0.0;
    s.timestampNs        = 0;
    return s;
}

// Build a strong buy snapshot.
//   orderFlowImbalance = 0.9, queueImbalance = 0.8, vpin = 0.1 (low toxicity)
//   midPrice > prevMidPrice (upward momentum), rollSpread = 0.02
//   kylesLambda = 0 (no adverse selection counter-signal)
static MicrostructureSnapshot strongBuySnap() {
    MicrostructureSnapshot s{};
    s.orderFlowImbalance = 0.9;
    s.queueImbalance     = 0.8;
    s.vpin               = 0.1;
    s.rollSpread         = 0.02;
    s.kylesLambda        = 0.0;
    s.midPrice           = 100.0;
    s.prevMidPrice       = 99.9;
    s.arrivalRate        = 0.0;
    s.timestampNs        = 0;
    return s;
}

// ---------------------------------------------------------------------------
// Test 1: Neutral ticks — position stays 0, tradeCount == 0
// ---------------------------------------------------------------------------
TEST(PaperTraderTest, NeutralTicksNoTrades) {
    auto eng = makeEngine();
    // Use a very low maxPosition so cap test is independent
    PaperTrader pt(*eng, SYM, /*pid=*/9999,
                   /*buyThreshold=*/0.20, /*sellThreshold=*/0.20,
                   /*orderSize=*/100, /*maxPosition=*/1000);

    auto snap = neutralSnap();
    for (int i = 0; i < 20; ++i) {
        pt.onTick(snap);
    }

    EXPECT_EQ(pt.position(), 0);
    auto s = pt.stats(100.0);
    EXPECT_EQ(s.tradeCount, 0u);
    EXPECT_EQ(s.tickCount, 20u);
}

// ---------------------------------------------------------------------------
// Test 2: Strong buy signal — onTick returns value > buyThreshold
// ---------------------------------------------------------------------------
TEST(PaperTraderTest, StrongBuySignalExceedsThreshold) {
    auto eng = makeEngine();
    PaperTrader pt(*eng, SYM, /*pid=*/9999,
                   /*buyThreshold=*/0.20, /*sellThreshold=*/0.20,
                   /*orderSize=*/100, /*maxPosition=*/1000);

    auto snap = strongBuySnap();
    double sigVal = pt.onTick(snap);

    EXPECT_GT(sigVal, 0.20);
}

// ---------------------------------------------------------------------------
// Test 3: unrealizedPnl is 0 when position == 0
// ---------------------------------------------------------------------------
TEST(PaperTraderTest, UnrealizedPnlZeroWhenFlat) {
    auto eng = makeEngine();
    PaperTrader pt(*eng, SYM);

    // Feed neutral ticks — no fills expected
    auto snap = neutralSnap();
    for (int i = 0; i < 5; ++i) pt.onTick(snap);

    EXPECT_EQ(pt.position(), 0);
    EXPECT_DOUBLE_EQ(pt.unrealizedPnl(100.0), 0.0);
    EXPECT_DOUBLE_EQ(pt.unrealizedPnl(50.0),  0.0);
    EXPECT_DOUBLE_EQ(pt.unrealizedPnl(200.0), 0.0);
}

// ---------------------------------------------------------------------------
// Test 4: totalPnl == realizedPnl + unrealizedPnl
// ---------------------------------------------------------------------------
TEST(PaperTraderTest, TotalPnlIdentity) {
    auto eng = makeEngine();
    PaperTrader pt(*eng, SYM, /*pid=*/9999,
                   /*buyThreshold=*/0.20, /*sellThreshold=*/0.20,
                   /*orderSize=*/100, /*maxPosition=*/1000);

    // Process some ticks (mix of neutral and strong buy)
    pt.onTick(neutralSnap());
    pt.onTick(strongBuySnap());
    pt.onTick(neutralSnap());

    double mid = 100.05;
    double realized   = pt.realizedPnl();
    double unrealized = pt.unrealizedPnl(mid);
    double total      = pt.totalPnl(mid);

    EXPECT_NEAR(total, realized + unrealized, 1e-9);

    // Also check via stats struct
    auto s = pt.stats(mid);
    EXPECT_NEAR(s.totalPnl, s.realizedPnl + s.unrealizedPnl, 1e-9);
}

// ---------------------------------------------------------------------------
// Test 5: reset() zeroes all state
// ---------------------------------------------------------------------------
TEST(PaperTraderTest, ResetClearsAllState) {
    auto eng = makeEngine();
    PaperTrader pt(*eng, SYM, /*pid=*/9999,
                   /*buyThreshold=*/0.20, /*sellThreshold=*/0.20,
                   /*orderSize=*/100, /*maxPosition=*/1000);

    // Generate some ticks first
    pt.onTick(neutralSnap());
    pt.onTick(strongBuySnap());
    pt.onTick(neutralSnap());

    pt.reset();

    EXPECT_EQ(pt.position(), 0);
    EXPECT_DOUBLE_EQ(pt.realizedPnl(), 0.0);
    EXPECT_DOUBLE_EQ(pt.unrealizedPnl(100.0), 0.0);
    EXPECT_DOUBLE_EQ(pt.totalPnl(100.0), 0.0);

    auto s = pt.stats(100.0);
    EXPECT_EQ(s.tradeCount, 0u);
    EXPECT_EQ(s.tickCount, 0u);
    EXPECT_DOUBLE_EQ(s.lastSignal, 0.0);
    EXPECT_DOUBLE_EQ(s.realizedPnl, 0.0);
    EXPECT_DOUBLE_EQ(s.unrealizedPnl, 0.0);
}

// ---------------------------------------------------------------------------
// Test 6: Position cap — no new orders are submitted once |position| >= maxPosition
// ---------------------------------------------------------------------------
TEST(PaperTraderTest, PositionCapRespected) {
    auto eng = makeEngine();

    // orderSize (50) == maxPosition (50): after the first fill the cap is reached
    // and no further buys should be issued, so position stays <= 50.
    PaperTrader pt(*eng, SYM, /*pid=*/9999,
                   /*buyThreshold=*/0.001,  // very low threshold → fires on almost any buy signal
                   /*sellThreshold=*/0.001,
                   /*orderSize=*/50,        // each order ≤ maxPosition
                   /*maxPosition=*/50);     // cap at 50 shares

    auto snap = strongBuySnap();
    for (int i = 0; i < 30; ++i) {
        pt.onTick(snap);
    }

    // After the first fill position == 50; subsequent ticks are blocked by the cap.
    EXPECT_LE(std::abs(pt.position()), static_cast<int64_t>(50));
    // Verify the cap actually did something: tradeCount should be small (≤ 1 buy).
    EXPECT_LE(pt.stats(100.0).tradeCount, 2u);  // allow for one fill (buy)
}
