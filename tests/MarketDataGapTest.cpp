// MarketDataGapTest — a shared-memory subscriber is told when it lost entries
// and when the publisher it was reading went away.
//
// WHAT WAS WRONG (MD-12).
//   - Lapped: poll() skipped the subscriber to the oldest entry still in the
//     ring and returned it as if it were next. A subscriber that fell
//     `capacity` behind lost updates and carried on applying deltas to a book
//     that no longer matched.
//   - Restarted: a restarted publisher rewound writeSeq to 0 on the same
//     segment. The subscriber, still at its old position, saw readSeq >=
//     writeSeq and returned "nothing new" forever — or, once the new session
//     passed its old position, delivered the new session's entries as a
//     continuation of the old one.
//   - On macOS a restarted publisher could not even start: it reopened the
//     crashed one's segment and its second ftruncate failed with EINVAL.
//
// The header now carries the publisher's session epoch and poll() returns a
// PollResult: Gap when entries were lost, Reset when the epoch changed.

#include "MarketDataPublisher.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace OrderMatcher;

namespace {

MarketDataUpdate updateFor(uint64_t n) {
    MarketDataUpdate u{};
    u.action = MarketDataUpdate::Action::Add;
    u.side = Side::Buy;
    u.level.price = static_cast<Price>(n);
    u.sequenceNumber = n;
    return u;
}

std::string uniqueName(const char* tag) {
    return std::string("mdgap_") + tag + "_" + std::to_string(getpid());
}

void expectEntry(MarketDataSubscriber& sub, uint64_t sequence, uint64_t payload) {
    ShmEntry e{};
    assert(sub.poll(e) == PollResult::Entry);
    assert(e.sequence == sequence);
    assert(e.update.sequenceNumber == payload);
}

void lappedSubscriberIsToldItLostEntries() {
    const auto name = uniqueName("lap");
    MarketDataPublisher pub(name, 4);
    assert(pub.start());
    MarketDataSubscriber sub(name);
    assert(sub.connect());

    for (uint64_t n = 0; n < 10; ++n) pub.publishUpdate(updateFor(n));

    ShmEntry e{};
    assert(sub.poll(e) == PollResult::Gap && "entries 0-5 were overwritten unread");
    assert(sub.readSequence() == 6);
    for (uint64_t n = 6; n < 10; ++n) expectEntry(sub, n, n);
    assert(sub.poll(e) == PollResult::Empty);
    std::puts("  lapped subscriber gets Gap, then the oldest entry left: ok");
}

void crashRestartedPublisherResetsSubscriber() {
    const auto name = uniqueName("crash");

    // The first publisher dies without stop(): no unlink, no epoch clear.
    const pid_t child = fork();
    if (child == 0) {
        auto* crashed = new MarketDataPublisher(name, 8);
        if (!crashed->start()) _exit(1);
        for (uint64_t n = 0; n < 3; ++n) crashed->publishUpdate(updateFor(100 + n));
        _exit(0);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);

    MarketDataSubscriber sub(name);
    assert(sub.connect());
    for (uint64_t n = 0; n < 3; ++n) expectEntry(sub, n, 100 + n);

    MarketDataPublisher restarted(name, 8);
    assert(restarted.start() && "a publisher restarted after a crash must be able to start");
    restarted.publishUpdate(updateFor(200));

    ShmEntry e{};
    assert(sub.poll(e) == PollResult::Reset && "the subscriber must be told the session changed");
    assert(sub.poll(e) == PollResult::Empty && "a reset subscriber reads nothing until it reconnects");

    assert(sub.connect());
    expectEntry(sub, 0, 200);
    assert(sub.poll(e) == PollResult::Empty);
    std::puts("  crash-restarted publisher: Reset, then the new session from 0: ok");
}

void stoppedPublisherResetsSubscriber() {
    const auto name = uniqueName("stop");
    MarketDataPublisher pub(name, 8);
    assert(pub.start());
    MarketDataSubscriber sub(name);
    assert(sub.connect());
    pub.publishUpdate(updateFor(1));
    expectEntry(sub, 0, 1);

    pub.stop();

    ShmEntry e{};
    assert(sub.poll(e) == PollResult::Reset && "a stopped publisher must not look like a quiet one");
    std::puts("  stopped publisher: Reset: ok");
}

}  // namespace

int main() {
    lappedSubscriberIsToldItLostEntries();
    crashRestartedPublisherResetsSubscriber();
    stoppedPublisherResetsSubscriber();
    std::puts("MarketDataGapTest passed");
    return 0;
}
