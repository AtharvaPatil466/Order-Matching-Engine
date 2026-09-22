// Pins OrderBook::kMaxStopExecutionsPerSweep — the bound on how much work ONE
// incoming order can be made to do by the stop orders resting in the book.
//
// The mechanism being bounded is NOT unbounded recursion. checkStopOrders takes
// lastTradePrice BY VALUE, so a triggered stop's own fill price cannot elect
// further stops inside the same sweep (testLadderDoesNotCascade pins that). The
// unbounded term is fan-out: one print can elect every stop resting at or
// through that price, and each election runs a full match(). Measured before
// the bound, one order into a book with 10,000 co-located stops took 2.13 ms
// against a 237 ns P50.
//
// The bound must DEFER, never DROP. testDeferredStopsSurvivePriceRecovery is
// the one that matters: it pushes the price back up through the stop level
// after the budget runs out, which without the isStopTriggered latch would
// silently un-elect every stop that did not make the cut.

#include "OrderBook.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <vector>

using namespace OrderMatcher;

namespace {

constexpr Price kBase = 100000;
constexpr ParticipantId kMaker     = 2;    // resting bid liquidity
constexpr ParticipantId kStopOwner = 3;    // owns every stop order
constexpr ParticipantId kWarmup    = 5;
constexpr ParticipantId kTaker     = 6;
constexpr ParticipantId kRecovery  = 7;    // supplies the ask for the recovery print

struct FillCounter : EventListener {
    size_t trades = 0;
    size_t stopFills = 0;   // one per stop order that actually executed
    Price  maxPrice = 0;
    Price  lastPrice = 0;
    void onTrade(const Trade& t) override {
        ++trades;
        lastPrice = t.price;
        maxPrice = std::max(maxPrice, t.price);
        if (t.sellerId == kStopOwner) ++stopFills;
    }
};

enum class Arm { None, Colocated, Ladder };

// A book whose bid side is `levels` single-lot rungs descending from kBase-1,
// plus one rung well above for a warm-up print (checkStopOrders is only reached
// once lastTradePrice_ > 0). Then `stops` sell stops are armed. Arming never
// sweeps: addOrder returns inside parkNonMatchingOrder.
//
// A sell stop elects when the print is at or below its stopPrice, and becomes a
// Limit sell at price 1 — so it takes exactly one rung, whatever the best bid.
struct Fixture {
    OrderBook book;
    FillCounter fills;
    OrderId nextId = 1;

    Fixture(size_t levels, size_t stops, Arm arm)
        : book(0, MatchAlgorithm::PriceTime, (levels + stops) * 2 + 1024) {
        // The default 5% breaker is seeded from the first limit price and would
        // reject the deep end of the ladder. Orthogonal to what is under test.
        book.setCircuitBreakerThreshold(1000.0);

        add(kMaker, Side::Buy, kBase + 10, OrderType::Limit);
        for (size_t k = 1; k <= levels; ++k)
            add(kMaker, Side::Buy, kBase - static_cast<Price>(k), OrderType::Limit);
        add(kWarmup, Side::Sell, kBase + 10, OrderType::Limit);  // prints kBase+10

        // Attached after the warm-up so the counters only see the test's own flow.
        book.setEventListener(&fills);

        if (arm != Arm::None) {
            for (size_t k = 0; k < stops; ++k) {
                const Price sp = (arm == Arm::Ladder) ? kBase - 1 - static_cast<Price>(k)
                                                      : kBase - 1;
                add(kStopOwner, Side::Sell, 1, OrderType::Stop, sp);
            }
        }
    }

    void add(ParticipantId p, Side s, Price px, OrderType t, Price stopPx = 0) {
        auto r = book.addOrder(nextId++, p, s, px, 1, t, stopPx);
        assert(std::holds_alternative<OrderId>(r) && "fixture order was rejected");
        (void)r;
    }

    // The order that starts the cascade: sells one lot into the best bid.
    // Returns how many stops executed inside that single addOrder call.
    size_t sendTaker() {
        const size_t before = fills.stopFills;
        add(kTaker, Side::Sell, 1, OrderType::Limit);
        return fills.stopFills - before;
    }

    // A passive sell parked far above the book. It never trades, so it changes
    // no price — it exists only to reach the stop sweep that addOrder runs at
    // the end of every call, which is where a deferred election gets executed.
    size_t sendPassive() {
        const size_t before = fills.stopFills;
        add(kTaker, Side::Sell, kBase + 1000, OrderType::Limit);
        return fills.stopFills - before;
    }

    // Prints ABOVE every stop's trigger level, by resting an ask and lifting it.
    void printRecovery() {
        add(kRecovery, Side::Sell, kBase + 5, OrderType::Limit);
        add(kTaker,    Side::Buy,  kBase + 5, OrderType::Limit);
    }
};

const size_t kCap = OrderBook::kMaxStopExecutionsPerSweep;

// ── 1. One print electing far more stops than the cap does bounded work, and
//       every elected stop still fires. ───────────────────────────────────────
void testFanOutIsBoundedAndNothingIsDropped() {
    std::printf("  fan-out bounded + drains ...");
    const size_t kStops = kCap * 8 + 7;  // deliberately not a multiple of the cap

    Fixture f(kStops + 1, kStops, Arm::Colocated);

    const size_t firstSweep = f.sendTaker();
    assert(firstSweep == kCap &&
           "one incoming order must execute exactly kMaxStopExecutionsPerSweep stops");

    size_t fired = firstSweep;
    size_t sweeps = 1;
    while (fired < kStops) {
        fired += f.sendPassive();
        ++sweeps;
        assert(sweeps <= kStops && "deferred stops are not draining — they were dropped");
    }

    assert(fired == kStops && "every elected stop must eventually fire");
    assert(sweeps == (kStops + kCap - 1) / kCap &&
           "drain took more sweeps than the cap accounts for");
    std::printf(" %zu stops, %zu inline, drained in %zu sweeps PASSED\n",
                kStops, kCap, sweeps);
}

// ── 2. The bound defers rather than drops even when the market moves back. ────
void testDeferredStopsSurvivePriceRecovery() {
    std::printf("  deferral survives price recovery ...");
    const size_t kStops = kCap * 3;

    Fixture f(kStops + 2, kStops, Arm::Colocated);

    assert(f.sendTaker() == kCap && "first sweep should be capped");

    // Sell stops elect on prints at or below kBase-1. This prints kBase+5, so
    // on trigger price alone the stops still parked would no longer qualify.
    f.printRecovery();
    assert(f.fills.maxPrice >= kBase + 5 && "recovery print did not happen");

    size_t fired = f.fills.stopFills;
    size_t guard = 0;
    while (fired < kStops) {
        fired += f.sendPassive();
        assert(++guard <= kStops && "latched stops were dropped by the price recovery");
    }
    assert(fired == kStops && "every stop elected before the recovery must still fire");
    std::printf(" all %zu fired PASSED\n", kStops);
}

// ── 3. The predicted cascade mechanism, pinned as absent. ───────────────────
//    A ladder in which each triggered stop's own fill trips the next one does
//    NOT cascade: the sweep's price is frozen at entry, so exactly one stop
//    executes per incoming order however deep the ladder is.
void testLadderDoesNotCascade() {
    std::printf("  ladder does not cascade ...");
    const size_t kStops = 256;
    Fixture f(kStops + 2, kStops, Arm::Ladder);

    assert(f.sendTaker() == 1 && "a laddered stop must not elect the next one in-sweep");
    assert(f.sendPassive() == 1 && "the ladder should advance exactly one rung per order");
    std::printf(" 1 per order at depth %zu PASSED\n", kStops);
}

// ── 4. trailingStopOrders_ has the identical fan-out shape and the identical
//       bound. One deep bid rung absorbs the taker and every election, because
//       a triggered trailing stop becomes a Limit at its own trigger level and
//       cannot walk down a ladder of single-lot rungs. ──────────────────────
void testTrailingStopFanOutIsBounded() {
    std::printf("  trailing-stop fan-out bounded ...");
    const size_t kStops = kCap * 2;

    OrderBook book(0, MatchAlgorithm::PriceTime, kStops * 2 + 1024);
    book.setCircuitBreakerThreshold(1000.0);
    FillCounter fills;
    OrderId id = 1;

    book.addOrder(id++, kMaker, Side::Buy, kBase + 10, 1, OrderType::Limit);
    book.addOrder(id++, kMaker, Side::Buy, kBase - 1, kStops + 1, OrderType::Limit);
    book.addOrder(id++, kWarmup, Side::Sell, kBase + 10, 1, OrderType::Limit);  // prints kBase+10
    book.setEventListener(&fills);

    // trailRefPrice seeds from the kBase+10 print; trail of 11 puts every
    // trigger level at kBase-1, so one print at kBase-1 elects all of them.
    for (size_t k = 0; k < kStops; ++k) {
        auto r = book.addOrder(id++, kStopOwner, Side::Sell, kBase - 1, 1,
                               OrderType::TrailingStop, 0, 0, TimeInForce::GTC, 0, 0,
                               PegType::None, 0, /*trailAmount=*/11);
        assert(std::holds_alternative<OrderId>(r) && "trailing stop was rejected");
        (void)r;
    }

    book.addOrder(id++, kTaker, Side::Sell, kBase - 1, 1, OrderType::Limit);
    assert(fills.stopFills == kCap && "trailing-stop sweep must honour the same cap");

    size_t guard = 0;
    while (fills.stopFills < kStops) {
        book.addOrder(id++, kTaker, Side::Sell, kBase + 1000, 1, OrderType::Limit);
        assert(++guard <= kStops && "deferred trailing stops were dropped");
    }
    assert(fills.stopFills == kStops && "every trailing stop must eventually fire");
    std::printf(" %zu stops, %zu inline PASSED\n", kStops, kCap);
}

// ── 5. A book with no stops armed is untouched by any of this. ───────────────
void testNoStopsIsUnchanged() {
    std::printf("  no stops armed ...");
    Fixture f(8, 0, Arm::None);
    assert(f.sendTaker() == 0 && "no stops means no stop fills");
    assert(f.fills.trades == 1 && "the taker should produce exactly its own fill");
    assert(f.fills.lastPrice == kBase - 1 && "taker must fill at the best bid");
    std::printf(" PASSED\n");
}

// Not an assertion — the number the bound exists for. A wall-clock assertion in
// CI is a flake generator; this only records what one order costs.
void reportLatency() {
    std::printf("\n  single-order latency, N stops all elected by that one print:\n");
    std::printf("    %8s %12s %12s\n", "N", "min ns", "median ns");
    for (size_t n : {kCap, kCap * 4, kCap * 16}) {
        std::vector<double> ns;
        for (int trial = 0; trial < 15; ++trial) {
            Fixture f(n + 1, n, Arm::Colocated);
            const auto t0 = std::chrono::steady_clock::now();
            f.sendTaker();
            const auto t1 = std::chrono::steady_clock::now();
            ns.push_back(static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
        }
        std::sort(ns.begin(), ns.end());
        std::printf("    %8zu %12.0f %12.0f\n", n, ns.front(), ns[ns.size() / 2]);
    }
}

} // namespace

int main() {
    std::printf("\n=== Stop Cascade Bound Tests (cap = %zu) ===\n", kCap);
    testNoStopsIsUnchanged();
    testLadderDoesNotCascade();
    testFanOutIsBoundedAndNothingIsDropped();
    testDeferredStopsSurvivePriceRecovery();
    testTrailingStopFanOutIsBounded();
    reportLatency();
    std::printf("\nALL STOP CASCADE BOUND TESTS PASSED\n");
    return 0;
}
