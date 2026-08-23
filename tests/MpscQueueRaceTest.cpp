// MpscQueueRaceTest — concurrency regression for H5: the data race on
// MpscQueue::readPos_.
//
// Before the fix readPos_ was a plain size_t. pop() did `readPos_++` on the
// consumer thread while producers read the same object through approxSize()
// (and empty()) with no synchronisation — a C++ data race, which is undefined
// behaviour, not merely a stale read. TSan reports it.
//
// The suite never caught it because MatchingEngine::enqueueSafe only calls
// approxSize() when a backpressure threshold is configured, and the default is
// 0.0 (disabled). Both scenarios below therefore turn backpressure ON.
//
//   Scenario 1 — direct: N producer threads hammer push() + approxSize() +
//   empty() on one MpscQueue while a single consumer drains it. This is the
//   tightest formulation of the race.
//
//   Scenario 2 — production path: N threads call MatchingEngine::submitOrder
//   with a backpressure threshold set, so every submit routes through
//   enqueueSafe -> approxSize() while the worker thread pops.
//
// Both also assert the queue's actual contract under concurrency: every item
// pushed is popped exactly once, with nothing lost or duplicated. Run under
// TSan the pass condition is additionally "zero ThreadSanitizer reports".

#include "MatchingEngine.h"
#include "MpscQueue.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace OrderMatcher;

namespace {

constexpr int    kProducers      = 4;
constexpr int    kItemsPerThread = 20000;
constexpr size_t kCapacity       = 1024;  // deliberately small: forces the
                                          // queue to hover near full so the
                                          // consumer and producers stay
                                          // genuinely concurrent
// Wider for the engine scenario. At kCapacity, TSan's slowdown lets 4 producers
// outrun the single worker badly enough that ~93% of submits are shed on
// backpressure — which exercises approxSize() but barely exercises the drain.
constexpr size_t kEngineCapacity = 65536;

// ─── Scenario 1: the race, directly on MpscQueue ────────────────────────────
//
// Producers interleave push() with approxSize()/empty() — the two const
// accessors that read readPos_ — while the consumer advances it in pop().
void direct_queue_race() {
    MpscQueue<uint64_t> q(kCapacity);

    std::atomic<bool> producersDone{false};
    std::atomic<uint64_t> pushed{0};
    // Accumulated so the accessor calls cannot be optimised away.
    std::atomic<uint64_t> sizeObservations{0};

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kItemsPerThread; ++i) {
                // Encode (producer, index) so the consumer can verify that
                // every single item arrives exactly once.
                const uint64_t item =
                    (static_cast<uint64_t>(p) << 32) | static_cast<uint64_t>(i);

                // The racing reads: both touch readPos_ from a producer thread.
                sizeObservations.fetch_add(q.approxSize(), std::memory_order_relaxed);
                if (q.empty()) sizeObservations.fetch_add(1, std::memory_order_relaxed);

                while (!q.push(item)) {
                    // Full — spin until the consumer makes room. Keeps both
                    // sides hot on readPos_ for the whole run.
                    std::this_thread::yield();
                }
                pushed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Single consumer, as the queue's contract requires.
    std::vector<std::unordered_set<uint64_t>> seen(kProducers);
    uint64_t popped = 0;
    std::thread consumer([&] {
        uint64_t item;
        for (;;) {
            if (q.pop(item)) {
                const int      p = static_cast<int>(item >> 32);
                const uint64_t i = item & 0xFFFFFFFFULL;
                assert(p >= 0 && p < kProducers && "corrupt producer tag");
                assert(seen[p].insert(i).second && "item popped twice");
                ++popped;
                continue;
            }
            // Empty. Only stop once producers are finished AND a final drain
            // pass finds nothing, so nothing is left behind.
            if (producersDone.load(std::memory_order_acquire)) {
                if (!q.pop(item)) break;
                const int      p = static_cast<int>(item >> 32);
                const uint64_t i = item & 0xFFFFFFFFULL;
                assert(seen[p].insert(i).second && "item popped twice");
                ++popped;
                continue;
            }
            std::this_thread::yield();
        }
    });

    for (auto& t : producers) t.join();
    producersDone.store(true, std::memory_order_release);
    consumer.join();

    const uint64_t expected = static_cast<uint64_t>(kProducers) * kItemsPerThread;
    assert(pushed.load() == expected && "producer accounting wrong");
    assert(popped == expected && "items lost or duplicated across the ring");
    for (int p = 0; p < kProducers; ++p)
        assert(seen[p].size() == static_cast<size_t>(kItemsPerThread) &&
               "producer's items not all delivered");

    std::printf("[direct] %d producers x %d items: pushed=%llu popped=%llu\n",
                kProducers, kItemsPerThread,
                static_cast<unsigned long long>(pushed.load()),
                static_cast<unsigned long long>(popped));
}

// ─── Scenario 2: the production path through enqueueSafe ────────────────────
//
// setBackpressureThreshold(>0) is what arms the approxSize() call inside
// enqueueSafe. Without it the racing read never executes — which is precisely
// why the pre-existing suite ran TSan-clean over this file.
void engine_enqueue_safe_race() {
    MatchingEngine engine;
    constexpr SymbolId kSymbol = 1;
    engine.addSymbol(kSymbol);

    // Arm the approxSize() path. High enough that we mostly accept, low enough
    // that the check is meaningful rather than dead.
    engine.setBackpressureThreshold(0.90);
    engine.startAsync(/*numThreads=*/1, /*queueSize=*/kEngineCapacity);

    constexpr int kOrdersPerThread = 4000;
    std::atomic<int> accepted{0};
    std::atomic<int> rejected{0};

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kOrdersPerThread; ++i) {
                const OrderId id =
                    static_cast<OrderId>(p * kOrdersPerThread + i + 1);
                // All resting buys well below any ask: never crosses, never
                // trips the breaker, so the only failure mode is backpressure.
                auto r = engine.submitOrder(kSymbol, id, /*pid=*/7, Side::Buy,
                                            /*price=*/10000, /*qty=*/1,
                                            OrderType::Limit);
                if (r.isAccepted()) {
                    accepted.fetch_add(1, std::memory_order_relaxed);
                } else {
                    assert(r.rejectReason == RejectReason::QueueBackpressure &&
                           "only backpressure may reject in this scenario");
                    rejected.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& t : producers) t.join();

    engine.waitForDrain();

    const int total = kProducers * kOrdersPerThread;
    assert(accepted.load() + rejected.load() == total &&
           "accepted + rejected != submitted — order leak");

    std::printf("[engine] %d producers x %d orders: accepted=%d rejected=%d\n",
                kProducers, kOrdersPerThread, accepted.load(), rejected.load());

    engine.stop();
}

}  // namespace

int main() {
    direct_queue_race();
    engine_enqueue_safe_race();
    std::puts("MpscQueueRaceTest passed");
    return 0;
}
