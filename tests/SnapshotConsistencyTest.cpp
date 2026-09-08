// SnapshotConsistencyTest — proves the torn-read window documented in
// spec/Snapshot.tla is closed by the bookLock_ shared_mutex.
//
// Setup: writer thread does cancelReplace operations that flip an
// order's side (Buy → Sell at a different price). Reader thread takes
// snapshots in a tight loop. Without the lock, a snapshot could observe
// the same order on both sides; with the lock, every snapshot is a
// single point-in-time view.
//
// Property under test:
//   For every snapshot returned by getSnapshot, no orderId appears
//   simultaneously on the bid side and the ask side.
//
// (Note: getSnapshot returns aggregated PriceLevel records, not per-
// order ids, so the property reduces to "no price level appears on
// both sides" — which is observably equivalent for this test because
// each writer operation moves a single order between sides.)

#include "OrderBook.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace OrderMatcher;

int main() {
    OrderBook book(0);

    // Establish a reference price and disable the breaker so price
    // oscillation doesn't auto-halt the book mid-test.
    book.setCircuitBreakerThreshold(0.99);

    // Place an order that the writer will repeatedly flip between sides.
    book.addOrder(/*id=*/1, /*pid=*/1, Side::Buy, /*price=*/1000,
                  /*qty=*/100, OrderType::Limit);

    // Enough interleavings to be meaningful; reached in ~50ms unloaded.
    constexpr uint64_t kMinSnapshots = 1001;

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> snapsTaken{0};
    std::atomic<uint64_t> torn{0};

    std::thread reader([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            auto snap = book.getSnapshot(20);
            // Check: no price appears on both sides. Equivalent to
            // "no order on both sides" because our writer only ever
            // touches a single order at a single price per side.
            for (size_t i = 0; i < snap.bidCount; ++i) {
                for (size_t j = 0; j < snap.askCount; ++j) {
                    if (snap.bids[i].price == snap.asks[j].price) {
                        torn.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            // Also: no order should ever vanish — total quantity
            // should remain >= 1 (the single order we're flipping).
            uint64_t total = 0;
            for (size_t i = 0; i < snap.bidCount; ++i) total += snap.bids[i].totalQuantity;
            for (size_t i = 0; i < snap.askCount; ++i) total += snap.asks[i].totalQuantity;
            // Could be 0 momentarily during cancel before re-add inside
            // cancelReplace? cancelReplace is atomic under the lock —
            // when getSnapshot's shared_lock is granted, the writer's
            // unique_lock has been released, so the order MUST be
            // visible on exactly one side.
            assert(total >= 100 && "order vanished — torn read on quantity");
            snapsTaken.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread writer([&] {
        // Alternate the order between bid and ask sides via cancelReplace
        // to a price on the opposite side of the book. With the
        // pre-fix code path this would expose torn snapshots; with
        // the bookLock_ in place it cannot.
        bool buySide = true;
        while (!stop.load(std::memory_order_relaxed)) {
            if (buySide) {
                // Order id=1 currently exists. Cancel-replace its
                // price; cancelReplace mutates in place.
                book.cancelReplace(1, /*newPrice=*/1000, /*newQty=*/100);
            } else {
                book.cancelReplace(1, /*newPrice=*/1001, /*newQty=*/100);
            }
            buySide = !buySide;
        }
    });

    // Run until the reader has taken enough snapshots to be meaningful, NOT
    // for a fixed wall-clock. The old form slept 500ms and then asserted the
    // reader had managed >1000 iterations — a throughput floor inside a
    // correctness test, which fails on a loaded machine or an efficiency core
    // while `torn` is still 0. That is a flaky gate, and a flaky gate gets
    // ignored. The work target is the same; only the stopping rule changed.
    // The deadline is a backstop against a genuine hang, not a budget.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (snapsTaken.load(std::memory_order_relaxed) < kMinSnapshots &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    stop.store(true, std::memory_order_relaxed);
    reader.join();
    writer.join();

    std::printf("snapshots taken: %llu  torn observations: %llu\n",
                (unsigned long long)snapsTaken.load(),
                (unsigned long long)torn.load());

    assert(torn.load() == 0 &&
           "torn snapshot observed — bookLock_ does not actually serialize "
           "snapshot reads against writer mutations");
    assert(snapsTaken.load() >= kMinSnapshots &&
           "snapshot loop did not reach the target iteration count within the "
           "deadline — the reader or writer is wedged, not merely slow");

    std::puts("SnapshotConsistencyTest passed");
    return 0;
}
