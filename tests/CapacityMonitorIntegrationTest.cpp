// CapacityMonitorIntegrationTest — the monitor has to be LIVE, not present.
//
// CapacityMonitor, IncidentLogger and AlertDispatcher were all unit-tested and
// all constructed in zero production code paths. capacityMonitor_ in
// particular was a member of MatchingEngine with a getter, no callbacks, and
// no thread: it could not observe anything, nothing ever called start(), and
// its unit test passed anyway because a unit test builds its own monitor and
// hands it its own callbacks. That is the failure mode this file exists to
// catch — a test suite proving a component works while nothing uses it.
//
// So these tests never construct a CapacityMonitor. They start a real
// MatchingEngine and assert that (1) starting the engine installs resource
// callbacks and runs the monitor thread, (2) a breach reaches the structured
// log sink the rest of the engine already reports through, and (3) stopping
// the engine stops the monitor promptly instead of blocking exit for a whole
// check interval.

#include "MatchingEngine.h"
#include "StructuredLog.h"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace OrderMatcher;

namespace {

int passed = 0;
#define TEST(name) std::cout << "  " << #name << "..." << std::flush
#define PASS() do { std::cout << " ok" << std::endl; ++passed; } while (0)

// CapturingSink is documented as unsynchronised, and here the emitter is the
// monitor's background thread while the assertions run on this one. Lock.
class LockedSink : public StructuredSink {
public:
    void log(const LogEvent& e) override {
        std::lock_guard<std::mutex> lock(mu_);
        events_.push_back(e);
    }

    std::vector<LogEvent> snapshot() const {
        std::lock_guard<std::mutex> lock(mu_);
        return events_;
    }

private:
    mutable std::mutex mu_;
    std::vector<LogEvent> events_;
};

std::string field(const LogEvent& e, std::string_view key) {
    for (const auto& [k, v] : e.fields) if (k == key) return v;
    return {};
}

// First capacity_alert naming `resource`, or nullptr.
const LogEvent* findAlert(const std::vector<LogEvent>& events,
                          std::string_view resource) {
    for (const auto& e : events) {
        if (e.name == "capacity_alert" && field(e, "resource") == resource) return &e;
    }
    return nullptr;
}

std::filesystem::path tempJournalPath() {
    return std::filesystem::temp_directory_path() / "capacity_monitor_itest.wal";
}

// ─────────────────────────────────────────────────────────────────────────────
// 1. The engine installs the callbacks and runs the monitor.
//
// Both thresholds are set BELOW zero before the engine starts, so ANY reading
// a real callback returns is a breach. That is the whole point: this does not
// test the 90% policy (CapacityThresholds already owns that, and its defaults
// are untouched) — it tests that a callback is installed at all and that
// something is calling it. A monitor with no callbacks, or with callbacks but
// no thread, produces nothing here no matter how long it waits.
void test_EngineStartInstallsCallbacksAndRunsMonitor() {
    TEST(EngineStartInstallsCallbacksAndRunsMonitor);

    const auto journalPath = tempJournalPath();
    std::filesystem::remove(journalPath);

    LockedSink sink;
    setObSink(&sink);

    {
        MatchingEngine engine;
        engine.addSymbol(1);
        // Before startAsync: the journal directory is what the disk callback
        // is built from, and the monitor thread reads thresholds_ unguarded.
        assert(engine.enableJournal(journalPath.string()) &&
               "temp journal must open");

        CapacityThresholds t;      // checkIntervalMs stays at its default 1000
        t.queueDepthPct  = -1.0;
        t.journalDiskPct = -1.0;
        engine.getCapacityMonitor().setThresholds(t);

        engine.startAsync(2, 1024);

        // One check interval is 1s; allow a wide margin for a loaded or
        // sanitised runner rather than sleeping a fixed amount.
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(20);
        while (engine.getCapacityMonitor().alertsRaised() < 2 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        engine.stopAsync();  // joins the monitor: the snapshot below is stable
    }

    const auto events = sink.snapshot();

    const LogEvent* queueAlert = findAlert(events, "queue_depth");
    assert(queueAlert != nullptr &&
           "startAsync must install a queue-depth callback and run the monitor");
    assert(!field(*queueAlert, "usage_pct").empty());
    assert(field(*queueAlert, "level") == "WARNING");

    const LogEvent* diskAlert = findAlert(events, "journal_disk");
    assert(diskAlert != nullptr &&
           "an engine with a journal must monitor the journal filesystem");
    assert(field(*diskAlert, "level") == "CRITICAL");

    // A probe that could not stat the filesystem would have returned 0.0 and
    // reported itself; the disk reading above must be a real measurement.
    for (const auto& e : events) {
        assert(e.name != "capacity_probe_failed" &&
               "journal disk probe failed on a path the engine just wrote to");
    }

    setObSink(nullptr);
    std::filesystem::remove(journalPath);
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. A breach reaches the structured sink — deterministically.
//
// Test 1 rigs the threshold and uses the engine's real callback. This one is
// the mirror: the shipped default threshold (journalDiskPct = 0.90) against a
// callback under the test's control, so the breach needs no disk to actually
// fill up. Together they pin both halves of the path.
void test_ThresholdBreachReachesStructuredSink() {
    TEST(ThresholdBreachReachesStructuredSink);

    LockedSink sink;
    setObSink(&sink);

    {
        MatchingEngine engine;
        engine.addSymbol(1);
        engine.startAsync(1, 1024);

        auto& monitor = engine.getCapacityMonitor();
        // Quiesce the background thread before touching a callback it reads.
        // stopAsync() below calls stop() again; it is idempotent.
        monitor.stop();
        monitor.setDiskUsageCallback([] { return 0.97; });
        monitor.checkNow();

        engine.stopAsync();
    }

    const auto events = sink.snapshot();
    const LogEvent* alert = findAlert(events, "journal_disk");
    assert(alert != nullptr && "97% of a 90%-threshold disk must raise an alert");
    assert(field(*alert, "level") == "CRITICAL");
    assert(field(*alert, "usage_pct").rfind("97", 0) == 0);
    assert(field(*alert, "detail").find("97.0%") != std::string::npos);

    // Below threshold must stay silent — otherwise the assertion above proves
    // only that the monitor logs unconditionally.
    {
        LockedSink quiet;
        setObSink(&quiet);
        MatchingEngine engine;
        engine.addSymbol(1);
        engine.startAsync(1, 1024);
        auto& monitor = engine.getCapacityMonitor();
        monitor.stop();
        monitor.setDiskUsageCallback([] { return 0.10; });
        monitor.checkNow();
        engine.stopAsync();
        assert(findAlert(quiet.snapshot(), "journal_disk") == nullptr &&
               "10% used must not alert against a 90% threshold");
    }

    setObSink(nullptr);
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. stop() is honoured, and does not become the shutdown.
//
// The monitor sleeps between checks. Sleeping the full interval in one call
// makes join() wait it out, which would add up to a second to every engine
// teardown and sit inside the shutdown watchdog's reporting window. The
// bound belongs in a test because it is invisible until someone times it.
void test_StopAsyncDoesNotWaitOutTheCheckInterval() {
    TEST(StopAsyncDoesNotWaitOutTheCheckInterval);

    MatchingEngine engine;
    engine.addSymbol(1);
    engine.startAsync(1, 1024);

    // Land inside a sleep slice rather than racing start().
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const auto begin = std::chrono::steady_clock::now();
    engine.stopAsync();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin);

    // Default checkIntervalMs is 1000. Generous margin for worker drain and
    // for a sanitised build, but far below a full interval.
    assert(elapsed < std::chrono::milliseconds(500) &&
           "stopAsync must not block for a whole capacity-check interval");

    PASS();
}

}  // namespace

int main() {
    std::cout << "CapacityMonitorIntegrationTest\n";
    test_EngineStartInstallsCallbacksAndRunsMonitor();
    test_ThresholdBreachReachesStructuredSink();
    test_StopAsyncDoesNotWaitOutTheCheckInterval();
    std::cout << passed << " passed\n";
    return 0;
}
