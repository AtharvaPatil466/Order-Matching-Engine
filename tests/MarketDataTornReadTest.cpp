// MarketDataTornReadTest — a subscriber the writer laps must never accept a
// torn entry.
//
// WHAT WAS WRONG. MarketDataPublisher writes an entry's payload and then its
// sequence; MarketDataSubscriber copies the whole entry and then checks the
// copied sequence. The sequence is the first thing the copy reads, so a writer
// that laps the reader mid-copy leaves the OLD sequence next to the NEW payload,
// and the check passes. The subscriber hands out an entry half one update, half
// another — a price from one level and a quantity from the next.
//
// HOW IT IS OBSERVED. One thread publishes into a 4-slot ring as fast as it can;
// every field of update N is derived from N. Another thread polls. Any accepted
// entry whose fields disagree with its own sequence is torn. A tiny ring makes
// the writer lap the reader constantly; the old code tore about a third of the
// entries it accepted. The writer runs until the reader has accepted
// MIN_ACCEPTED entries (a correct reader is lapped often, so it accepts few),
// and the assertion has no timing in it — a correct ring passes however the
// threads interleave.

#include "MarketDataPublisher.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <string>
#include <thread>
#include <unistd.h>

using namespace OrderMatcher;

namespace {

constexpr uint64_t MIN_PUBLISHES = 1'000'000;
constexpr uint64_t MIN_ACCEPTED = 2'000;
constexpr size_t RING_SLOTS = 16;

MarketDataUpdate updateFor(uint64_t n) {
    MarketDataUpdate u{};
    u.action = MarketDataUpdate::Action::Modify;
    u.side = (n & 1) ? Side::Sell : Side::Buy;
    u.level.price = static_cast<Price>(n);
    u.level.totalQuantity = static_cast<Quantity>(n);
    u.level.orderCount = static_cast<uint32_t>(n);
    u.timestamp = n;
    u.sequenceNumber = n;
    return u;
}

bool isWhole(const ShmEntry& e) {
    const uint64_t n = e.update.sequenceNumber;
    return e.sequence == n &&
           e.update.level.price == static_cast<Price>(n) &&
           e.update.level.totalQuantity == static_cast<Quantity>(n) &&
           e.update.level.orderCount == static_cast<uint32_t>(n) &&
           e.update.timestamp == n &&
           e.update.side == ((n & 1) ? Side::Sell : Side::Buy);
}

}  // namespace

int main() {
    const std::string name = "md_torn_" + std::to_string(getpid());
    MarketDataPublisher pub(name, RING_SLOTS);
    assert(pub.start());
    MarketDataSubscriber sub(name);
    assert(sub.connect());

    std::atomic<bool> done{false};
    std::atomic<uint64_t> accepted{0};
    std::thread writer([&] {
        for (uint64_t n = 0; n < MIN_PUBLISHES ||
                             accepted.load(std::memory_order_relaxed) < MIN_ACCEPTED; ++n)
            pub.publishUpdate(updateFor(n));
        done.store(true, std::memory_order_release);
    });

    uint64_t torn = 0;
    ShmEntry e{};
    while (!done.load(std::memory_order_acquire)) {
        if (!sub.poll(e)) continue;
        accepted.fetch_add(1, std::memory_order_relaxed);
        if (!isWhole(e)) {
            if (torn == 0)
                std::printf("  first torn entry: sequence=%llu but sequenceNumber=%llu "
                            "price=%lld qty=%llu\n",
                            static_cast<unsigned long long>(e.sequence),
                            static_cast<unsigned long long>(e.update.sequenceNumber),
                            static_cast<long long>(e.update.level.price),
                            static_cast<unsigned long long>(e.update.level.totalQuantity));
            ++torn;
        }
    }
    writer.join();

    std::printf("  accepted %llu entries, %llu torn\n",
                static_cast<unsigned long long>(accepted.load()),
                static_cast<unsigned long long>(torn));
    assert(torn == 0 && "the subscriber accepted a torn entry");

    sub.disconnect();
    pub.stop();
    std::puts("MarketDataTornReadTest passed");
    return 0;
}
