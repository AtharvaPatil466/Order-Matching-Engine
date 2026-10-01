// JournalImmediateDurableTest — under SyncPolicy::Immediate, an entry is durable
// by the time the append that wrote it RETURNS. On every commit path.
//
// WHY THIS IS A CONTRACT AND NOT A DETAIL. The order-entry binary sends a
// client its Ack after submitOrder returns, and submitOrder returns after the
// journal append. So "the ack means the order is on disk" is exactly as true as
// "Immediate is durable when the append returns". The synchronous commit path
// always did that: it writes, fdatasyncs and fires onDurable inline. The io_uring
// path — taken by any Linux build with liburing, though CI has no liburing, the
// shipped image does not install it, and Docker's default seccomp blocks it, so
// until now no environment ran it — did NOT. It
// submitted the write and the fsync, popped the batch and returned, and a reaper
// thread finished the chain later. So on such a build the Ack could leave the process
// before the fsync did, and kill -9 in that window loses an acknowledged order.
// Every environment that actually runs this code uses the synchronous path, so
// the order-entry binary's acks were durable in practice; this closes the gap
// for any build that does take the io_uring path.
//
// It also mattered for correctness, not just durability. DurabilityGate — which
// holds fills until they are durable — has no lock; it relies on being touched
// by one thread. On io_uring, onDurable runs on the reaper, concurrently with the
// writer's next capture. Waiting for the chain makes the reaper's release happen
// while the writer is blocked, and the chain-done condition orders it before the
// writer resumes.
//
// WHAT IS PINNED. After each Immediate append returns, onDurable has already
// reported that entry. On macOS and on Linux without liburing this held before
// the fix too — those use the synchronous path — so the regression it guards
// against is Linux io_uring specifically, which is where it was found.

#include "Journal.h"
#include "TempPath.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

using namespace OrderMatcher;

int main() {
    const std::string path = uniqueTempPath("ob_journal_immediate.wal");
    std::remove(path.c_str());

    constexpr int kEntries = 20;
    int behind = 0;
    {
        Journal journal(path, Journal::SyncPolicy::Immediate, 1);
        std::atomic<uint64_t> durable{0};   // the reaper writes it on io_uring
        journal.setOnDurable([&](size_t n) {
            durable.fetch_add(n, std::memory_order_release);
        });

        for (int i = 1; i <= kEntries; ++i) {
            journal.logAddOrder(static_cast<OrderId>(i), 7, 1, Side::Buy, 10000, 10,
                                OrderType::Limit);
            // Checked the instant the append returns — no flush, no wait. An
            // fsync that is still in flight shows up here as a shortfall.
            if (durable.load(std::memory_order_acquire) < static_cast<uint64_t>(i)) ++behind;
        }
        journal.flush();
        assert(durable.load() == static_cast<uint64_t>(kEntries) &&
               "not every entry was ever reported durable");
    }
    std::remove(path.c_str());

    std::printf("appends returning before their entry was durable: %d of %d\n", behind,
                kEntries);
    assert(behind == 0 &&
           "an Immediate append returned before its entry was durable — the order-"
           "entry binary sends its Ack right after, so that Ack is not durable");

    std::puts("JournalImmediateDurableTest passed");
    return 0;
}
