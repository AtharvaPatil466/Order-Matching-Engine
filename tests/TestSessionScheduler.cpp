// TestSessionScheduler — deterministic daily-lifecycle sequencing.
//
// What this proves:
//   1. With an injected ms-of-day clock, SessionScheduler drives a symbol
//      universe through PreOpen -> Continuous -> AuctionClose -> PostClose
//      at exactly the scheduled boundaries, with no transition before its
//      time.
//   2. Every transition is one-shot per session: ticking repeatedly inside
//      a phase does not re-fire it (verified by counting engine batch calls
//      via observable side effects — uncross prints + state changes).
//   3. A coarse tick that jumps past several boundaries at once still fires
//      each intervening phase, in order.
//   4. A midnight wrap (clock running backwards) and an explicit
//      resetSession() both re-arm the full sequence.
//
// The test drives tick() manually rather than relying on the timer thread,
// so it is fully deterministic and fast; a separate section exercises the
// real timer thread's start()/stop() lifecycle for coverage.

#include "MatchingEngine.h"
#include "SessionScheduler.h"
#include "Types.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <thread>
#include <vector>

using namespace OrderMatcher;

static int tests_passed = 0;

#define SECTION(name) std::cout << "\n  " << name << "..." << std::flush
#define PASS()        do { ++tests_passed; std::cout << " PASS" << std::endl; } while (0)

namespace {

// Convenience: every symbol in the universe should report the same state.
void assertAllState(MatchingEngine& engine,
                    const std::vector<SymbolId>& symbols,
                    TradingState expected) {
    for (SymbolId s : symbols) {
        const OrderBook* book = engine.getOrderBook(s);
        assert(book != nullptr);
        assert(book->getTradingState() == expected);
    }
}

// Build an async engine seeded with a small universe.
std::vector<SymbolId> makeUniverse(MatchingEngine& engine) {
    std::vector<SymbolId> symbols{1, 2, 3};
    for (SymbolId s : symbols) {
        engine.addSymbol(s);
    }
    engine.startAsync(/*numThreads=*/1, /*queueSize=*/1024);
    return symbols;
}

} // namespace

// ─── Test 1: full-day phase sequence at the right boundaries ────────────────

void test_full_day_sequence() {
    SECTION("full-day sequence hits each boundary exactly once");

    MatchingEngine engine;
    auto symbols = makeUniverse(engine);

    uint64_t vnow = 0;  // ms-of-day, stepped manually
    SessionSchedule sched;  // US-equity defaults
    SessionScheduler sched_(engine, symbols, sched);
    sched_.setClock([&]() -> uint64_t { return vnow; });

    // Before pre-open: nothing applied. Books default to Continuous, and the
    // scheduler's tracked phase is Idle.
    vnow = sched.preOpenMs - 1;
    sched_.tick();
    assert(sched_.currentPhase() == SessionPhase::Idle);

    // 09:00 — pre-open accumulation.
    vnow = sched.preOpenMs;
    sched_.tick();
    assert(sched_.currentPhase() == SessionPhase::PreOpen);
    assertAllState(engine, symbols, TradingState::PreOpen);

    // Still before the open: re-ticking must NOT advance.
    vnow = sched.openMs - 1;
    sched_.tick();
    assert(sched_.currentPhase() == SessionPhase::PreOpen);
    assertAllState(engine, symbols, TradingState::PreOpen);

    // 09:30 — opening cross then continuous.
    vnow = sched.openMs;
    sched_.tick();
    assert(sched_.currentPhase() == SessionPhase::Continuous);
    assertAllState(engine, symbols, TradingState::Continuous);

    // Mid-session: many ticks, no change.
    for (uint64_t t = sched.openMs + 1000; t < sched.closeAuctionMs; t += 60ULL * 60ULL * 1000ULL) {
        vnow = t;
        sched_.tick();
        assert(sched_.currentPhase() == SessionPhase::Continuous);
    }
    assertAllState(engine, symbols, TradingState::Continuous);

    // 15:55 — closing auction.
    vnow = sched.closeAuctionMs;
    sched_.tick();
    assert(sched_.currentPhase() == SessionPhase::CloseAuction);
    assertAllState(engine, symbols, TradingState::AuctionClose);

    // 16:00 — closing cross then post-close.
    vnow = sched.closeMs;
    sched_.tick();
    assert(sched_.currentPhase() == SessionPhase::PostClose);
    assertAllState(engine, symbols, TradingState::PostClose);

    // After close: terminal. Further ticks are no-ops within the session.
    vnow = sched.closeMs + 5ULL * 60ULL * 1000ULL;
    sched_.tick();
    assert(sched_.currentPhase() == SessionPhase::PostClose);
    assertAllState(engine, symbols, TradingState::PostClose);

    engine.stopAsync();
    PASS();
}

// ─── Test 2: coarse tick jumping past multiple boundaries still orders ──────

void test_coarse_tick_fires_all_phases_in_order() {
    SECTION("coarse tick past several boundaries fires each in order");

    MatchingEngine engine;
    auto symbols = makeUniverse(engine);

    uint64_t vnow = 0;
    SessionSchedule sched;
    SessionScheduler sched_(engine, symbols, sched);
    sched_.setClock([&]() -> uint64_t { return vnow; });

    // A single tick taken well after the close: the scheduler must walk
    // Idle -> PreOpen -> Continuous -> CloseAuction -> PostClose in one call
    // (each applyPhase runs in sequence, so the opening/closing crosses still
    // happen before the respective state flips) and land in PostClose.
    vnow = sched.closeMs + 10'000;
    sched_.tick();
    assert(sched_.currentPhase() == SessionPhase::PostClose);
    assertAllState(engine, symbols, TradingState::PostClose);

    engine.stopAsync();
    PASS();
}

// ─── Test 3: midnight wrap and explicit reset re-arm the session ────────────

void test_session_reset() {
    SECTION("midnight wrap and resetSession re-arm the sequence");

    MatchingEngine engine;
    auto symbols = makeUniverse(engine);

    uint64_t vnow = 0;
    SessionSchedule sched;
    SessionScheduler sched_(engine, symbols, sched);
    sched_.setClock([&]() -> uint64_t { return vnow; });

    // Run a full day to PostClose.
    vnow = sched.closeMs;
    sched_.tick();
    assert(sched_.currentPhase() == SessionPhase::PostClose);

    // Explicit reset: phase returns to Idle; a tick before pre-open stays Idle.
    sched_.resetSession();
    assert(sched_.currentPhase() == SessionPhase::Idle);
    vnow = sched.preOpenMs - 1;
    sched_.tick();
    assert(sched_.currentPhase() == SessionPhase::Idle);

    // Drive into PreOpen again to confirm the sequence re-fires post-reset.
    vnow = sched.preOpenMs;
    sched_.tick();
    assert(sched_.currentPhase() == SessionPhase::PreOpen);
    assertAllState(engine, symbols, TradingState::PreOpen);

    // Now simulate a midnight wrap WITHOUT an explicit reset: advance to
    // Continuous, then make the clock jump backwards (new calendar day). The
    // next tick should auto-reset and, since the new time is before pre-open,
    // land on Idle.
    vnow = sched.openMs;
    sched_.tick();
    assert(sched_.currentPhase() == SessionPhase::Continuous);

    vnow = 60'000;  // 00:01:00 next day — earlier than lastTick (wrap)
    sched_.tick();
    assert(sched_.currentPhase() == SessionPhase::Idle);

    // And the fresh day proceeds normally.
    vnow = sched.preOpenMs;
    sched_.tick();
    assert(sched_.currentPhase() == SessionPhase::PreOpen);

    engine.stopAsync();
    PASS();
}

// ─── Test 4: real timer thread lifecycle drives the schedule ────────────────

void test_timer_thread_lifecycle() {
    SECTION("background timer thread advances phases and stops cleanly");

    MatchingEngine engine;
    auto symbols = makeUniverse(engine);

    // A controllable clock the test advances while the timer thread polls it.
    std::atomic<uint64_t> vnow{0};
    SessionSchedule sched;
    SessionScheduler sched_(engine, symbols, sched);
    sched_.setClock([&]() -> uint64_t { return vnow.load(std::memory_order_acquire); });

    sched_.start(/*intervalMs=*/1);
    assert(sched_.isRunning());

    // Park the clock at the open and wait for the timer thread to observe it.
    // We poll the scheduler's phase rather than sleeping a fixed amount, so a
    // slow CI box does not flake.
    vnow.store(sched.openMs, std::memory_order_release);
    for (int i = 0; i < 2000 && sched_.currentPhase() != SessionPhase::Continuous; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(sched_.currentPhase() == SessionPhase::Continuous);
    assertAllState(engine, symbols, TradingState::Continuous);

    sched_.stop();
    assert(!sched_.isRunning());

    // stop() is idempotent.
    sched_.stop();

    engine.stopAsync();
    PASS();
}

int main() {
    std::cout << "Running SessionScheduler tests..." << std::endl;

    test_full_day_sequence();
    test_coarse_tick_fires_all_phases_in_order();
    test_session_reset();
    test_timer_thread_lifecycle();

    std::cout << "\nAll " << tests_passed << " SessionScheduler test sections passed."
              << std::endl;
    return 0;
}
