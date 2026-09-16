// ShutdownWatchdogTest — a slow or wedged shutdown has to say so.
//
// stopAsync() ends in std::thread::join(), which has no timeout. A worker that
// was never going to finish looked exactly like one that was about to: the
// process sat in join() until the orchestrator's grace period expired and
// SIGKILLed it, and the logs said nothing at all. "The pod took 30s to
// terminate and we don't know why" is not a diagnosis.
//
// The fix is deliberately NOT to stop waiting. A wedged worker still owns the
// queues and book state stopAsync() is about to destroy, so detaching it would
// trade a visible hang for a use-after-free — a worse failure, and a quieter
// one. What changed is that the wait now reports who it is waiting on.
//
// GracefulShutdownTest covers that accepted work drains. This covers what
// happens when it doesn't.

#include "MatchingEngine.h"
#include "StructuredLog.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

using namespace OrderMatcher;

namespace {

int passed = 0;
#define TEST(name) std::cout << "  " << #name << "..." << std::flush
#define PASS() do { std::cout << " ok" << std::endl; ++passed; } while (0)

constexpr SymbolId kSym   = 0;
constexpr Price    kPrice = 1000000;

size_t countEvents(const CapturingSink& sink, std::string_view name) {
    size_t n = 0;
    for (const auto& e : sink.events) if (e.name == name) ++n;
    return n;
}

const LogEvent* findEvent(const CapturingSink& sink, std::string_view name) {
    for (const auto& e : sink.events) if (e.name == name) return &e;
    return nullptr;
}

std::string field(const LogEvent& e, std::string_view key) {
    for (const auto& [k, v] : e.fields) if (k == key) return v;
    return {};
}

// Holds the worker inside processRequest for a bounded time. onTrade is
// dispatched inline on the worker thread, so a listener that blocks there is a
// worker that cannot reach its exit check — the real shape of the hang,
// without needing an actually-unkillable thread in a test.
struct BlockingListener : EventListener {
    std::atomic<bool>         entered{false};
    std::chrono::milliseconds hold{0};

    void onTrade(const Trade&) override {
        entered.store(true, std::memory_order_release);
        std::this_thread::sleep_for(hold);
    }
};

void submitCross(MatchingEngine& engine, OrderId base) {
    engine.submitOrder(kSym, base,     1, Side::Buy,  kPrice, 10, OrderType::Limit);
    engine.submitOrder(kSym, base + 1, 2, Side::Sell, kPrice, 10, OrderType::Limit);
}

// Block until the worker is actually inside onTrade, so stopAsync() runs
// against a worker that genuinely cannot reach its exit check.
void awaitInsideCallback(BlockingListener& blocker) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!blocker.entered.load(std::memory_order_acquire)) {
        assert(std::chrono::steady_clock::now() < deadline && "no trade fired");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// ─── 1: a normal shutdown records every worker as exited, and says nothing ──
//
// The reporter must stay silent on the path everything takes. A watchdog that
// cries on every clean shutdown is one that gets filtered out of the logs
// before the day it matters.
void test_CleanShutdownIsSilent() {
    TEST(CleanShutdownIsSilent);
    CapturingSink cap;
    setObSink(&cap);

    {
        MatchingEngine engine;
        engine.addSymbol(kSym);
        engine.startAsync(2, 256);
        engine.setShutdownReportInterval(std::chrono::milliseconds(50));
        submitCross(engine, 1);
        engine.waitForDrain();
        engine.stopAsync();
    }

    assert(countEvents(cap, "shutdown_worker_still_running") == 0 &&
           "a clean shutdown must not report a stuck worker");
    setObSink(nullptr);
    PASS();
}

// ─── 2: a worker that has not exited is named, with its counters ────────────
void test_SlowWorkerIsReportedWithItsCounters() {
    TEST(SlowWorkerIsReportedWithItsCounters);
    CapturingSink cap;
    setObSink(&cap);

    BlockingListener blocker;
    blocker.hold = std::chrono::milliseconds(600);

    MatchingEngine engine;
    engine.addSymbol(kSym);
    engine.getOrderBook(kSym)->setEventListener(&blocker);
    engine.startAsync(1, 256);
    engine.setShutdownReportInterval(std::chrono::milliseconds(50));

    submitCross(engine, 1);
    awaitInsideCallback(blocker);

    const auto began = std::chrono::steady_clock::now();
    engine.stopAsync();
    const auto took = std::chrono::steady_clock::now() - began;

    // It waited for the worker rather than abandoning it.
    assert(took >= std::chrono::milliseconds(200) &&
           "stopAsync must wait for the worker, not detach it");

    const auto* report = findEvent(cap, "shutdown_worker_still_running");
    assert(report && "a shutdown that outran the interval must report");
    assert(report->severity == LogSeverity::Warn);
    assert(field(*report, "thread") == "0");
    // Enough to tell a stalled worker from a merely busy one.
    assert(!field(*report, "waited_ms").empty());
    assert(!field(*report, "submitted").empty());
    assert(!field(*report, "processed").empty());
    assert(!field(*report, "outstanding").empty());

    setObSink(nullptr);
    PASS();
}

// ─── 3: it keeps reporting, rather than warning once and going quiet ────────
//
// One line at the top of a shutdown that then hangs for the whole grace period
// scrolls away. The operator looking at a stuck pod needs the signal to still
// be arriving while they are looking at it.
void test_ReportRepeatsWhileWaiting() {
    TEST(ReportRepeatsWhileWaiting);
    CapturingSink cap;
    setObSink(&cap);

    BlockingListener blocker;
    blocker.hold = std::chrono::milliseconds(700);

    MatchingEngine engine;
    engine.addSymbol(kSym);
    engine.getOrderBook(kSym)->setEventListener(&blocker);
    engine.startAsync(1, 256);
    engine.setShutdownReportInterval(std::chrono::milliseconds(50));

    submitCross(engine, 1);
    awaitInsideCallback(blocker);

    engine.stopAsync();

    // ~700ms held against a 50ms interval. Asserting ">= 2" rather than an
    // exact count: this is a scheduler-timed test, and pinning the number
    // would make it flaky under a sanitizer or on a loaded CI box for no gain.
    assert(countEvents(cap, "shutdown_worker_still_running") >= 2 &&
           "the report must repeat while the wait continues");

    setObSink(nullptr);
    PASS();
}

// ─── 4: shutdown still completes, and the work still drained ────────────────
//
// The watchdog must not change the outcome it is watching.
void test_ShutdownStillCompletesAndDrains() {
    TEST(ShutdownStillCompletesAndDrains);
    setObSink(nullptr);

    BlockingListener blocker;
    blocker.hold = std::chrono::milliseconds(200);

    MatchingEngine engine;
    engine.addSymbol(kSym);
    engine.getOrderBook(kSym)->setEventListener(&blocker);
    engine.startAsync(2, 256);
    engine.setShutdownReportInterval(std::chrono::milliseconds(20));

    for (OrderId i = 1; i < 40; i += 2) submitCross(engine, i);

    engine.stopAsync();   // returns, rather than hanging

    // Both sides of every cross were consumed, so nothing is left resting.
    const auto snap =
        engine.getOrderBook(kSym)->getSnapshot(MarketDataSnapshot::MAX_DEPTH);
    assert(snap.bidCount == 0 && snap.askCount == 0 &&
           "accepted work must still drain across a reported shutdown");
    PASS();
}

}  // namespace

int main() {
    std::cout << "\n=== Shutdown Watchdog Tests ===\n\n";

    test_CleanShutdownIsSilent();
    test_SlowWorkerIsReportedWithItsCounters();
    test_ReportRepeatsWhileWaiting();
    test_ShutdownStillCompletesAndDrains();

    std::cout << "\n" << passed << " passed\n\n";
    return 0;
}
