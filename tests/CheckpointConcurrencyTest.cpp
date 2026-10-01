// CheckpointConcurrencyTest — only one checkpoint may be in flight at a time,
// and the one gracefulShutdown runs is never the one that gets dropped.
//
// WHAT WAS WRONG (THR-6). checkpointInternal is reached from several threads —
// main's size-based rotation (checkpoint()), worker 0 once checkpointPending_
// is set, waitForDrain(), and gracefulShutdown — and nothing serialised them.
// Two real overlaps: the rotation and worker 0's auto-checkpoint, and
// gracefulShutdown with worker 0, because waitForDrain() returns as soon as
// worker 0 bumps its processed count, which it does BEFORE it runs the
// checkpoint. Both callers then build <journal>.tmp outside journalMutex_. The
// second one's prepareRewrite remove()s the first one's finished file and starts
// its own; the first one's rename then publishes the SECOND one's file — however
// far it has got — over the live journal, and the second keeps writing through a
// non-append handle into what is now the live journal. A crash in that window
// restarts into a partial book.
//
// HOW THE OVERLAP IS FORCED. A structured-log sink that blocks inside the first
// "checkpoint_prepared" event parks that checkpoint exactly between building its
// replacement and swapping it in. Everything else runs on other threads while it
// is parked, so the interleaving is deterministic, not a timing race.
//
// WHAT IS PINNED.
//   1. A checkpoint requested while one is in flight is skipped, and the
//      in-flight one's prepared replacement is still on disk afterwards.
//   2. gracefulShutdown does NOT skip: it waits for the in-flight checkpoint and
//      then runs its own, so the file on disk when it returns is its snapshot.
//   3. The in-flight checkpoint commits, and a restart restores every order.

#include "MatchingEngine.h"
#include "TempPath.h"
#include "StructuredLog.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace OrderMatcher;

namespace {

constexpr auto kWait = std::chrono::seconds(10);
constexpr int kOrders = 3;

// Records every event name and parks the first "checkpoint_prepared" until
// release(). Thread-safe: the engine logs from several threads here.
class ParkingSink : public StructuredSink {
public:
    void log(const LogEvent& e) override {
        std::unique_lock<std::mutex> lk(m_);
        names_.emplace_back(e.name);
        cv_.notify_all();
        if (e.name == "checkpoint_prepared" && !parkedOnce_) {
            parkedOnce_ = true;
            cv_.wait(lk, [&] { return released_; });
        }
    }

    bool waitForCount(const std::string& name, size_t n) {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, kWait, [&] { return countLocked(name) >= n; });
    }

    size_t count(const std::string& name) {
        std::lock_guard<std::mutex> lk(m_);
        return countLocked(name);
    }

    void release() {
        std::lock_guard<std::mutex> lk(m_);
        released_ = true;
        cv_.notify_all();
    }

private:
    size_t countLocked(const std::string& name) const {
        return static_cast<size_t>(std::count(names_.begin(), names_.end(), name));
    }

    std::mutex m_;
    std::condition_variable cv_;
    std::vector<std::string> names_;
    bool parkedOnce_ = false;
    bool released_ = false;
};

size_t countOrders(const OrderBook& book) {
    size_t n = 0;
    book.forEachOrder([&](const Order&) { ++n; });
    return n;
}

bool exists(const std::string& path) { return ::access(path.c_str(), F_OK) == 0; }

void testOneCheckpointAtATimeAndShutdownWaits() {
    std::cout << "Running testOneCheckpointAtATimeAndShutdownWaits..." << std::endl;
    const std::string path = uniqueTempPath("checkpoint_concurrency.journal");
    const std::string tmpPath = path + ".tmp";
    std::remove(path.c_str());
    std::remove(tmpPath.c_str());

    {
        MatchingEngine engine;
        engine.addSymbol(1);
        engine.enableJournal(path);
        engine.start();
        for (int i = 0; i < kOrders; ++i) {
            // Non-crossing GTC buys: all rest.
            engine.submitOrder(1, 100 + i, 1, Side::Buy, 990000 - i * 10000, 10,
                               OrderType::Limit, 0, 0, TimeInForce::GTC);
        }

        ParkingSink sink;
        setObSink(&sink);

        std::thread first([&] { engine.checkpoint(); });
        assert(sink.waitForCount("checkpoint_prepared", 1) &&
               "first checkpoint never reached its prepared state");
        assert(exists(tmpPath) && "prepared replacement missing before any overlap");

        // (1) A second checkpoint while the first is between prepare and commit.
        engine.checkpoint();
        assert(exists(tmpPath) &&
               "a concurrent checkpoint destroyed the in-flight one's prepared "
               "replacement (THR-6)");
        assert(sink.count("checkpoint_skipped_busy") == 1 &&
               "the overlapping checkpoint must be skipped and say so");

        // (2) gracefulShutdown must wait for the in-flight one, not skip.
        MatchingEngine::ShutdownReport report{};
        std::thread shutdown([&] { report = engine.gracefulShutdown(); });
        assert(sink.waitForCount("checkpoint_waiting", 1) &&
               "gracefulShutdown's checkpoint did not wait for the in-flight one");

        sink.release();
        first.join();
        shutdown.join();
        setObSink(nullptr);

        // (3) Both ran, in order, and neither failed.
        assert(sink.count("checkpoint_prepared") == 2 &&
               "gracefulShutdown's checkpoint did not run after the in-flight one");
        assert(sink.count("checkpoint_skipped_busy") == 1 &&
               "gracefulShutdown's checkpoint was skipped");
        assert(sink.count("checkpoint_commit_failed") == 0);
        assert(sink.count("checkpoint_prepare_failed") == 0);
        assert(!exists(tmpPath) && "a prepared replacement was left behind");
        assert(report.ordersPersisted == static_cast<size_t>(kOrders));
        engine.stop();
    }

    MatchingEngine restarted;
    restarted.addSymbol(1);
    restarted.enableJournal(path);
    (void)restarted.replayJournal();
    assert(countOrders(*restarted.getOrderBook(1)) == static_cast<size_t>(kOrders));

    std::remove(path.c_str());
    std::cout << "testOneCheckpointAtATimeAndShutdownWaits PASSED" << std::endl;
}

}  // namespace

int main() {
    testOneCheckpointAtATimeAndShutdownWaits();
    std::cout << "All CheckpointConcurrencyTest tests passed." << std::endl;
    return 0;
}
