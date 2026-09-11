// EngineLifecycleTest — audit H11, second pass: engine-level surfaces that no
// test executed.
//
// Measured with llvm-cov across all 516 tests, each of these was at 0.00%:
//
//     MatchingEngine::startExpiryTimer / stopExpiryTimer
//     MatchingEngine::resumeVolatilityAuctions
//     MatchingEngine::setReplayModeAllBooks
//     MatchingEngine::setRiskLimits
//
// Not obscure corners. The expiry timer is the only thing that retires a GTD
// order once its time passes — untested, an engine can keep matching orders
// whose instructions expired. resumeVolatilityAuctions is how a halted symbol
// returns to trading. setReplayModeAllBooks is what a backup toggles at
// promotion.
//
// GTDReplayTest covers journal REPLAY of expiries and GracefulShutdownTest
// covers drain-time GTD handling; neither starts the background timer thread.

#include "MatchingEngine.h"
#include "OrderBook.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

using namespace OrderMatcher;

namespace {

int passed = 0;
#define TEST(name) std::cout << "  " << #name << "..." << std::flush
#define PASS() do { std::cout << " ok" << std::endl; ++passed; } while (0)

constexpr SymbolId kSym = 3;
constexpr Price    kPx  = 1'000'000;

// Wait for a condition rather than sleeping a fixed time. A fixed sleep in a
// test driving a background thread is a throughput assumption, and those are
// what made other tests in this repo flaky on loaded machines.
template <typename Fn>
bool waitFor(Fn&& cond, std::chrono::milliseconds limit = std::chrono::milliseconds(5000)) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

// ─── 1: the expiry timer actually retires a GTD order ───────────────────────
//
// The timer thread is the only thing that expires orders on a live engine. An
// injected clock keeps this deterministic: the test moves time, not the wall.
void test_ExpiryTimerRetiresGtdOrders() {
    TEST(ExpiryTimerRetiresGtdOrders);
    MatchingEngine engine;
    engine.addSymbol(kSym);

    std::atomic<uint64_t> now{1'000};
    engine.setExpiryClock([&now] { return now.load(std::memory_order_relaxed); });
    engine.start();

    // A GTD expiring at 5000, and a GTC that must survive — a sweep that
    // cancelled everything would otherwise look like a pass.
    engine.submitOrder(kSym, 1, 100, Side::Buy, kPx - 1000, 50, OrderType::Limit,
                       /*stopPrice=*/0, /*displayQty=*/0, TimeInForce::GTD,
                       /*expiryTime=*/5'000);
    engine.submitOrder(kSym, 2, 100, Side::Buy, kPx - 2000, 50, OrderType::Limit);

    auto* book = engine.getOrderBook(kSym);
    assert(book->getOrder(1) && book->getOrder(2));

    engine.startExpiryTimer(/*intervalMs=*/5);

    // Before its time it must stay. Give the timer several ticks to get this
    // wrong in, rather than asserting after the first one.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(book->getOrder(1) && "a GTD order must not expire before its time");

    now.store(5'001, std::memory_order_relaxed);
    assert(waitFor([&] { return book->getOrder(1) == nullptr; }) &&
           "the expiry timer must retire a GTD order once its time passes");
    assert(book->getOrder(2) && "a GTC order must survive the sweep");

    engine.stopExpiryTimer();
    engine.stop();
    PASS();
}

// ─── 2: starting the timer twice does not start a second thread ─────────────
//
// startExpiryTimer returns early when already running. If that guard broke,
// two threads would sweep concurrently and the second would never be joined by
// stopExpiryTimer — so the process would not exit.
void test_ExpiryTimerStartIsIdempotent() {
    TEST(ExpiryTimerStartIsIdempotent);
    MatchingEngine engine;
    engine.addSymbol(kSym);
    std::atomic<uint64_t> now{1'000};
    engine.setExpiryClock([&now] { return now.load(std::memory_order_relaxed); });
    engine.start();

    engine.startExpiryTimer(5);
    engine.startExpiryTimer(5);   // must be a no-op, not a second thread
    engine.startExpiryTimer(5);

    engine.submitOrder(kSym, 1, 100, Side::Buy, kPx, 10, OrderType::Limit,
                       0, 0, TimeInForce::GTD, /*expiryTime=*/2'000);
    now.store(2'001, std::memory_order_relaxed);
    assert(waitFor([&] { return engine.getOrderBook(kSym)->getOrder(1) == nullptr; }));

    // The real assertion: this returns. A leaked second thread would hang it.
    engine.stopExpiryTimer();
    engine.stop();
    PASS();
}

// ─── 3: stopping a timer that was never started is safe ─────────────────────
void test_StopExpiryTimerWithoutStartIsSafe() {
    TEST(StopExpiryTimerWithoutStartIsSafe);
    MatchingEngine engine;
    engine.addSymbol(kSym);
    engine.start();
    engine.stopExpiryTimer();   // must not join an unstarted thread
    engine.stopExpiryTimer();   // and must stay safe when repeated
    engine.stop();
    PASS();
}

// ─── 4: resuming volatility auctions returns halted symbols to trading ──────
//
// How a halted symbol comes back. Untested, a venue could halt and have no
// verified way out.
void test_ResumeVolatilityAuctionsReturnsSymbolsToTrading() {
    TEST(ResumeVolatilityAuctionsReturnsSymbolsToTrading);
    MatchingEngine engine;
    engine.addSymbol(kSym);
    engine.addSymbol(kSym + 1);
    engine.start();

    // Nothing halted: resume must report zero rather than claiming work.
    assert(engine.resumeVolatilityAuctions() == 0 &&
           "with nothing halted, resume must report no symbols resumed");

    auto* book = engine.getOrderBook(kSym);
    book->setTradingState(TradingState::VolatilityAuction);
    assert(book->getTradingState() == TradingState::VolatilityAuction);

    const size_t resumed = engine.resumeVolatilityAuctions();
    assert(resumed == 1 && "exactly the halted symbol must resume");
    assert(book->getTradingState() != TradingState::VolatilityAuction &&
           "the symbol must have left the auction state");

    // The untouched symbol must be unaffected.
    assert(engine.getOrderBook(kSym + 1)->getTradingState() != TradingState::VolatilityAuction);

    engine.stop();
    PASS();
}

// ─── 5: replay mode toggles every book ──────────────────────────────────────
//
// A backup runs in replay mode so local order updates and market data are
// suppressed — the primary is canonical — and leaves it at promotion. Getting
// this wrong makes a backup emit market data as if it were live.
void test_ReplayModeAppliesToAllBooks() {
    TEST(ReplayModeAppliesToAllBooks);
    MatchingEngine engine;
    engine.addSymbol(kSym);
    engine.addSymbol(kSym + 1);

    engine.setReplayModeAllBooks(true);
    assert(engine.getOrderBook(kSym)->isReplayMode());
    assert(engine.getOrderBook(kSym + 1)->isReplayMode());

    engine.setReplayModeAllBooks(false);
    assert(!engine.getOrderBook(kSym)->isReplayMode());
    assert(!engine.getOrderBook(kSym + 1)->isReplayMode());
    PASS();
}

// ─── 6: risk limits set through the engine reach the book ───────────────────
//
// setRiskLimits is how an operator installs per-participant limits on a running
// engine. If it does not reach the book the limits silently do not exist, and
// the failure mode is an oversized order that should have been refused.
void test_RiskLimitsSetThroughEngineAreEnforced() {
    TEST(RiskLimitsSetThroughEngineAreEnforced);
    MatchingEngine engine;
    engine.addSymbol(kSym);
    engine.start();

    RiskLimits limits;
    limits.maxOrderSize = 100;
    engine.setRiskLimits(kSym, /*participantId=*/42, limits);

    auto ok = engine.submitOrder(kSym, 1, 42, Side::Buy, kPx - 1000, 100, OrderType::Limit);
    assert(ok.isAccepted() && "an order at the cap must pass");

    auto tooBig = engine.submitOrder(kSym, 2, 42, Side::Buy, kPx - 1000, 101, OrderType::Limit);
    assert(!tooBig.isAccepted() && "an order over the cap must be refused");

    // And the cap must be scoped to its participant, not global.
    auto other = engine.submitOrder(kSym, 3, 43, Side::Buy, kPx - 1000, 500, OrderType::Limit);
    assert(other.isAccepted() && "another participant must not inherit the cap");

    engine.stop();
    PASS();
}

}  // namespace

int main() {
    std::cout << "\n=== Engine Lifecycle Tests (H11) ===\n\n";

    test_ExpiryTimerRetiresGtdOrders();
    test_ExpiryTimerStartIsIdempotent();
    test_StopExpiryTimerWithoutStartIsSafe();
    test_ResumeVolatilityAuctionsReturnsSymbolsToTrading();
    test_ReplayModeAppliesToAllBooks();
    test_RiskLimitsSetThroughEngineAreEnforced();

    std::cout << "\n" << passed << " passed\n\n";
    return 0;
}
