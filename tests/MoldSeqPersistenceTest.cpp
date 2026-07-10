// MoldSeqPersistenceTest — P3-3: MoldUDP64 sequence continuity across a
// restart.
//
// A restarted publisher must NOT reset its sequence counter to 1. A
// subscriber that last held seq N would then be re-fed a 1..N range it
// already delivered and, worse, could interpret the venue as having
// rewound — a false gap / broken continuity. The fix persists the
// high-water mark on every group commit (flush / end-of-session) via a
// durable sink (EpochStore) and reloads it on restart.
//
// This mirrors spec/EpochDurability.tla: persist-on-commit +
// reload-on-restart, with the persisted value strictly monotonic
// (never regresses on disk), so "resume above the last committed
// sequence" holds across crashes.
//
// Coverage:
//   1. Each flush persists the next-to-publish high-water durably.
//   2. After a mid-session crash, a restart resumes the counter from the
//      persisted value (packets continue at N, not 1).
//   3. A subscriber fed the pre-crash AND post-restart packets sees a
//      contiguous stream — zero gaps across the restart boundary.
//   4. Control (the bug): a restart that ignores the persisted value
//      rewinds to seq 1, which a caught-up subscriber drops as stale —
//      demonstrating the false-continuity the persistence prevents.
//   5. The persisted value is monotonic across further commits.

#include "EpochStore.h"
#include "MoldUDP64.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <unistd.h>
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

static std::string tmpPath() {
    return "/tmp/mold_seq_persist_" + std::to_string(::getpid()) + ".bin";
}

// Header sequence number of a captured MoldUDP64 packet.
static uint64_t headerSeq(const std::vector<uint8_t>& pkt) {
    MoldHeader hdr;
    CHECK(moldReadHeader(pkt.data(), pkt.size(), hdr));
    return hdr.sequenceNumber;
}

void test_SequenceResumesAcrossRestart() {
    TEST(SequenceResumesAcrossRestart) {
        const std::string path = tmpPath();
        std::remove(path.c_str());
        EpochStore store(path);

        std::vector<std::vector<uint8_t>> allPackets;
        auto capture = [&](std::string_view b) {
            allPackets.emplace_back(b.begin(), b.end());
        };
        auto persist = [&](uint64_t hw) { store.store(hw); };

        // ── Session 1: publish three single-message packets. ──
        {
            MoldUDP64Publisher pub("SESS", capture);
            pub.setSequencePersistence(persist);
            CHECK(store.load() == 0);  // fresh node

            pub.addMessage("a", 1); pub.flush();   // seq 1 → persist 2
            pub.addMessage("b", 1); pub.flush();   // seq 2 → persist 3
            pub.addMessage("c", 1); pub.flush();   // seq 3 → persist 4
            // "Crash": drop the publisher without an orderly shutdown.
        }
        CHECK(store.load() == 4
              && "high-water = next-to-publish must survive the crash");
        CHECK(allPackets.size() == 3);
        CHECK(headerSeq(allPackets[0]) == 1);
        CHECK(headerSeq(allPackets[2]) == 3);

        // ── Session 2 (restart): resume from the persisted high-water. ──
        {
            MoldUDP64Publisher pub2("SESS", capture);
            uint64_t resume = store.load();
            CHECK(resume == 4 && "restart must load the persisted counter");
            pub2.setNextSequence(resume);        // resume, do NOT reset to 1
            pub2.setSequencePersistence(persist);

            pub2.addMessage("d", 1); pub2.flush();  // seq 4 → persist 5
            pub2.addMessage("e", 1); pub2.flush();  // seq 5 → persist 6
        }
        CHECK(allPackets.size() == 5);
        CHECK(headerSeq(allPackets[3]) == 4
              && "first post-restart packet continues at 4, not 1");
        CHECK(headerSeq(allPackets[4]) == 5);
        CHECK(store.load() == 6);

        // ── A reconnecting subscriber must see NO gap across the seam. ──
        MoldUDP64Subscriber sub;
        sub.setExpectedSession("SESS");
        uint64_t delivered = 0;
        sub.setOnMessage([&](uint64_t, const uint8_t*, size_t) { ++delivered; });
        for (const auto& p : allPackets) {
            CHECK(sub.feedPacket(p.data(), p.size()));
        }
        CHECK(delivered == 5);
        CHECK(sub.gapsObserved() == 0
              && "contiguous 1..5 across the restart → zero false gaps");
        CHECK(sub.nextExpectedSequence() == 6);

        std::remove(path.c_str());
    } END
}

void test_BrokenRestartRewindsAndStalls() {
    TEST(BrokenRestartRewindsAndStalls) {
        // The bug this guards against: a restart that ignores the
        // persisted counter restarts at seq 1. A subscriber already at
        // seq 4 treats the rewound 1..3 as stale duplicates (seq < front)
        // and never advances — the feed appears frozen. Non-vacuity
        // control for the fix above (cf. EpochDurabilityBroken.cfg).
        std::vector<std::vector<uint8_t>> pre, post;

        {
            MoldUDP64Publisher pub("SESS", [&](std::string_view b) {
                pre.emplace_back(b.begin(), b.end());
            });
            pub.addMessage("a", 1); pub.flush();  // seq 1
            pub.addMessage("b", 1); pub.flush();  // seq 2
            pub.addMessage("c", 1); pub.flush();  // seq 3
        }
        {
            // Restart WITHOUT restoring the counter — resets to 1.
            MoldUDP64Publisher pub2("SESS", [&](std::string_view b) {
                post.emplace_back(b.begin(), b.end());
            });
            pub2.addMessage("d", 1); pub2.flush();  // seq 1 again (WRONG)
        }
        CHECK(headerSeq(post[0]) == 1
              && "no-restore restart wrongly rewinds to 1");

        MoldUDP64Subscriber sub;
        sub.setExpectedSession("SESS");
        uint64_t delivered = 0;
        sub.setOnMessage([&](uint64_t, const uint8_t*, size_t) { ++delivered; });
        for (const auto& p : pre)  CHECK(sub.feedPacket(p.data(), p.size()));
        CHECK(delivered == 3);
        CHECK(sub.nextExpectedSequence() == 4);

        // Feed the rewound post-restart packet: seq 1 < front 4 → dropped
        // as a stale duplicate. The subscriber does NOT advance — proof of
        // the broken continuity that persistence prevents.
        CHECK(sub.feedPacket(post[0].data(), post[0].size()));
        CHECK(delivered == 3 && "rewound packet delivers nothing new");
        CHECK(sub.nextExpectedSequence() == 4);
    } END
}

void test_PersistedHighWaterIsMonotonic() {
    TEST(PersistedHighWaterIsMonotonic) {
        const std::string path = tmpPath();
        std::remove(path.c_str());
        EpochStore store(path);

        uint64_t last = 0;
        bool monotonic = true;
        MoldUDP64Publisher pub("SESS", [](std::string_view) {});
        pub.setSequencePersistence([&](uint64_t hw) {
            if (hw <= last) monotonic = false;   // strictly increasing
            last = hw;
            store.store(hw);
        });

        for (int i = 0; i < 32; ++i) {
            pub.addMessage("x", 1);
            pub.flush();
        }
        CHECK(monotonic && "persisted high-water must never regress");
        CHECK(store.load() == 33);   // 32 msgs from seq 1 → next-to-publish 33
        CHECK(last == 33);

        std::remove(path.c_str());
    } END
}

int main() {
    std::cout << "Running MoldSeqPersistenceTest\n";
    test_SequenceResumesAcrossRestart();
    test_BrokenRestartRewindsAndStalls();
    test_PersistedHighWaterIsMonotonic();
    std::cout << "\n" << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
