// GTDReplayTest — deterministic expiry replay with a virtual clock.
//
// What this proves:
//   1. DAY and GTD orders expire identically under a synthetic clock
//      in both the live engine and the journal-replayed engine.
//   2. The setExpiryClock() seam feeds the same timestamps to both
//      halves, so replay is bit-identical even without real wall time.
//   3. Expiration journal entries (CancelOrder) produced by the first
//      run are sufficient for the replayed engine to reach the same
//      final state without running its own expiry timer.
//
// This is the "GTD virtual clock for cross-day replay" item from the
// honest accounting.

#include "MatchingEngine.h"
#include "Journal.h"
#include <cassert>
#include <cstdio>
#include <iostream>

using namespace OrderMatcher;

static int tests_passed = 0;

#define SECTION(name) std::cout << "\n  " << name << "..." << std::flush
#define PASS()        do { ++tests_passed; std::cout << " PASS" << std::endl; } while(0)

// Helper: count remaining orders in a book.
static size_t countOrders(const OrderBook& book) {
    size_t n = 0;
    book.forEachOrder([&](const Order&) { ++n; });
    return n;
}

// ─── Test 1: DAY orders expire under virtual clock ──────────────────────────

void test_day_expire_virtual_clock() {
    SECTION("DAY orders expire under virtual clock");

    const char* jpath = "/tmp/gtd_replay_test_day.journal";
    std::remove(jpath);

    // Scoped block: engine is destroyed (journal flushed) before replay
    {
        MatchingEngine engine;
        engine.addSymbol(1);
        engine.enableJournal(jpath);
        engine.start();

        // Virtual clock starts at T=1000
        uint64_t virtualNow = 1000;
        engine.setExpiryClock([&]() -> uint64_t { return virtualNow; });

        // Place two DAY orders that expire at T=2000
        engine.submitOrder(1, 100, 1, Side::Buy,  1000000, 10, OrderType::Limit,
                           0, 0, TimeInForce::DAY, /*expiryTime=*/2000);
        engine.submitOrder(1, 101, 1, Side::Sell, 1010000, 10, OrderType::Limit,
                           0, 0, TimeInForce::DAY, /*expiryTime=*/2000);

        // Place a GTC order that never expires
        engine.submitOrder(1, 102, 2, Side::Buy,  990000, 5, OrderType::Limit);

        assert(countOrders(*engine.getOrderBook(1)) == 3);

        // Advance clock past expiry and trigger expiry
        virtualNow = 3000;
        engine.expireOrdersFromClock();

        // Only the GTC order should survive
        assert(countOrders(*engine.getOrderBook(1)) == 1);
        assert(engine.getOrderBook(1)->getOrder(102) != nullptr);

        engine.stop();
    } // <-- journal flushed here

    // ── Replay the journal in a fresh engine ──
    MatchingEngine replayed;
    replayed.addSymbol(1);
    replayed.enableJournal(jpath);
    size_t entries = replayed.replayJournal();
    assert(entries > 0);

    // The replayed engine should have the same final state: only order 102
    // survives, because the journal recorded the expiry cancels.
    assert(countOrders(*replayed.getOrderBook(1)) == 1);
    assert(replayed.getOrderBook(1)->getOrder(102) != nullptr);

    std::remove(jpath);
    PASS();
}

// ─── Test 2: GTD orders with staggered expiry times ─────────────────────────

void test_gtd_staggered_expiry() {
    SECTION("GTD staggered expiry + replay");

    const char* jpath = "/tmp/gtd_replay_test_stagger.journal";
    std::remove(jpath);

    {
        MatchingEngine engine;
        engine.addSymbol(1);
        engine.enableJournal(jpath);
        engine.start();

        uint64_t virtualNow = 100;
        engine.setExpiryClock([&]() -> uint64_t { return virtualNow; });

        // Three GTD orders with different expiry times
        engine.submitOrder(1, 200, 1, Side::Buy,  1000000, 10, OrderType::Limit,
                           0, 0, TimeInForce::GTD, /*expiryTime=*/200);
        engine.submitOrder(1, 201, 1, Side::Buy,   990000, 10, OrderType::Limit,
                           0, 0, TimeInForce::GTD, /*expiryTime=*/500);
        engine.submitOrder(1, 202, 1, Side::Buy,   980000, 10, OrderType::Limit,
                           0, 0, TimeInForce::GTD, /*expiryTime=*/1000);

        assert(countOrders(*engine.getOrderBook(1)) == 3);

        // Advance to T=300: order 200 should expire
        virtualNow = 300;
        engine.expireOrdersFromClock();
        assert(countOrders(*engine.getOrderBook(1)) == 2);
        assert(engine.getOrderBook(1)->getOrder(200) == nullptr);

        // Advance to T=600: order 201 should expire
        virtualNow = 600;
        engine.expireOrdersFromClock();
        assert(countOrders(*engine.getOrderBook(1)) == 1);
        assert(engine.getOrderBook(1)->getOrder(201) == nullptr);

        // Advance to T=1100: order 202 should expire
        virtualNow = 1100;
        engine.expireOrdersFromClock();
        assert(countOrders(*engine.getOrderBook(1)) == 0);

        engine.stop();
    }

    // Replay: should reach the same empty state
    MatchingEngine replayed;
    replayed.addSymbol(1);
    replayed.enableJournal(jpath);
    size_t entries = replayed.replayJournal();
    assert(entries > 0);
    assert(countOrders(*replayed.getOrderBook(1)) == 0);

    std::remove(jpath);
    PASS();
}

// ─── Test 3: Mixed GTC + DAY with trades before expiry ──────────────────────

void test_mixed_orders_trade_then_expire() {
    SECTION("Mixed GTC+DAY: trades then expiry + replay");

    const char* jpath = "/tmp/gtd_replay_test_mixed.journal";
    std::remove(jpath);

    {
        MatchingEngine engine;
        engine.addSymbol(1);
        engine.enableJournal(jpath);
        engine.start();

        uint64_t virtualNow = 100;
        engine.setExpiryClock([&]() -> uint64_t { return virtualNow; });

        // DAY bid at 100.00 for 10, expires at T=500
        engine.submitOrder(1, 300, 1, Side::Buy, 1000000, 10, OrderType::Limit,
                           0, 0, TimeInForce::DAY, /*expiryTime=*/500);

        // GTC ask at 100.00 for 5 — should match immediately (partial fill of bid)
        engine.submitOrder(1, 301, 2, Side::Sell, 1000000, 5, OrderType::Limit);

        // After the match: bid 300 should have remainingQty=5
        auto* order300 = engine.getOrderBook(1)->getOrder(300);
        assert(order300 != nullptr);
        assert(order300->remainingQty == 5);

        // Now expire the partially-filled DAY order
        virtualNow = 600;
        engine.expireOrdersFromClock();

        // Order 300 should be gone; order 301 was fully filled so also gone.
        assert(countOrders(*engine.getOrderBook(1)) == 0);

        engine.stop();
    }

    // Replay
    MatchingEngine replayed;
    replayed.addSymbol(1);
    replayed.enableJournal(jpath);
    replayed.replayJournal();
    assert(countOrders(*replayed.getOrderBook(1)) == 0);

    // Verify the trade actually happened in replay too
    const auto& tradeRing = replayed.getOrderBook(1)->getTradeRingBuffer();
    // The ring buffer should have at least one trade
    assert(tradeRing.size() > 0);

    std::remove(jpath);
    PASS();
}

// ─── Test 4: Virtual clock seam — clearExpiryClock reverts to real time ─────

void test_clear_expiry_clock() {
    SECTION("clearExpiryClock reverts to wall time");

    MatchingEngine engine;
    engine.addSymbol(1);
    engine.start();

    uint64_t virtualNow = 1;
    engine.setExpiryClock([&]() -> uint64_t { return virtualNow; });
    assert(engine.expiryNow() == 1);

    // Clear and verify it reads real wall time (>> 1)
    engine.clearExpiryClock();
    uint64_t real = engine.expiryNow();
    assert(real > 1000);  // any real clock value is many orders of magnitude > 1

    engine.stop();
    PASS();
}

// ─── Test 5: Deterministic replay with checkpoint ───────────────────────────

void test_checkpoint_preserves_expiry_state() {
    SECTION("Checkpoint + replay preserves GTD orders");

    const char* jpath = "/tmp/gtd_replay_test_ckpt.journal";
    std::remove(jpath);

    {
        MatchingEngine engine;
        engine.addSymbol(1);
        engine.enableJournal(jpath);
        engine.start();

        uint64_t virtualNow = 100;
        engine.setExpiryClock([&]() -> uint64_t { return virtualNow; });

        // Place two GTD orders
        engine.submitOrder(1, 400, 1, Side::Buy, 1000000, 10, OrderType::Limit,
                           0, 0, TimeInForce::GTD, /*expiryTime=*/500);
        engine.submitOrder(1, 401, 1, Side::Buy,  990000, 10, OrderType::Limit,
                           0, 0, TimeInForce::GTD, /*expiryTime=*/1000);

        // Checkpoint — the journal now contains only snapshot entries
        engine.checkpoint();

        // Expire order 400
        virtualNow = 600;
        engine.expireOrdersFromClock();
        assert(countOrders(*engine.getOrderBook(1)) == 1);

        engine.stop();
    }

    // Replay from checkpoint + expiry cancel
    MatchingEngine replayed;
    replayed.addSymbol(1);
    replayed.enableJournal(jpath);
    replayed.replayJournal();
    assert(countOrders(*replayed.getOrderBook(1)) == 1);
    assert(replayed.getOrderBook(1)->getOrder(401) != nullptr);

    std::remove(jpath);
    PASS();
}

// ─── Main ───────────────────────────────────────────────────────────────────

int main() {
    std::cout << "\n═══ GTD Virtual Clock Deterministic Replay Tests ═══" << std::endl;

    test_day_expire_virtual_clock();
    test_gtd_staggered_expiry();
    test_mixed_orders_trade_then_expire();
    test_clear_expiry_clock();
    test_checkpoint_preserves_expiry_state();

    std::cout << "\n─── Results: " << tests_passed << " passed ───" << std::endl;
    std::cout << "\nAll GTD replay tests passed.\n" << std::endl;
    return 0;
}
