// GracefulShutdownTest — P3-6 graceful shutdown with state persistence.
//
// Explicit semantics under test (see MatchingEngine::gracefulShutdown):
//   * GTD resting orders PERSIST and are restored on restart.
//   * GTC resting orders persist too (they outlive a session by definition).
//   * DAY orders are CANCELLED at session end and are NOT restored.
//   * An in-flight IOC that cannot fully match cancels its remainder — it is
//     never left resting, so it is not persisted.
//   * An in-flight partial fill completes: the resting remainder of a GTD is
//     persisted with the correct remaining quantity; the remainder of a DAY is
//     cancelled.
//
// Restart is modelled the same way the GTD replay test does it: destroy the
// engine (flushing the journal), then build a fresh engine and replayJournal().

#include "MatchingEngine.h"
#include "Journal.h"

#include <cassert>
#include <cstdio>
#include <iostream>

using namespace OrderMatcher;

static int tests_passed = 0;

#define SECTION(name) std::cout << "\n  " << name << "..." << std::flush
#define PASS()        do { ++tests_passed; std::cout << " PASS" << std::endl; } while (0)

static size_t countOrders(const OrderBook& book) {
    size_t n = 0;
    book.forEachOrder([&](const Order&) { ++n; });
    return n;
}

// ─── Test 1: GTD + GTC persist, DAY cancelled (sync mode) ───────────────────

void test_gtd_persists_day_cancelled() {
    SECTION("GTD+GTC persist, DAY cancelled + restored across restart");

    const char* jpath = "/tmp/graceful_shutdown_basic.journal";
    std::remove(jpath);

    MatchingEngine::ShutdownReport report{};
    {
        MatchingEngine engine;
        engine.addSymbol(1);
        engine.enableJournal(jpath);
        engine.start();

        // Three non-crossing resting buys (no asks → nothing matches).
        engine.submitOrder(1, 100, 1, Side::Buy, 990000, 10, OrderType::Limit,
                           0, 0, TimeInForce::GTD, /*expiryTime=*/1'000'000);
        engine.submitOrder(1, 101, 1, Side::Buy, 980000, 10, OrderType::Limit,
                           0, 0, TimeInForce::DAY);
        engine.submitOrder(1, 102, 2, Side::Buy, 970000, 10, OrderType::Limit,
                           0, 0, TimeInForce::GTC);

        assert(countOrders(*engine.getOrderBook(1)) == 3);

        report = engine.gracefulShutdown();

        // DAY gone from the live book; GTD + GTC still resting.
        assert(engine.getOrderBook(1)->getOrder(101) == nullptr);
        assert(engine.getOrderBook(1)->getOrder(100) != nullptr);
        assert(engine.getOrderBook(1)->getOrder(102) != nullptr);
        assert(countOrders(*engine.getOrderBook(1)) == 2);

        engine.stop();
    } // journal flushed on destruction

    assert(report.dayOrdersCancelled == 1);
    assert(report.gtdOrdersPersisted == 1);
    assert(report.otherOrdersPersisted == 1);   // the GTC
    assert(report.ordersPersisted == 2);
    assert(report.journalEnabled);

    // ── Restart: replay the journal in a fresh engine ──
    MatchingEngine restarted;
    restarted.addSymbol(1);
    restarted.enableJournal(jpath);
    size_t entries = restarted.replayJournal();
    assert(entries > 0);

    // GTD (100) and GTC (102) restored; DAY (101) is NOT.
    assert(restarted.getOrderBook(1)->getOrder(100) != nullptr);
    assert(restarted.getOrderBook(1)->getOrder(102) != nullptr);
    assert(restarted.getOrderBook(1)->getOrder(101) == nullptr);
    assert(countOrders(*restarted.getOrderBook(1)) == 2);

    std::remove(jpath);
    PASS();
}

// ─── Test 2: partial-fill GTD remainder persists with correct qty ───────────

void test_partial_fill_gtd_remainder_persists() {
    SECTION("Partially-filled GTD persists remainder across restart");

    const char* jpath = "/tmp/graceful_shutdown_partial.journal";
    std::remove(jpath);

    {
        MatchingEngine engine;
        engine.addSymbol(1);
        engine.enableJournal(jpath);
        engine.start();

        // GTD buy 10 @ 100.0000; GTC sell 4 crosses → GTD left resting with 6.
        engine.submitOrder(1, 200, 1, Side::Buy, 1000000, 10, OrderType::Limit,
                           0, 0, TimeInForce::GTD, /*expiryTime=*/1'000'000);
        engine.submitOrder(1, 201, 2, Side::Sell, 1000000, 4, OrderType::Limit);

        auto* o200 = engine.getOrderBook(1)->getOrder(200);
        assert(o200 != nullptr && o200->remainingQty == 6);
        assert(engine.getOrderBook(1)->getOrder(201) == nullptr); // fully filled

        auto report = engine.gracefulShutdown();
        assert(report.gtdOrdersPersisted == 1);
        assert(report.dayOrdersCancelled == 0);

        engine.stop();
    }

    MatchingEngine restarted;
    restarted.addSymbol(1);
    restarted.enableJournal(jpath);
    restarted.replayJournal();

    auto* o200 = restarted.getOrderBook(1)->getOrder(200);
    assert(o200 != nullptr && o200->remainingQty == 6);
    assert(countOrders(*restarted.getOrderBook(1)) == 1);

    std::remove(jpath);
    PASS();
}

// ─── Test 3: partially-filled DAY is cancelled at session end ───────────────

void test_partial_fill_day_cancelled() {
    SECTION("Partially-filled DAY cancelled at shutdown, not restored");

    const char* jpath = "/tmp/graceful_shutdown_day_partial.journal";
    std::remove(jpath);

    {
        MatchingEngine engine;
        engine.addSymbol(1);
        engine.enableJournal(jpath);
        engine.start();

        engine.submitOrder(1, 300, 1, Side::Buy, 1000000, 10, OrderType::Limit,
                           0, 0, TimeInForce::DAY);
        engine.submitOrder(1, 301, 2, Side::Sell, 1000000, 3, OrderType::Limit);

        auto* o300 = engine.getOrderBook(1)->getOrder(300);
        assert(o300 != nullptr && o300->remainingQty == 7);

        auto report = engine.gracefulShutdown();
        assert(report.dayOrdersCancelled == 1);
        assert(report.ordersPersisted == 0);
        assert(engine.getOrderBook(1)->getOrder(300) == nullptr);

        engine.stop();
    }

    MatchingEngine restarted;
    restarted.addSymbol(1);
    restarted.enableJournal(jpath);
    restarted.replayJournal();
    assert(restarted.getOrderBook(1)->getOrder(300) == nullptr);
    assert(countOrders(*restarted.getOrderBook(1)) == 0);

    std::remove(jpath);
    PASS();
}

// ─── Test 4: async drain — IOC remainder cancelled, GTD persists ────────────

void test_async_drain_ioc_and_gtd() {
    SECTION("Async: drain in-flight, IOC not persisted, GTD restored");

    const char* jpath = "/tmp/graceful_shutdown_async.journal";
    std::remove(jpath);

    {
        MatchingEngine engine;
        engine.addSymbol(1);
        engine.enableJournal(jpath);
        engine.startAsync(2, 4096);

        // GTD + DAY resting buys, plus an IOC buy with no asks (unmatchable →
        // its remainder is cancelled during matching; it never rests).
        engine.submitOrder(1, 400, 1, Side::Buy, 990000, 10, OrderType::Limit,
                           0, 0, TimeInForce::GTD, /*expiryTime=*/1'000'000);
        engine.submitOrder(1, 401, 1, Side::Buy, 980000, 10, OrderType::Limit,
                           0, 0, TimeInForce::DAY);
        engine.submitOrder(1, 402, 2, Side::Buy, 970000, 5, OrderType::IOC);

        auto report = engine.gracefulShutdown();  // drains internally

        assert(report.dayOrdersCancelled == 1);
        assert(report.gtdOrdersPersisted == 1);
        assert(engine.getOrderBook(1)->getOrder(401) == nullptr);  // DAY
        assert(engine.getOrderBook(1)->getOrder(402) == nullptr);  // IOC
        assert(engine.getOrderBook(1)->getOrder(400) != nullptr);  // GTD

        // New submissions are refused once shutting down.
        auto refused = engine.submitOrder(1, 999, 1, Side::Buy, 960000, 1,
                                          OrderType::Limit);
        assert(!refused.isAccepted());
        assert(engine.isShuttingDown());

        engine.stopAsync();
    }

    MatchingEngine restarted;
    restarted.addSymbol(1);
    restarted.enableJournal(jpath);
    restarted.replayJournal();
    assert(restarted.getOrderBook(1)->getOrder(400) != nullptr);
    assert(restarted.getOrderBook(1)->getOrder(401) == nullptr);
    assert(restarted.getOrderBook(1)->getOrder(402) == nullptr);
    assert(countOrders(*restarted.getOrderBook(1)) == 1);

    std::remove(jpath);
    PASS();
}

// ─── Test 5: shutdown with no journal is safe (nothing durable) ─────────────

void test_no_journal_safe() {
    SECTION("Shutdown without a journal cancels DAY, reports non-durable");

    MatchingEngine engine;
    engine.addSymbol(1);
    engine.start();

    engine.submitOrder(1, 500, 1, Side::Buy, 990000, 10, OrderType::Limit,
                       0, 0, TimeInForce::GTD, /*expiryTime=*/1'000'000);
    engine.submitOrder(1, 501, 1, Side::Buy, 980000, 10, OrderType::Limit,
                       0, 0, TimeInForce::DAY);

    auto report = engine.gracefulShutdown();
    assert(report.dayOrdersCancelled == 1);
    assert(report.gtdOrdersPersisted == 1);
    assert(!report.journalEnabled);  // nothing durable to restore from
    assert(engine.getOrderBook(1)->getOrder(501) == nullptr);
    assert(engine.getOrderBook(1)->getOrder(500) != nullptr);

    engine.stop();
    PASS();
}

int main() {
    std::cout << "\n═══ Graceful Shutdown (P3-6) Tests ═══" << std::endl;

    test_gtd_persists_day_cancelled();
    test_partial_fill_gtd_remainder_persists();
    test_partial_fill_day_cancelled();
    test_async_drain_ioc_and_gtd();
    test_no_journal_safe();

    std::cout << "\n─── Results: " << tests_passed << " passed ───" << std::endl;
    std::cout << "\nAll graceful shutdown tests passed.\n" << std::endl;
    return 0;
}
