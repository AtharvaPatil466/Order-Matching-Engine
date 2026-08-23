// KillSwitchCompletenessTest — C5 regression. The kill switch must be
// COMPLETE, not best-effort.
//
// Pre-fix, setKillSwitch / killSwitch(pid) / the expiry sweep all called
// enqueueSafe and ignored its return. enqueueSafe gives up after
// maxPushRetries_ and drops the message (MatchingEngine.cpp:89-93). Under load
// that produced the worst state a safety control can reach: new orders rejected
// by the flag, every resting order still live. Worse, the drop also skipped the
// submittedTotal_ increment, so the waitForDrain() that followed had nothing to
// wait for and returned immediately — the caller believed a sweep had run.
//
// The invariant under test: after setKillSwitch(true) returns, ZERO resting
// orders survive. Same for killSwitch(pid), scoped to that participant.
//
// RiskControlsTest already covers the kill switch, but only sequentially, in
// sync mode, over two orders — never with a contended queue or concurrent
// producers, which is the only regime where C5 manifests.
//
// Each scenario runs against a deliberately small ring with maxPushRetries_
// set to 1, which is what makes the pre-fix drop overwhelmingly likely rather
// than a rare race.

#include "FaultInjector.h"
#include "MatchingEngine.h"

#include <atomic>
#include <chrono>
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

using namespace OrderMatcher;

namespace {

constexpr SymbolId kSymbol          = 1;
constexpr int      kLoadThreads     = 4;
constexpr int      kOrdersPerThread = 3000;
constexpr size_t   kSmallQueue      = 256;  // small ring => contended control
                                            // enqueue => pre-fix drop
constexpr ParticipantId kVictim    = 7;
constexpr ParticipantId kBystander = 8;

// Async engine with the volatility breaker relaxed. Scenario 2 rests a
// bystander at 9000 and the victim at 10000; that ~11% spread trips the default
// breaker and parks the book in VolatilityAuction, which has nothing to do with
// the kill switch under test.
void configure(MatchingEngine& engine) {
    engine.addSymbol(kSymbol);
    // One retry: pre-fix this makes the control-message drop the common case
    // rather than a rare interleaving.
    engine.setMaxPushRetries(1);
    engine.startAsync(/*numThreads=*/1, /*queueSize=*/kSmallQueue);
    engine.getOrderBook(kSymbol)->setCircuitBreakerThreshold(0.99);
}

// pid == 0 means "every participant".
size_t restingCount(MatchingEngine& engine, ParticipantId pid) {
    auto* book = engine.getOrderBook(kSymbol);
    assert(book && "book missing");
    size_t n = 0;
    book->forEachOrderLocked([&](const Order& o) {
        if (pid == 0 || o.participantId == pid) ++n;
    });
    return n;
}

// Poll until `pid` has at least `target` orders resting.
//
// Backs off between polls on purpose. restingCount takes the book's bookLock_,
// which is a plain std::mutex, and macOS mutexes are not fair: a tight
// re-acquire loop here starves the single worker thread so the book never grows
// and the poll never terminates. Bounded so a real failure is a diagnosable
// failure rather than a hang.
bool waitForDepth(MatchingEngine& engine, ParticipantId pid, size_t target) {
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + seconds(20);
    while (steady_clock::now() < deadline) {
        if (restingCount(engine, pid) >= target) return true;
        std::this_thread::sleep_for(milliseconds(1));
    }
    std::printf("FAIL: book never reached %zu resting orders for pid=%llu\n",
                target, static_cast<unsigned long long>(pid));
    return false;
}

// Spawn load threads that submit resting buys until `stop` is set.
// Prices stay well below any ask so nothing crosses and everything rests.
std::vector<std::thread> startLoad(MatchingEngine& engine,
                                   std::atomic<bool>& stop,
                                   std::atomic<int>& nextId,
                                   ParticipantId pid) {
    std::vector<std::thread> threads;
    for (int t = 0; t < kLoadThreads; ++t) {
        threads.emplace_back([&engine, &stop, &nextId, pid] {
            for (int i = 0; i < kOrdersPerThread && !stop.load(std::memory_order_relaxed); ++i) {
                const OrderId id =
                    static_cast<OrderId>(nextId.fetch_add(1, std::memory_order_relaxed));
                engine.submitOrder(kSymbol, id, pid, Side::Buy,
                                   /*price=*/10000, /*qty=*/1, OrderType::Limit);
            }
        });
    }
    return threads;
}

// ─── Scenario 1: engine-wide kill switch under concurrent load ──────────────
void engine_kill_switch_is_complete() {
    MatchingEngine engine;
    configure(engine);

    std::atomic<bool> stop{false};
    std::atomic<int>  nextId{1};
    auto load = startLoad(engine, stop, nextId, kVictim);

    // Let the book build up real depth before firing.
    if (!waitForDepth(engine, 0, 200)) std::abort();

    engine.setKillSwitch(true);

    // THE INVARIANT: setKillSwitch has returned, so nothing may still rest.
    // Checked before joining the load threads, so producers are still hammering
    // submitOrder — every one of those must now be refused, at submit or at
    // apply. A survivor here is exactly the C5 partial state.
    const size_t survivors = restingCount(engine, 0);
    if (survivors != 0) {
        std::printf("FAIL: %zu orders survived an engaged kill switch\n", survivors);
        std::abort();
    }

    stop.store(true, std::memory_order_relaxed);
    for (auto& t : load) t.join();

    // Still zero after the producers finish draining their last submissions.
    engine.waitForDrain();
    const size_t after = restingCount(engine, 0);
    if (after != 0) {
        std::printf("FAIL: %zu orders rested after the kill switch engaged\n", after);
        std::abort();
    }

    std::printf("[engine-kill] submitted~%d, survivors=0, control_spins=%llu\n",
                nextId.load() - 1,
                static_cast<unsigned long long>(engine.getControlSpinCount()));
    engine.stop();
}

// ─── Scenario 2: participant kill leaves bystanders untouched ───────────────
void participant_kill_is_complete_and_scoped() {
    MatchingEngine engine;
    configure(engine);

    std::atomic<bool> stop{false};
    std::atomic<int>  nextId{1};

    // A bystander rests orders that must NOT be swept.
    for (int i = 0; i < 50; ++i) {
        engine.submitOrder(kSymbol, static_cast<OrderId>(1'000'000 + i), kBystander,
                           Side::Buy, /*price=*/9000, /*qty=*/1, OrderType::Limit);
    }
    engine.waitForDrain();
    const size_t bystanderBefore = restingCount(engine, kBystander);
    assert(bystanderBefore == 50 && "bystander setup did not rest");

    auto load = startLoad(engine, stop, nextId, kVictim);
    if (!waitForDepth(engine, kVictim, 200)) std::abort();

    engine.killSwitch(kVictim);

    // Victim fully swept...
    const size_t victimSurvivors = restingCount(engine, kVictim);
    if (victimSurvivors != 0) {
        std::printf("FAIL: %zu victim orders survived killSwitch(pid)\n", victimSurvivors);
        std::abort();
    }
    // ...and the sweep was scoped: the bystander is untouched.
    const size_t bystanderAfter = restingCount(engine, kBystander);
    if (bystanderAfter != bystanderBefore) {
        std::printf("FAIL: participant kill collaterally cancelled %zu bystander orders\n",
                    bystanderBefore - bystanderAfter);
        std::abort();
    }

    stop.store(true, std::memory_order_relaxed);
    for (auto& t : load) t.join();

    std::printf("[participant-kill] victim survivors=0, bystanders intact=%zu\n",
                bystanderAfter);
    engine.stop();
}

// ─── Scenario 3: the control message is never dropped ───────────────────────
//
// Direct check on the mechanism rather than its effect: with the ring kept
// saturated and maxPushRetries_ at 1, an order-flow enqueueSafe is expected to
// shed (that is its job), while every control enqueue must still land. If a
// control message were dropped the sweep below would not run and survivors
// would be non-zero.
void control_messages_are_never_dropped() {
    MatchingEngine engine;
    configure(engine);

    std::atomic<bool> stop{false};
    std::atomic<int>  nextId{1};
    auto load = startLoad(engine, stop, nextId, kVictim);
    if (!waitForDepth(engine, 0, 100)) std::abort();

    // Fire repeatedly while the ring is hot — every one must be honoured.
    for (int round = 0; round < 5; ++round) {
        engine.killSwitch(kVictim);
        const size_t survivors = restingCount(engine, kVictim);
        if (survivors != 0) {
            std::printf("FAIL: round %d left %zu survivors — control message dropped\n",
                        round, survivors);
            std::abort();
        }
    }

    stop.store(true, std::memory_order_relaxed);
    for (auto& t : load) t.join();

    std::printf("[no-drop] 5 sweeps under saturation, survivors=0, "
                "order_flow_dropped=%llu control_spins=%llu\n",
                static_cast<unsigned long long>(engine.getDroppedCount()),
                static_cast<unsigned long long>(engine.getControlSpinCount()));
    engine.stop();
}

// ─── Scenario 4: the actual reproducer (needs fault injection) ──────────────
//
// Scenarios 1-3 are INVARIANT GUARDS, not reproducers: they pass against the
// pre-fix code too. The reason is structural — setKillSwitch sets
// killSwitchActive_ BEFORE dispatching the sweep, so producers immediately stop
// feeding, the ring drains, and the single un-retried control push lands after
// all. The drop needs the ring to be full at the instant the control message is
// pushed, which load alone does not reliably produce.
//
// queue.push.spurious_fail forces exactly that condition. Armed at 0.98 with
// maxPushRetries_ = 1, an enqueueSafe control message is dropped with p ~= 0.98
// per worker, while enqueueControl simply spins ~50 times and lands. Pre-fix
// the sweep therefore never runs and the victim's orders survive; post-fix it
// always runs. This is the scenario that actually goes red without the fix.
void kill_switch_survives_a_hostile_queue() {
#ifndef OB_ENABLE_FAULT_INJECTION
    std::puts("[hostile-queue] skipped — build with -DENABLE_FAULT_INJECTION=ON");
    return;
#else
    auto& fi = FaultInjector::instance();
    fi.reset();
    fi.seed(0xC5C5C5);

    MatchingEngine engine;
    configure(engine);

    // Rest the victim's orders with the queue healthy.
    for (int i = 0; i < 300; ++i) {
        engine.submitOrder(kSymbol, static_cast<OrderId>(i + 1), kVictim, Side::Buy,
                           /*price=*/10000, /*qty=*/1, OrderType::Limit);
    }
    engine.waitForDrain();
    const size_t before = restingCount(engine, kVictim);
    assert(before > 0 && "setup did not rest any victim orders");

    // Now make the ring hostile and fire. The sweep must still land.
    fi.arm("queue.push.spurious_fail", 0.98);
    engine.killSwitch(kVictim);
    fi.disarm("queue.push.spurious_fail");

    const size_t survivors = restingCount(engine, kVictim);
    if (survivors != 0) {
        std::printf("FAIL: %zu of %zu victim orders survived — the control "
                    "message was dropped by a hostile queue\n", survivors, before);
        std::abort();
    }

    std::printf("[hostile-queue] rested=%zu survivors=0 push_fail_fires=%llu "
                "control_spins=%llu\n",
                before,
                static_cast<unsigned long long>(fi.activations("queue.push.spurious_fail")),
                static_cast<unsigned long long>(engine.getControlSpinCount()));
    fi.reset();
    engine.stop();
#endif
}

}  // namespace

int main() {
    engine_kill_switch_is_complete();
    participant_kill_is_complete_and_scoped();
    control_messages_are_never_dropped();
    kill_switch_survives_a_hostile_queue();
    std::puts("KillSwitchCompletenessTest passed");
    return 0;
}
