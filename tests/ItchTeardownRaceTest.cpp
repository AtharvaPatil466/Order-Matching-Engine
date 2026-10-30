// ItchTeardownRaceTest — stopAsyncPublishing() while a producer is publishing.
//
// Roadmap 1.7-G2 (LF-5). stopAsyncPublishing() joined the worker, drained the
// ring and reset it with nothing to stop a producer that had already seen
// async mode from pushing into it: the push landed in freed memory (ASan), or
// after the final drain, where the event was silently lost; and a producer
// that saw async turned off serialized inline while the drain was still
// running on the stopping thread. The fix is an admission gate in front of
// the ring: teardown closes it and waits out producers already inside, and a
// producer that arrives mid-teardown waits for it to finish, then publishes
// inline like any sync-mode event.
//
// Every event the producer publishes must reach the sink exactly once (or be
// counted as dropped on a full ring). Run under ASan and TSan.
//
// Rounds run on several publishers at once so the CPUs are oversubscribed: the
// races below need a thread preempted between two of the gate's atomics, which
// an idle machine almost never does. A producer that read async mode on, then
// slept through a whole teardown, pushed into the freed ring (SIGSEGV); one
// that slept between its two gate RMWs while teardown reset the gate wrapped
// the counter into "closed" and spun forever (CI timeout).

#include "ItchPublisher.h"
#include "OrderBook.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

using namespace OrderMatcher;

namespace {

constexpr int kRounds = 300;
constexpr uint64_t kEventsBeforeStop = 100;
constexpr uint64_t kEventsAfterStop = 100;

bool runRound(int round) {
    // On the heap: the book is too big for a secondary thread's stack.
    const auto book = std::make_unique<OrderBook>(1);
    std::atomic<uint64_t> frames{0};
    ItchPublisher pub(*book, [&](std::string_view) {
        frames.fetch_add(1, std::memory_order_relaxed);
    });
    pub.enableAsyncPublishing(1 << 10);

    std::atomic<uint64_t> published{0};
    std::atomic<bool> stopped{false};
    std::thread producer([&] {
        uint64_t afterStop = 0;
        while (afterStop < kEventsAfterStop) {
            pub.publishSystemEvent('Q');
            published.fetch_add(1, std::memory_order_relaxed);
            if (stopped.load(std::memory_order_acquire)) ++afterStop;
        }
    });
    while (published.load(std::memory_order_relaxed) < kEventsBeforeStop) {
        std::this_thread::yield();
    }
    pub.stopAsyncPublishing();
    stopped.store(true, std::memory_order_release);
    producer.join();

    // A drop is followed by a resync (MD-9): its marker is a frame no producer
    // published. With no orders resting, a resync is that one frame. Events
    // published while stop joins the worker can fill the ring and drop.
    const uint64_t delivered = frames.load() - pub.resyncsEmitted() + pub.droppedEvents();
    if (delivered != published.load()) {
        std::fprintf(stderr, "round %d: published %llu, sent or dropped %llu\n", round,
                     static_cast<unsigned long long>(published.load()),
                     static_cast<unsigned long long>(delivered));
        return false;
    }
    return true;
}

}  // namespace

int main() {
    const int groups = static_cast<int>(std::max(4u, 2 * std::thread::hardware_concurrency()));
    std::atomic<int> next{0};
    std::atomic<bool> failed{false};
    std::vector<std::thread> runners;
    for (int g = 0; g < groups; ++g) {
        runners.emplace_back([&] {
            for (int round = next++; round < kRounds; round = next++) {
                if (!runRound(round)) failed = true;
            }
        });
    }
    for (std::thread& t : runners) t.join();
    if (failed) std::abort();
    std::printf("ItchTeardownRaceTest passed (%d rounds, %d at a time)\n", kRounds, groups);
    return 0;
}
