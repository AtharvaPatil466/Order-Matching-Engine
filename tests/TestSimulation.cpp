// TestSimulation.cpp
// GoogleTest suite for the agent-based market simulation layer.
//
// Tests:
//   1. NoiseTrader produces orders over 100 ticks (nonzero history volume)
//   2. MarketMaker posts quotes every tick (bestBid and bestAsk exist after tick 1)
//   3. InformedTrader moves price when signal is set far from mid
//   4. SimulationDriver history length == numTicks after run()
//   5. Two participants (NoiseTrader + MarketMaker) coexist without crashing over 500 ticks

#include "MatchingEngine.h"
#include "OrderBook.h"
#include "Types.h"
#include "SimulationDriver.h"
#include "NoiseTrader.h"
#include "MarketMaker.h"
#include "InformedTrader.h"

#include <gtest/gtest.h>
#include <memory>
#include <cstdint>

using namespace OrderMatcher;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static constexpr SymbolId SYM = 1;

// Build a running engine with a single symbol and no circuit breakers.
// Returns by unique_ptr because MatchingEngine is not copyable.
// IMPORTANT: addSymbol must be called BEFORE start() — start() freezes the
// book registry (booksFrozen_ = true) which causes addSymbol to be a no-op.
static std::unique_ptr<MatchingEngine> makeEngine() {
    auto eng = std::make_unique<MatchingEngine>();
    eng->addSymbol(SYM);   // must come before start()
    eng->start();
    OrderBook* book = eng->getOrderBook(SYM);
    if (book) {
        book->setCircuitBreakerThreshold(1e18);  // disable circuit breaker
    }
    return eng;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: NoiseTrader produces orders over 100 ticks
// ─────────────────────────────────────────────────────────────────────────────
TEST(SimulationTest, NoiseTraderProducesOrders) {
    auto engPtr = makeEngine();
    MatchingEngine& eng = *engPtr;

    NoiseTrader::Config cfg;
    cfg.lambdaPerTick  = 1.0;    // submit every tick
    cfg.cancelProb     = 0.0;    // no cancels so orders accumulate
    cfg.maxSpreadTicks = 5;
    cfg.minQty         = 10;
    cfg.maxQty         = 10;
    cfg.fallbackMid    = toPrice(100.0);

    SimulationDriver driver(eng, SYM);
    driver.addParticipant(std::make_unique<NoiseTrader>(1, cfg, /*seed=*/42));
    driver.run(100);

    const auto& hist = driver.history();
    ASSERT_EQ(hist.size(), 100u);

    // After 100 ticks with lambda=1 there should be resting bids or asks
    bool anyQuotes = false;
    for (const auto& snap : hist) {
        if (snap.bestBid > 0 || snap.bestAsk > 0) {
            anyQuotes = true;
            break;
        }
    }
    EXPECT_TRUE(anyQuotes) << "NoiseTrader should have placed at least one resting order";
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: MarketMaker posts quotes every tick
// ─────────────────────────────────────────────────────────────────────────────
TEST(SimulationTest, MarketMakerPostsQuotes) {
    auto engPtr = makeEngine();
    MatchingEngine& eng = *engPtr;

    MarketMaker::Config cfg;
    cfg.gamma         = 0.1;
    cfg.sigma         = 0.01;
    cfg.T             = 1.0;
    cfg.kappa         = 1.5;
    cfg.quoteQty      = 50;
    cfg.fallbackMid   = toPrice(100.0);
    cfg.minHalfSpread = 1;

    SimulationDriver driver(eng, SYM);
    driver.addParticipant(std::make_unique<MarketMaker>(10, cfg, /*seed=*/42));
    driver.run(10);

    const auto& hist = driver.history();
    ASSERT_EQ(hist.size(), 10u);

    // After tick 1 the market maker should have resting quotes on both sides
    bool bidPresent = false, askPresent = false;
    for (size_t i = 1; i < hist.size(); ++i) {
        if (hist[i].bestBid > 0) bidPresent = true;
        if (hist[i].bestAsk > 0) askPresent = true;
    }
    EXPECT_TRUE(bidPresent)  << "MarketMaker should post a bid every tick";
    EXPECT_TRUE(askPresent)  << "MarketMaker should post an ask every tick";
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3: InformedTrader moves price when signal is set far from mid
// ─────────────────────────────────────────────────────────────────────────────
TEST(SimulationTest, InformedTraderMovesPrice) {
    auto engPtr = makeEngine();
    MatchingEngine& eng = *engPtr;

    // Seed the book with a market maker to provide initial liquidity
    MarketMaker::Config mmCfg;
    mmCfg.fallbackMid   = toPrice(100.0);
    mmCfg.quoteQty      = 200;
    mmCfg.minHalfSpread = 1;

    InformedTrader::Config itCfg;
    itCfg.signalThreshold = 1;
    itCfg.maxPosition     = 10000;
    itCfg.orderQty        = 50;

    // Build the InformedTrader first so we retain a raw pointer for signal updates
    auto itOwned = std::make_unique<InformedTrader>(20, itCfg, 99);
    InformedTrader* it = itOwned.get();

    SimulationDriver driver(eng, SYM);
    driver.addParticipant(std::make_unique<MarketMaker>(10, mmCfg, 42));
    driver.addParticipant(std::move(itOwned));

    // Run 5 ticks to establish initial quotes from the market maker
    driver.run(5);

    // Set a very aggressive buy signal — well above the ask
    Price highSignal = toPrice(200.0);
    it->setSignal(highSignal);

    driver.run(10);

    const auto& hist = driver.history();
    // After the informed trader fires aggressive buys, the last trade should
    // have occurred (lastTradePrice > 0)
    bool traded = false;
    for (const auto& snap : hist) {
        if (snap.lastTradePrice > 0) {
            traded = true;
            break;
        }
    }
    EXPECT_TRUE(traded) << "InformedTrader should have crossed the spread and executed";
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4: SimulationDriver history length == numTicks after run()
// ─────────────────────────────────────────────────────────────────────────────
TEST(SimulationTest, HistoryLengthEqualsNumTicks) {
    auto engPtr = makeEngine();
    MatchingEngine& eng = *engPtr;
    SimulationDriver driver(eng, SYM);
    driver.addParticipant(std::make_unique<NoiseTrader>(1, NoiseTrader::Config{}, 42));

    constexpr uint64_t N = 77;
    driver.run(N);
    EXPECT_EQ(driver.history().size(), N);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5: Two participants coexist without crashing over 500 ticks
// ─────────────────────────────────────────────────────────────────────────────
TEST(SimulationTest, NoiseTraderAndMarketMakerCoexist) {
    auto engPtr = makeEngine();
    MatchingEngine& eng = *engPtr;

    NoiseTrader::Config ntCfg;
    ntCfg.lambdaPerTick  = 0.5;
    ntCfg.cancelProb     = 0.1;
    ntCfg.maxSpreadTicks = 8;
    ntCfg.minQty         = 1;
    ntCfg.maxQty         = 50;
    ntCfg.fallbackMid    = toPrice(100.0);

    MarketMaker::Config mmCfg;
    mmCfg.gamma         = 0.1;
    mmCfg.sigma         = 0.02;
    mmCfg.T             = 1.0;
    mmCfg.kappa         = 1.5;
    mmCfg.quoteQty      = 30;
    mmCfg.fallbackMid   = toPrice(100.0);
    mmCfg.minHalfSpread = 1;
    mmCfg.maxInventory  = 300;

    SimulationDriver driver(eng, SYM);
    driver.addParticipant(std::make_unique<NoiseTrader>( 1, ntCfg, /*seed=*/1));
    driver.addParticipant(std::make_unique<MarketMaker>(10, mmCfg, /*seed=*/2));

    // Must not throw or crash
    ASSERT_NO_THROW(driver.run(500));
    EXPECT_EQ(driver.history().size(), 500u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6: clearHistory resets the history vector
// ─────────────────────────────────────────────────────────────────────────────
TEST(SimulationTest, ClearHistoryWorks) {
    auto engPtr = makeEngine();
    MatchingEngine& eng = *engPtr;
    SimulationDriver driver(eng, SYM);
    driver.addParticipant(std::make_unique<NoiseTrader>(1, NoiseTrader::Config{}, 42));
    driver.run(20);
    EXPECT_EQ(driver.history().size(), 20u);
    driver.clearHistory();
    EXPECT_EQ(driver.history().size(), 0u);
}
