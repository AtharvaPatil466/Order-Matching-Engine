// ItchPublisherAsyncTest — P3-1: ITCH publication is decoupled from the
// matching thread.
//
// With enableAsyncPublishing() the engine-facing calls (onOrderUpdate /
// onTrade, driven here from the matching thread by submitOrder) must only
// push a small POD event into the lock-free MPSC ring — no ITCH
// serialization, no sink invocation on that thread. A dedicated worker
// thread owned by the publisher drains the ring and runs the encode+send.
//
// Coverage:
//   1. The sink executes on a DIFFERENT thread than the caller (proof
//      that serialize/send is off the matching-thread critical path).
//   2. The full A/E/D stream still arrives, byte-correct, with the same
//      counts a synchronous publisher would produce.
//   3. stopAsyncPublishing() drains buffered events (nothing lost).
//   4. No drops under a keeping-up consumer.

#include "ItchProtocol.h"
#include "ItchPublisher.h"
#include "MatchingEngine.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

using namespace OrderMatcher;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)                                                        \
    std::cout << "  " << #name << "... " << std::flush;                   \
    try
#define END                                                               \
    catch (const std::exception& e) {                                     \
        std::cout << "FAIL: " << e.what() << "\n";                        \
        ++tests_failed;                                                   \
        return;                                                           \
    }                                                                     \
    std::cout << "ok\n";                                                  \
    ++tests_passed;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            throw std::runtime_error("CHECK failed: " #cond);             \
        }                                                                 \
    } while (0)

template <typename F>
static bool waitFor(F pred, int deadlineMs = 2000) {
    auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count() > deadlineMs) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

void test_SinkRunsOffCallerThread() {
    TEST(SinkRunsOffCallerThread) {
        MatchingEngine engine;
        engine.addSymbol(7);
        engine.start();
        auto* book = engine.getOrderBook(7);
        CHECK(book != nullptr);

        std::mutex mtx;
        std::vector<std::thread::id> sinkThreads;
        std::vector<uint8_t> firstFrame;
        ItchPublisher pub(*book, [&](std::string_view b) {
            std::lock_guard<std::mutex> lk(mtx);
            sinkThreads.push_back(std::this_thread::get_id());
            if (firstFrame.empty()) firstFrame.assign(b.begin(), b.end());
        });
        pub.enableAsyncPublishing();
        book->setEventListener(&pub);

        const std::thread::id callerTid = std::this_thread::get_id();

        // Resting buy → one 'A' frame, produced on the caller (matching)
        // thread but serialized/sent on the worker thread.
        auto r = engine.submitOrder(7, 100, 1, Side::Buy, 1000, 50,
                                    OrderType::Limit);
        CHECK(r.isAccepted());

        CHECK(waitFor([&] { return pub.addsEmitted() >= 1; }));

        std::lock_guard<std::mutex> lk(mtx);
        CHECK(!sinkThreads.empty());
        for (auto tid : sinkThreads) {
            CHECK(tid != callerTid
                  && "ITCH serialize/send must NOT run on the matching thread");
        }
        CHECK(firstFrame.size() == ITCH_SIZE_ADD_ORDER);
        CHECK(firstFrame[0] == ITCH_MT_ADD_ORDER);
        CHECK(readU64BE(firstFrame.data() + 11) == 100ULL);
        CHECK(pub.droppedEvents() == 0);

        pub.stopAsyncPublishing();
    } END
}

void test_FullStreamMatchesSyncCounts() {
    TEST(FullStreamMatchesSyncCounts) {
        MatchingEngine engine;
        engine.addSymbol(9);
        engine.start();
        auto* book = engine.getOrderBook(9);

        std::atomic<size_t> frames{0};
        ItchPublisher pub(*book, [&](std::string_view) {
            frames.fetch_add(1, std::memory_order_relaxed);
        });
        pub.enableAsyncPublishing();
        book->setEventListener(&pub);

        // Resting buy 50, then a sell 20 that partially fills it:
        //   A(1) A(2) E(buy) E(sell) D(2)  → 5 frames.
        engine.submitOrder(9, 1, 1, Side::Buy, 1000, 50, OrderType::Limit);
        engine.submitOrder(9, 2, 2, Side::Sell, 1000, 20, OrderType::Limit);

        CHECK(waitFor([&] { return frames.load() >= 5; }));

        // Let the worker settle, then assert exact counts.
        CHECK(waitFor([&] {
            return pub.addsEmitted() == 2 && pub.executedEmitted() == 2 &&
                   pub.deletesEmitted() == 1;
        }));
        CHECK(frames.load() == 5);
        CHECK(pub.eventsProcessed() >= 5);
        CHECK(pub.droppedEvents() == 0);

        pub.stopAsyncPublishing();
    } END
}

void test_StopDrainsBufferedEvents() {
    TEST(StopDrainsBufferedEvents) {
        // Submit a burst then immediately stop — the drain on stop must
        // flush everything still in the ring (no lost frames).
        MatchingEngine engine;
        engine.addSymbol(11);
        engine.start();
        auto* book = engine.getOrderBook(11);

        std::atomic<size_t> frames{0};
        ItchPublisher pub(*book, [&](std::string_view) {
            frames.fetch_add(1, std::memory_order_relaxed);
        });
        pub.enableAsyncPublishing();
        book->setEventListener(&pub);

        constexpr int kOrders = 500;
        uint64_t accepted = 0;
        for (int i = 0; i < kOrders; ++i) {
            // Distinct participants keep every resting order independent of
            // per-account risk caps; count how many the engine accepts so
            // the drain assertion targets the true 'A' population.
            auto r = engine.submitOrder(11, 1000 + i, /*pid=*/1000 + i,
                                        Side::Buy, 900 - i, 10,
                                        OrderType::Limit);
            if (r.isAccepted()) ++accepted;
        }
        // Stop right away — this joins the worker AND drains the rest.
        pub.stopAsyncPublishing();

        CHECK(accepted > 0);
        CHECK(pub.droppedEvents() == 0);
        CHECK(pub.addsEmitted() == accepted
              && "every accepted resting order's 'A' survives the drain");
        CHECK(frames.load() == accepted);
    } END
}

int main() {
    std::cout << "Running ItchPublisherAsyncTest\n";
    test_SinkRunsOffCallerThread();
    test_FullStreamMatchesSyncCounts();
    test_StopDrainsBufferedEvents();
    std::cout << "\n" << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
