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
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <limits>
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

// THR-13 and D5: a size-triggered checkpoint runs off the matching thread, and
// the size trigger fires at all — on io_uring too.
//
// Async with one worker, as OrderEngine runs. Immediate, so each record is on
// disk before the trigger looks at the size and the threshold is crossed by
// exactly the last of kPlaced adds and their cancels (behind the sequence-id
// lease record, D1). The sink parks that
// checkpoint between building its replacement and swapping it in, and while it
// is parked the worker must still match a new order. Worker 0 used to run the
// checkpoint itself, so its symbols stopped for as long as the snapshot took to
// write and sync — and here, until the park ended. What the worker does in the
// meantime (an order, a fill, a cancel) must survive the swap: it is copied
// onto the replacement, and a restart replays it on top of the snapshot.
//
// In the io_uring lane this is also the proof the SIZE trigger fires there:
// bytesOnDisk_ did not move on that path (JRN-8), so it never did.
void testSizeCheckpointRunsOffTheMatchingThread() {
    std::cout << "Running testSizeCheckpointRunsOffTheMatchingThread..." << std::endl;
    constexpr OrderId kPlaced = 40, kKept = 4, kLate = kPlaced + 1;   // one tick below the rest
    constexpr size_t kRecords = 1 + kPlaced + (kPlaced - kKept);   // lease + adds + cancels
    constexpr size_t kThreshold = sizeof(JournalFileHeader) + kRecords * sizeof(JournalEntry);
    const std::string path = uniqueTempPath("checkpoint_off_worker.journal");
    std::remove(path.c_str());
    std::remove((path + ".tmp").c_str());

    struct LateAck : EventListener {
        std::atomic<bool> seen{false};
        void onTrade(const Trade&) override {}
        void onOrderUpdate(const OrderUpdate& u) override {
            if (u.orderId == kLate && u.status == OrderStatus::Accepted)
                seen.store(true, std::memory_order_release);
        }
        void onMarketData(const MarketDataUpdate&) override {}
        void onBookVisible(const BookVisibleUpdate&) override {}
    } late;
    const auto buy = [](MatchingEngine& e, OrderId id) {
        e.submitOrder(1, id, 1, Side::Buy, 990000 - static_cast<Price>(id) * 100, 10,
                      OrderType::Limit, 0, 0, TimeInForce::GTC);
    };

    ParkingSink sink;
    bool parked = false, matched = false, committed = false;
    size_t bytesAfter = 0;
    {
        MatchingEngine engine;
        engine.addSymbol(1);
        engine.getOrderBook(1)->setEventListener(&late);
        engine.enableJournal(path, Journal::SyncPolicy::Immediate);
        engine.setCheckpointThresholds(std::numeric_limits<size_t>::max(), kThreshold);
        setObSink(&sink);
        engine.startAsync(1);
        for (OrderId id = 1; id <= kPlaced; ++id) buy(engine, id);
        for (OrderId id = kKept + 1; id <= kPlaced; ++id) engine.submitCancel(1, id);

        parked = sink.waitForCount("checkpoint_prepared", 1);
        if (parked) {
            buy(engine, kLate);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!late.seen.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            matched = late.seen.load(std::memory_order_acquire);
            if (matched) {
                // More for the swap to carry over: a fill and a cancel.
                engine.submitOrder(1, 2000, 2, Side::Sell, 990000 - 100, 10, OrderType::Limit,
                                   0, 0, TimeInForce::GTC);   // fills order 1
                engine.submitCancel(1, 2);
                engine.waitForDrain();
            }
        }
        sink.release();
        committed = sink.waitForCount("checkpoint_committed", 1);
        engine.waitForDrain();
        engine.stopAsync();
        setObSink(nullptr);
        bytesAfter = static_cast<size_t>(std::filesystem::file_size(path));
    }
    std::cout << "  parked=" << parked << " matched while parked=" << matched
              << " committed=" << committed << " journal " << bytesAfter
              << " bytes (threshold " << kThreshold << ")" << std::endl;
    assert(parked && "the size threshold never triggered a checkpoint");
    assert(matched && "the worker stopped matching while a checkpoint ran (THR-13)");
    assert(committed && "the size-triggered checkpoint did not commit");
    assert(bytesAfter < kThreshold && "the checkpoint did not compact the journal");

    MatchingEngine restarted;
    restarted.addSymbol(1);
    restarted.enableJournal(path);
    (void)restarted.replayJournal();
    const OrderBook& book = *restarted.getOrderBook(1);
    // Kept 1..4, then during the checkpoint: kLate rests, 1 fills, 2 is cancelled.
    assert(countOrders(book) == static_cast<size_t>(kKept + 1 - 2) &&
           "what happened while the checkpoint was built did not survive its swap");
    assert(book.getOrder(kLate) && book.getOrder(kKept) && !book.getOrder(kKept + 1));
    assert(!book.getOrder(1) && !book.getOrder(2) && !book.getOrder(2000));
    std::remove(path.c_str());
    std::cout << "testSizeCheckpointRunsOffTheMatchingThread PASSED" << std::endl;
}

}  // namespace

int main() {
    testOneCheckpointAtATimeAndShutdownWaits();
    testSizeCheckpointRunsOffTheMatchingThread();
    std::cout << "All CheckpointConcurrencyTest tests passed." << std::endl;
    return 0;
}
