// MoldPublishThreadTest — the single-publisher-thread invariant on
// MoldUDP64Publisher, the one that lets nextSeq_ stay a plain uint64_t.
//
// The rule (MoldUDP64.h, assertPublishThread()): every sequence-advancing call
// on ONE publisher instance comes from ONE thread. It is latched on the first
// such call rather than at construction, because the shipped wiring
// legitimately constructs and publishes on different threads —
// ItchUdpPublisher::start() news the publisher on the thread bringing the feed
// up, while addMessage()/flush() arrive from the book's worker thread.
//
// Coverage:
//   1. Single-threaded publishing is untouched — sequences stay contiguous
//      and every counter matches, check or no check.
//   2. Construct on thread A, publish entirely on thread B. This is the real
//      shape (ItchUdpTransport.h:71 vs MatchingEngine.cpp:264) and it must
//      pass; it is the reason the latch is on first publish.
//   3. Heartbeat and end-of-session count as sequenced calls and are happy on
//      the owning thread.
//   4. DEATH TEST (assertions-on builds only): once a thread owns the
//      publisher, a second thread advancing the sequence aborts instead of
//      silently corrupting the wire.
//
// On (4): an assert aborts the process, so it is checked by fork()ing and
// waiting for SIGABRT rather than by any in-process mechanism. The child's
// stderr goes to /dev/null so the deliberate assert message does not read like
// a failure in the log. Under NDEBUG the check does not exist, so the case is
// compiled out rather than made to pass vacuously.

#include "MoldUDP64.h"

#include <cassert>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

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

// Pull the sequence number out of a captured MoldUDP64 packet.
static uint64_t seqOf(const std::string& packet) {
    MoldHeader hdr;
    if (!moldReadHeader(reinterpret_cast<const uint8_t*>(packet.data()),
                        packet.size(), hdr)) {
        throw std::runtime_error("unparseable packet");
    }
    return hdr.sequenceNumber;
}

// ─── 1. The ordinary case still works ───────────────────────────────────────

void test_SingleThreadedPublishingUnaffected() {
    TEST(SingleThreadedPublishingUnaffected) {
        std::vector<std::string> packets;
        MoldUDP64Publisher pub("SESS", [&](std::string_view b) {
            packets.emplace_back(b);
        });

        const char body[] = "abcd";
        for (int i = 0; i < 5; ++i) {
            uint64_t assigned = pub.addMessage(body, 4);
            CHECK(assigned == static_cast<uint64_t>(i) + 1);
            pub.flush();
        }

        CHECK(packets.size() == 5);
        for (size_t i = 0; i < packets.size(); ++i) {
            CHECK(seqOf(packets[i]) == i + 1);
        }
        CHECK(pub.packetsEmitted() == 5);
        CHECK(pub.nextSequence() == 6);
        CHECK(pub.pendingCount() == 0);
    } END
}

// ─── 2. Construct here, publish over there — the shipped wiring ─────────────

void test_ConstructOnOneThreadPublishOnAnother() {
    TEST(ConstructOnOneThreadPublishOnAnother) {
        // Constructed on the main thread, exactly as ItchUdpPublisher::start()
        // does it (ItchUdpTransport.h:71).
        std::vector<std::string> packets;
        MoldUDP64Publisher pub("SESS", [&](std::string_view b) {
            packets.emplace_back(b);
        });
        CHECK(pub.nextSequence() == 1);

        // Every sequence-advancing call comes from the worker instead. The
        // worker is the owner; the main thread never advanced anything.
        std::string caught;
        std::thread worker([&] {
            try {
                const char body[] = "wire";
                for (int i = 0; i < 3; ++i) {
                    pub.addMessage(body, 4);
                    pub.flush();
                }
                pub.sendHeartbeat();
            } catch (const std::exception& e) {
                caught = e.what();
            }
        });
        worker.join();

        CHECK(caught.empty());
        CHECK(packets.size() == 4);          // 3 data packets + 1 heartbeat
        CHECK(seqOf(packets[0]) == 1);
        CHECK(seqOf(packets[2]) == 3);
        CHECK(seqOf(packets[3]) == 4);       // heartbeat carries next-expected
        CHECK(pub.nextSequence() == 4);
        CHECK(pub.heartbeatsEmitted() == 1);
    } END
}

// ─── 3. Heartbeat / EOS are sequenced calls too ─────────────────────────────

void test_SpecialPacketsRunOnTheOwningThread() {
    TEST(SpecialPacketsRunOnTheOwningThread) {
        std::vector<std::string> packets;
        std::string caught;
        std::thread owner([&] {
            try {
                MoldUDP64Publisher pub("SESS", [&](std::string_view b) {
                    packets.emplace_back(b);
                });
                const char body[] = "xy";
                pub.addMessage(body, 2);
                pub.flush();
                pub.sendHeartbeat();
                pub.sendEndOfSession();
                CHECK(pub.heartbeatsEmitted() == 1);
                CHECK(pub.endOfSessionsEmitted() == 1);
            } catch (const std::exception& e) {
                caught = e.what();
            }
        });
        owner.join();

        CHECK(caught.empty());
        CHECK(packets.size() == 3);
    } END
}

// ─── 4. The violation aborts (assertions-on builds) ─────────────────────────

#ifndef NDEBUG
// Runs in the forked child; never returns on a working check.
static void childViolatesInvariant() {
    // Silence the deliberate assert message — it is expected output, and a
    // stack of it in the test log reads like a real failure.
    int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        ::dup2(devnull, STDERR_FILENO);
        ::dup2(devnull, STDOUT_FILENO);
        ::close(devnull);
    }

    static std::string sink;
    MoldUDP64Publisher pub("SESS", [](std::string_view b) { sink.append(b); });

    // This thread claims the publisher.
    const char body[] = "own";
    pub.addMessage(body, 3);
    pub.flush();

    // A second publish site appears later — the exact mistake the invariant
    // exists to catch. Must abort, not quietly renumber the feed.
    std::thread intruder([&] {
        const char other[] = "bad";
        pub.addMessage(other, 3);
        pub.flush();
    });
    intruder.join();

    ::_exit(0);   // reached only if the check failed to fire
}

void test_SecondThreadAdvancingSequenceAborts() {
    TEST(SecondThreadAdvancingSequenceAborts) {
        pid_t pid = ::fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            childViolatesInvariant();
            ::_exit(0);
        }
        int status = 0;
        CHECK(::waitpid(pid, &status, 0) == pid);
        CHECK(WIFSIGNALED(status));
        CHECK(WTERMSIG(status) == SIGABRT);
    } END
}
#endif  // NDEBUG

int main() {
    std::cout << "Running MoldPublishThreadTest\n";

    test_SingleThreadedPublishingUnaffected();
    test_ConstructOnOneThreadPublishOnAnother();
    test_SpecialPacketsRunOnTheOwningThread();
#ifndef NDEBUG
    test_SecondThreadAdvancingSequenceAborts();
#else
    std::cout << "  SecondThreadAdvancingSequenceAborts... skipped (NDEBUG)\n";
#endif

    std::cout << "\n" << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
