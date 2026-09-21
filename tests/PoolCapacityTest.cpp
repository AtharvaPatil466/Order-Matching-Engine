// PoolCapacityTest — P3-8 order-pool exhaustion monitoring + controlled
// degradation.
//
// Properties asserted:
//   * poolCapacity / poolInUse / poolUtilization track the pool accurately.
//   * The per-symbol utilization gauge is published to the MetricsRegistry.
//   * ≥80% utilization emits a warning event (order_pool_pressure_warn).
//   * ≥95% utilization REJECTS new orders with RejectReason::PoolCapacityExceeded
//     (never crashes, never silently drops), while resting orders remain
//     cancellable — the book degrades gracefully instead of falling over.
//   * 100% utilization behaves like the kill switch: no new orders + a critical
//     alert event (order_pool_exhausted).
//
//   * The default pool size comes from config (order_pool_capacity /
//     OB_ORDER_POOL_CAPACITY), and an explicit ctor argument overrides it.
//
// Uses a small pool (via the OrderBook orderPoolCapacity ctor arg) so the
// thresholds are reached in a handful of orders rather than a full pool.

#include "Metrics.h"
#include "OrderBook.h"
#include "StructuredLog.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>   // setenv/unsetenv — the config keys are env-overridable
#include <string>
#include <variant>

using namespace OrderMatcher;

static int passed = 0;
#define SECTION(name) std::printf("  %s...", name); std::fflush(stdout)
#define PASS() do { ++passed; std::puts(" PASS"); } while (0)

static bool hasEvent(const CapturingSink& sink, const std::string& name) {
    for (const auto& e : sink.events)
        if (std::string(e.name) == name) return true;
    return false;
}

// Add a resting buy at a fixed price (all same price → no crossing, no circuit
// breaker trip). Returns the raw addOrder result.
static AddOrderResult addResting(OrderBook& book, OrderId id) {
    return book.addOrder(id, /*participant=*/1, Side::Buy, /*price=*/100000,
                         /*qty=*/1, OrderType::Limit);
}

// ─── Test 1: utilization accessors + gauge ──────────────────────────────────

void test_utilization_and_gauge() {
    SECTION("Utilization accessors + Prometheus gauge");
    OrderBook book(42, MatchAlgorithm::PriceTime, /*orderPoolCapacity=*/100);

    for (OrderId i = 1; i <= 50; ++i) {
        assert(std::holds_alternative<OrderId>(addResting(book, i)));
    }
    assert(book.poolCapacity() == 100);
    assert(book.poolInUse() == 50);
    assert(book.poolUtilization() > 0.49 && book.poolUtilization() < 0.51);

    // Gauge reflects pre-add utilization; last update was at the 50th add
    // (inUse=49) → 49 percent.
    auto& g = MetricsRegistry::instance().gauge(
        "order_pool_utilization_percent{symbol=\"42\"}");
    assert(g.value() == 49);
    PASS();
}

// ─── Test 2: 80% warn ───────────────────────────────────────────────────────

void test_warn_at_80pct() {
    SECTION("Warn event at 80% utilization");
    CapturingSink sink;
    setObSink(&sink);

    OrderBook book(43, MatchAlgorithm::PriceTime, 100);
    for (OrderId i = 1; i <= 85; ++i) {
        assert(std::holds_alternative<OrderId>(addResting(book, i)));
    }
    assert(hasEvent(sink, "order_pool_pressure_warn"));
    // Below the reject band, nothing has been shed yet.
    assert(book.getPoolRejectCount() == 0);

    setObSink(nullptr);  // restore fallback
    PASS();
}

// ─── Test 3: 95% reject with graceful degradation ───────────────────────────

void test_reject_at_95pct() {
    SECTION("Reject at 95% with PoolCapacityExceeded, book stays serviceable");
    CapturingSink sink;
    setObSink(&sink);

    OrderBook book(44, MatchAlgorithm::PriceTime, 100);
    // 95 orders fit (pre-add inUse 0..94 all < 95%).
    for (OrderId i = 1; i <= 95; ++i) {
        assert(std::holds_alternative<OrderId>(addResting(book, i)));
    }
    assert(book.poolInUse() == 95);

    // The 96th sees inUse=95 → 95% → shed.
    auto r = addResting(book, 96);
    assert(std::holds_alternative<RejectReason>(r));
    assert(std::get<RejectReason>(r) == RejectReason::PoolCapacityExceeded);
    assert(book.getPoolRejectCount() >= 1);
    assert(hasEvent(sink, "order_pool_rejecting_new"));
    // Nothing landed for the shed order.
    assert(book.getOrder(96) == nullptr);

    // Graceful degradation: resting orders can still be cancelled, and once a
    // slot frees, a new order is admitted again.
    book.cancelOrder(1);
    assert(book.poolInUse() == 94);
    assert(std::holds_alternative<OrderId>(addResting(book, 200)));

    setObSink(nullptr);
    PASS();
}

// ─── Test 4: 100% behaves like the kill switch + critical alert ─────────────

void test_exhaustion_alert_at_100pct() {
    SECTION("100% behaves like kill switch: no new orders + critical alert");
    CapturingSink sink;
    setObSink(&sink);

    OrderBook book(45, MatchAlgorithm::PriceTime, /*orderPoolCapacity=*/1);
    assert(std::holds_alternative<OrderId>(addResting(book, 1)));
    assert(book.poolInUse() == 1);
    assert(book.poolUtilization() > 0.999);  // 100%

    // Next order: pool full → shed + critical alert, never a crash.
    auto r = addResting(book, 2);
    assert(std::holds_alternative<RejectReason>(r));
    assert(std::get<RejectReason>(r) == RejectReason::PoolCapacityExceeded);
    assert(hasEvent(sink, "order_pool_exhausted"));

    // The resting order is intact and still cancellable (nothing was dropped).
    assert(book.getOrder(1) != nullptr);
    book.cancelOrder(1);
    assert(book.poolInUse() == 0);

    setObSink(nullptr);
    PASS();
}

// ─── Test 5: default pool is unaffected (no false shedding) ──────────────────
//
// The requirement is "a book on the engine default sheds nothing under a load
// that is light FOR THAT DEFAULT" — not "the default is 200,000". The default
// is configurable (order_pool_capacity / OB_ORDER_POOL_CAPACITY), so both the
// load and the bound are expressed relative to the capacity this book actually
// got. Filling a tenth of the pool must stay below the 80% warn band, and far
// below the 95% shed band.
void test_default_pool_no_false_reject() {
    SECTION("Default-capacity book sheds nothing under light load");
    OrderBook book(46);  // engine default pool
    const OrderId light = static_cast<OrderId>(book.poolCapacity() / 10);
    assert(light > 0 && "the default pool must have room for a light load");
    for (OrderId i = 1; i <= light; ++i) {
        assert(std::holds_alternative<OrderId>(addResting(book, i)));
    }
    assert(book.getPoolRejectCount() == 0);
    assert(book.poolUtilization() < 0.80);
    PASS();
}

// ─── Test 6: the engine default is config-driven ────────────────────────────
//
// The pool is the dominant term in a book's resident footprint (~192 B per
// slot, all touched at construction), so an operator has to be able to size it
// per deployment instead of recompiling. It comes from `order_pool_capacity` /
// OB_ORDER_POOL_CAPACITY, and an explicit ctor argument still overrides it.
void test_default_pool_capacity_is_configurable() {
    SECTION("OB_ORDER_POOL_CAPACITY sizes the default pool; ctor arg still wins");

    ::setenv("OB_ORDER_POOL_CAPACITY", "4096", /*overwrite=*/1);
    OrderBook configured(47);
    assert(configured.poolCapacity() == 4096);

    OrderBook explicitCap(48, MatchAlgorithm::PriceTime, /*orderPoolCapacity=*/512);
    assert(explicitCap.poolCapacity() == 512 && "ctor argument beats config");

    ::unsetenv("OB_ORDER_POOL_CAPACITY");
    OrderBook fallback(49);
    assert(fallback.poolCapacity() == 10000 && "built-in default when unconfigured");

    PASS();
}

int main() {
    std::puts("\n═══ Pool Capacity (P3-8) Tests ═══\n");

    test_utilization_and_gauge();
    test_warn_at_80pct();
    test_reject_at_95pct();
    test_exhaustion_alert_at_100pct();
    test_default_pool_no_false_reject();
    test_default_pool_capacity_is_configurable();

    std::printf("\n─── Results: %d passed ───\n", passed);
    std::puts("\nAll pool capacity tests passed.\n");
    return 0;
}
