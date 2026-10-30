// stopAsyncPublishing() with a resync pending, while a matching thread
// publishes.
//
// Two fixes meet here. LF-5 made a producer that arrives during teardown wait
// in dispatch() until the ring is drained and freed. MD-9 made a dropped event
// trigger a resync, which takes the book lock. A producer publishes from inside
// the book's own operations, so it waits in dispatch() HOLDING the book lock,
// and the stop thread's resync then waits for that lock: neither can move.
//
// The schedule below forces it: the sink parks the worker so the ring
// overflows (resync pending), the stop thread closes the gate and blocks on the
// join, a matching thread submits an order (book lock held, gate closed), and
// then the sink is released.

#include "ItchPublisher.h"
#include "MatchingEngine.h"
#include "OrderBook.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>

using namespace OrderMatcher;
using namespace std::chrono_literals;

namespace {

bool waitFor(const std::atomic<bool>& flag, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

} // namespace

int main() {
    MatchingEngine engine;
    engine.addSymbol(1);
    engine.start();
    OrderBook* book = engine.getOrderBook(1);

    std::atomic<bool> open{false};
    std::atomic<bool> entered{false};
    ItchPublisher pub(*book, [&](std::string_view) {
        entered.store(true, std::memory_order_release);
        while (!open.load(std::memory_order_acquire)) std::this_thread::sleep_for(1ms);
    });
    pub.enableAsyncPublishing(/*ringCapacity=*/8);
    book->setEventListener(&pub);

    // Park the worker in the sink, then overflow the 8-slot ring.
    engine.submitOrder(1, 1, 1, Side::Buy, 980, 10, OrderType::Limit);
    if (!waitFor(entered, 2000ms)) {
        std::fprintf(stderr, "FAIL: the publisher worker never reached the sink\n");
        open.store(true);
        return 1;
    }
    OrderId id = 2;
    for (int i = 0; i < 20; ++i)
        engine.submitOrder(1, id++, 10 + i, Side::Buy, 970 - i, 10, OrderType::Limit);
    if (pub.droppedEvents() == 0) {
        std::fprintf(stderr, "FAIL: the ring never overflowed, so no resync is pending\n");
        open.store(true);
        return 1;
    }

    std::atomic<bool> stopped{false};
    std::thread stopper([&] {
        pub.stopAsyncPublishing();   // closes the gate, then joins the parked worker
        stopped.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(100ms);

    std::atomic<bool> submitted{false};
    std::thread producer([&] {
        engine.submitOrder(1, id, 99, Side::Sell, 1010, 10, OrderType::Limit);
        submitted.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(100ms);

    open.store(true, std::memory_order_release);   // the worker finishes and exits

    const bool stopDone = waitFor(stopped, 10000ms);
    const bool submitDone = waitFor(submitted, 1000ms);
    if (!stopDone || !submitDone) {
        std::fprintf(stderr,
            "FAIL: deadlock — stopAsyncPublishing %s, the producer's submit %s "
            "(the producer waits at the closed gate holding the book lock; the "
            "stop thread's resync waits for that lock)\n",
            stopDone ? "returned" : "never returned",
            submitDone ? "returned" : "never returned");
        std::fflush(stderr);
        std::_Exit(1);   // both threads are stuck; they cannot be joined
    }
    stopper.join();
    producer.join();

    if (pub.asyncEnabled()) {
        std::fprintf(stderr, "FAIL: async publishing still enabled after stop\n");
        return 1;
    }
    if (pub.resyncsEmitted() == 0) {
        std::fprintf(stderr, "FAIL: the dropped events were never followed by a resync\n");
        return 1;
    }
    std::printf("ItchStopResyncDeadlockTest passed (dropped %llu, resyncs %llu)\n",
                static_cast<unsigned long long>(pub.droppedEvents()),
                static_cast<unsigned long long>(pub.resyncsEmitted()));
    return 0;
}
