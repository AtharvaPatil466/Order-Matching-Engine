// QueueChaosTest — drive MatchingEngine with spurious push failures and
// spurious pop "empty" returns injected into MpscQueue. Verifies:
//
//   1. Every submitOrder either succeeds (sequence id assigned, order lands
//      in the book) OR returns QueueBackpressure — never both, never neither.
//   2. accepted + rejected == submitted (no orders silently lost).
//   3. The book contains exactly the accepted orders (no double-apply).
//   4. The consumer loop survives spurious-empty pops without livelock or
//      skipping items (every accepted order eventually applies).
//
// Requires -DENABLE_FAULT_INJECTION=ON.

#include "FaultInjector.h"
#include "MatchingEngine.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace OrderMatcher;

namespace {

void run_scenario(const char* tag, double pushFailProb, double popEmptyProb,
                  uint32_t maxPushRetries = 1000) {
    auto& fi = FaultInjector::instance();
    fi.reset();
    fi.seed(0xC4A05);
    fi.arm("queue.push.spurious_fail",  pushFailProb);
    fi.arm("queue.pop.spurious_empty",  popEmptyProb);

    MatchingEngine engine;
    constexpr SymbolId kSymbol = 1;
    engine.addSymbol(kSymbol);
    engine.setMaxPushRetries(maxPushRetries);
    // Async path engages the MpscQueue — submitOrder enqueues, worker pops.
    // Plain start() would short-circuit the queue and bypass the fault points.
    engine.startAsync(/*numThreads=*/1, /*queueSize=*/8192);

    constexpr int kOrders = 2000;
    std::vector<OrderId> acceptedIds;
    acceptedIds.reserve(kOrders);
    int rejected = 0;

    for (int i = 0; i < kOrders; ++i) {
        OrderId id = static_cast<OrderId>(i + 1);
        // All buys at a single deep price; never crosses, never trips
        // volatility breakers, never exhausts capacity at any one level.
        auto r = engine.submitOrder(kSymbol, id, /*pid=*/7, Side::Buy,
                                     /*price=*/10000, /*qty=*/1,
                                     OrderType::Limit);
        if (r.isAccepted()) {
            acceptedIds.push_back(id);
        } else {
            // The only legitimate rejection in this scenario is queue
            // backpressure — anything else means our test setup is wrong.
            if (r.rejectReason != RejectReason::QueueBackpressure) {
                std::printf("unexpected reject reason=%d at i=%d\n",
                            int(r.rejectReason), i);
                std::abort();
            }
            ++rejected;
        }
    }

    // Total submissions accounted for.
    assert(acceptedIds.size() + rejected == static_cast<size_t>(kOrders) &&
           "accepted + rejected != submitted — order leak");

    // Wait for the consumer to drain. With spurious empties this may take
    // longer than usual, but waitForDrain accounts for actually-submitted work.
    engine.waitForDrain();
    // Belt-and-braces: small extra grace for pop-side wake-ups.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto* book = engine.getOrderBook(kSymbol);
    assert(book && "book missing");

    // Every accepted order must be findable in the book exactly once. No
    // duplicates means the engine's apply path is single-shot.
    std::unordered_set<OrderId> seen;
    for (OrderId id : acceptedIds) {
        const Order* o = book->getOrder(id);
        assert(o != nullptr && "accepted order missing from book");
        assert(seen.insert(id).second && "duplicate order in book");
    }
    // Book size matches accepted count exactly — no spurious orders.
    std::vector<const Order*> all(acceptedIds.size() + 16);
    size_t bookCount = book->getAllOrders(all.data(), all.size());
    assert(bookCount == acceptedIds.size() &&
           "book size mismatch with accepted count");

    uint64_t pushFires = fi.activations("queue.push.spurious_fail");
    uint64_t popFires  = fi.activations("queue.pop.spurious_empty");
    std::printf("[%s] accepted=%zu rejected=%d "
                "push_fail fires=%llu pop_empty fires=%llu\n",
                tag, acceptedIds.size(), rejected,
                static_cast<unsigned long long>(pushFires),
                static_cast<unsigned long long>(popFires));

    engine.stop();
}

}  // namespace

int main() {
#ifndef OB_ENABLE_FAULT_INJECTION
    std::puts("QueueChaosTest: OB_ENABLE_FAULT_INJECTION not defined — "
              "reconfigure with -DENABLE_FAULT_INJECTION=ON.");
    return 0;
#endif

    // Push-only: 50% per-call fail with 1000 retries — retry loop should
    // absorb every fault, producing zero rejections.
    run_scenario("push-retry-absorbs", /*push=*/0.5, /*pop=*/0.0);

    // Pop-only: consumer must not livelock or skip items.
    run_scenario("pop-spurious-empty", /*push=*/0.0, /*pop=*/0.5);

    // Combined chaos.
    run_scenario("combined", /*push=*/0.3, /*pop=*/0.3);

    // Force the rejection path: drop retries to 2 so the backpressure-reject
    // route actually fires under aggressive push faults. Validates that
    // SubmitResult::rejected(QueueBackpressure) is the only failure mode and
    // that the book size matches accepted exactly.
    run_scenario("backpressure-rejects", /*push=*/0.95, /*pop=*/0.0,
                 /*maxPushRetries=*/2);

    std::puts("QueueChaosTest passed");
    return 0;
}
