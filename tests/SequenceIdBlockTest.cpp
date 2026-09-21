// SequenceIdBlockTest — sequence ids stay unique and per-thread-monotonic now
// that they are handed out in per-thread BLOCKS.
//
// WHY THIS EXISTS. `nextSubmitSequence_` used to be one engine-wide atomic
// `fetch_add` on every single order, which — together with two other global
// counters — is what pinned this engine's scaling flat past two workers. It was
// replaced by a thread-local block allocator: a thread claims a block of ids
// with ONE relaxed fetch_add and then hands them out from thread-local state,
// so the contended RMW happens once per block instead of once per order.
//
// That trade is only safe if the ids keep the properties their consumers rely
// on. `SubmitResult::sequenceId` is echoed back to clients by the binary
// gateway and printed by the admin server, so:
//
//   * ids must be globally UNIQUE — two clients must never be told the same id
//   * ids must never be 0, which callers treat as "no sequence assigned"
//   * ids must be monotonic WITHIN a thread
//
// Note what is deliberately NOT asserted: ids are no longer densely ordered
// across threads. Thread B can hold a block numerically below thread A's while
// submitting later. Nothing consumes a global ordering, and pinning a property
// the design intentionally gave up would freeze the contention back in.
//
// SubmitResultTest.cpp:31,39 already covers the single-threaded case
// (non-zero, increasing). The failure mode block allocation introduces is a
// CROSS-thread one — two threads served from overlapping ranges, or a block
// boundary handing back a duplicate — so this test is explicitly concurrent and
// submits enough per thread to cross several block boundaries.

#include "MatchingEngine.h"

#include <cassert>
#include <cstdio>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace OrderMatcher;

int main() {
    constexpr int    kThreads          = 8;
    constexpr int    kSubmitsPerThread = 5000;   // several blocks' worth
    constexpr size_t kExpected         = static_cast<size_t>(kThreads) * kSubmitsPerThread;

    MatchingEngine engine;
    engine.addSymbol(1);
    engine.startAsync(4, 1 << 20);

    std::vector<std::vector<uint64_t>> perThread(kThreads);
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&perThread, &engine, t] {
            perThread[t].reserve(kSubmitsPerThread);
            for (int i = 0; i < kSubmitsPerThread; ++i) {
                // Unique order ids per thread so nothing is rejected as a
                // duplicate. A rejected submit still gets a sequence id, but
                // keeping the flow clean makes any failure here unambiguous.
                const OrderId id =
                    static_cast<OrderId>(t) * kSubmitsPerThread + i + 1;
                SubmitResult r = engine.submitOrder(
                    1, id, static_cast<ParticipantId>(t + 1),
                    (i % 2) ? Side::Buy : Side::Sell,
                    10000 + (i % 100), 10, OrderType::Limit);
                perThread[t].push_back(r.sequenceId);
            }
        });
    }
    for (auto& th : threads) th.join();

    engine.waitForDrain();
    engine.stopAsync();

    // ── Never zero, and monotonic within each thread ────────────────────────
    for (int t = 0; t < kThreads; ++t) {
        assert(perThread[t].size() == static_cast<size_t>(kSubmitsPerThread));
        uint64_t prev = 0;
        for (uint64_t id : perThread[t]) {
            assert(id != 0 && "sequence id 0 means 'unassigned' to callers");
            assert(id > prev &&
                   "ids must increase within a thread — a block boundary that "
                   "hands back a lower id breaks the only ordering callers have");
            prev = id;
        }
    }

    // ── Globally unique across threads ──────────────────────────────────────
    //
    // This is the assertion block allocation could actually break: two threads
    // served overlapping ranges would show up here and nowhere else.
    std::unordered_set<uint64_t> seen;
    seen.reserve(kExpected * 2);
    for (int t = 0; t < kThreads; ++t) {
        for (uint64_t id : perThread[t]) {
            const bool fresh = seen.insert(id).second;
            assert(fresh && "duplicate sequence id — two clients would be told "
                            "the same id for different orders");
        }
    }
    assert(seen.size() == kExpected);

    std::printf("SequenceIdBlockTest passed — %zu ids, %d threads, all unique, "
                "non-zero, per-thread monotonic\n",
                seen.size(), kThreads);
    return 0;
}
