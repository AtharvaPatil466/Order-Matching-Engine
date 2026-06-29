// ItchRetransmissionTest — exercises the gap-recovery loop.
//
//   Publisher → MoldUDP64 → Subscriber observes gap
//                            ↓
//                       connect to RetransmissionService
//                            ↓
//                       re-request missing sequence range
//                            ↓
//                       receive replayed messages via SoupBinTCP
//
// Coverage:
//   Journal
//     1. Record + replay-range delivers each message once
//     2. Eviction (ring overflow) drops the oldest entries
//     3. Replay of empty range returns 0
//     4. Replay of an entirely-evicted range returns 0
//     5. contains() reflects journal window
//
//   Re-request packet
//     6. Build/parse roundtrip
//     7. Parse rejects wrong tag / wrong length
//
//   Service (real TCP loopback)
//     8. Bind + accept
//     9. Login + send re-request + receive replayed messages
//    10. Empty journal → no replay messages, request count incremented
//    11. Range entirely missing → no replay messages

#include "ItchRetransmissionService.h"
#include "MoldPacketJournal.h"
#include "OuchProtocol.h"
#include "SoupBinTCP.h"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <thread>
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

// ─── Journal tests ──────────────────────────────────────────────────────────

void test_JournalRecordAndReplay() {
    TEST(JournalRecordAndReplay) {
        MoldPacketJournal j(16);
        j.record(1, "A", 1);
        j.record(2, "BB", 2);
        j.record(3, "CCC", 3);

        std::vector<std::string> got;
        size_t n = j.replayRange(1, 3,
            [&](uint64_t /*seq*/, const uint8_t* p, size_t len) {
                got.emplace_back(reinterpret_cast<const char*>(p), len);
            });
        CHECK(n == 3);
        CHECK(got.size() == 3);
        CHECK(got[0] == "A");
        CHECK(got[1] == "BB");
        CHECK(got[2] == "CCC");
    } END
}

void test_JournalEvictsOldest() {
    TEST(JournalEvictsOldest) {
        MoldPacketJournal j(/*maxEntries=*/2);
        j.record(1, "A", 1);
        j.record(2, "B", 1);
        j.record(3, "C", 1);    // evicts seq=1

        CHECK(j.size() == 2);
        CHECK(j.lowestSeq() == 2);
        CHECK(j.highestSeq() == 3);
        CHECK(!j.contains(1));
        CHECK(j.contains(2));
    } END
}

void test_JournalReplayEmptyRange() {
    TEST(JournalReplayEmptyRange) {
        MoldPacketJournal j;
        j.record(1, "A", 1);
        size_t n = j.replayRange(10, 5,
            [](uint64_t, const uint8_t*, size_t) {});
        CHECK(n == 0 && "no entries in [10,15) → 0 delivered");
    } END
}

void test_JournalReplayEvictedRange() {
    TEST(JournalReplayEvictedRange) {
        MoldPacketJournal j(2);
        j.record(1, "A", 1);
        j.record(2, "B", 1);
        j.record(3, "C", 1);

        // Request seq=1 — already evicted.
        size_t n = j.replayRange(1, 1,
            [](uint64_t, const uint8_t*, size_t) {});
        CHECK(n == 0 && "evicted seq must NOT replay");
    } END
}

void test_JournalContainsReflectsWindow() {
    TEST(JournalContainsReflectsWindow) {
        MoldPacketJournal j;
        CHECK(!j.contains(1));
        j.record(5, "X", 1);
        CHECK(j.contains(5));
        CHECK(!j.contains(6));
        CHECK(!j.contains(4));
    } END
}

void test_JournalReplayCountZeroNearUint64Max() {
    TEST(JournalReplayCountZeroNearUint64Max) {
        // A journal whose newest sequence sits at the very top of the
        // 64-bit space. A "to end" (count==0) replay must still emit the
        // journaled entries: computing endSeq = back().seq + 1 wraps to
        // 0 when back().seq == UINT64_MAX, which would make the half-open
        // window [startSeq, 0) empty and silently replay nothing.
        MoldPacketJournal j(16);
        j.record(UINT64_MAX - 2, "A", 1);
        j.record(UINT64_MAX - 1, "B", 1);
        j.record(UINT64_MAX,     "C", 1);

        std::vector<std::string> got;
        size_t n = j.replayRange(UINT64_MAX - 2, /*count=*/0,
            [&](uint64_t, const uint8_t* p, size_t len) {
                got.emplace_back(reinterpret_cast<const char*>(p), len);
            });
        CHECK(n == 3 && "count==0 replay near UINT64_MAX must not wrap to empty");
        CHECK(got.size() == 3);
        CHECK(got[0] == "A");
        CHECK(got[1] == "B");
        CHECK(got[2] == "C");
    } END
}

void test_JournalReplayCountWrapGuard() {
    TEST(JournalReplayCountWrapGuard) {
        // startSeq + count must not wrap: a request near the top of the
        // sequence space with a large count must clamp to UINT64_MAX
        // rather than fold back into a tiny [startSeq, small) window that
        // delivers nothing.
        MoldPacketJournal j(16);
        j.record(UINT64_MAX - 1, "A", 1);
        j.record(UINT64_MAX,     "B", 1);

        std::vector<std::string> got;
        size_t n = j.replayRange(UINT64_MAX - 1, /*count=*/50000,
            [&](uint64_t, const uint8_t* p, size_t len) {
                got.emplace_back(reinterpret_cast<const char*>(p), len);
            });
        CHECK(n == 2 && "count window must not wrap past UINT64_MAX");
        CHECK(got.size() == 2);
        CHECK(got[0] == "A");
        CHECK(got[1] == "B");
    } END
}

// ─── Re-request packet tests ────────────────────────────────────────────────

void test_RetransmitRequestRoundtrip() {
    TEST(RetransmitRequestRoundtrip) {
        uint8_t buf[ITCH_RETRANSMIT_REQUEST_BYTES];
        size_t n = buildRetransmitRequest(buf, /*startSeq=*/100,
                                           /*count=*/50);
        CHECK(n == ITCH_RETRANSMIT_REQUEST_BYTES);

        ItchRetransmitRequest req;
        CHECK(parseRetransmitRequest(buf, n, req));
        CHECK(req.startSeq == 100);
        CHECK(req.count == 50);
    } END
}

void test_RetransmitRequestRejectsBadTag() {
    TEST(RetransmitRequestRejectsBadTag) {
        uint8_t buf[ITCH_RETRANSMIT_REQUEST_BYTES] = {0};
        buf[0] = 'X';   // not 'R'
        ItchRetransmitRequest req;
        CHECK(!parseRetransmitRequest(buf, sizeof(buf), req));
    } END
}

void test_RetransmitRequestRejectsBadLength() {
    TEST(RetransmitRequestRejectsBadLength) {
        uint8_t buf[5] = {'R', 0, 0, 0, 0};
        ItchRetransmitRequest req;
        CHECK(!parseRetransmitRequest(buf, sizeof(buf), req));
    } END
}

// ─── Service tests (real TCP loopback) ──────────────────────────────────────

namespace {

int tcpConnect(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd); return -1;
    }
    return fd;
}

bool sendAll(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        ssize_t n = ::send(fd, p, len, 0);
        if (n <= 0) return false;
        p += n; len -= static_cast<size_t>(n);
    }
    return true;
}

bool recvAll(int fd, void* out, size_t len, int deadlineMs = 2000) {
    char* p = static_cast<char*>(out);
    auto start = std::chrono::steady_clock::now();
    while (len > 0) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed > deadlineMs) return false;
        timeval tv{0, 100 * 1000};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ssize_t n = ::recv(fd, p, len, 0);
        if (n > 0) { p += n; len -= static_cast<size_t>(n); continue; }
        if (n == 0) return false;
    }
    return true;
}

std::vector<uint8_t> recvSoupPacket(int fd) {
    uint8_t lenBuf[2];
    if (!recvAll(fd, lenBuf, 2)) return {};
    uint16_t len = readU16BE(lenBuf);
    if (len == 0) return {};
    std::vector<uint8_t> packet(2 + len);
    std::memcpy(packet.data(), lenBuf, 2);
    if (!recvAll(fd, packet.data() + 2, len)) return {};
    return packet;
}

std::vector<uint8_t> buildLogin(const std::string& u, const std::string& p) {
    std::vector<uint8_t> wire(3 + SOUP_SIZE_LOGIN_REQUEST_PAYLOAD);
    soupWriteLoginRequest(wire.data(), u, p, "", 0);
    return wire;
}

std::vector<uint8_t> buildLoginSession(const std::string& u, const std::string& p,
                                       const std::string& session) {
    std::vector<uint8_t> wire(3 + SOUP_SIZE_LOGIN_REQUEST_PAYLOAD);
    soupWriteLoginRequest(wire.data(), u, p, session, 0);
    return wire;
}

}  // namespace

void test_ServiceBindsAndAccepts() {
    TEST(ServiceBindsAndAccepts) {
        MoldPacketJournal j;
        ItchRetransmissionService svc(j, "REPLAY1");
        CHECK(svc.start(0));
        CHECK(svc.boundPort() != 0);

        int fd = tcpConnect(svc.boundPort());
        CHECK(fd >= 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ::close(fd);
        svc.stop();
        CHECK(!svc.isRunning());
    } END
}

void test_ServiceReplaysJournaledMessages() {
    TEST(ServiceReplaysJournaledMessages) {
        MoldPacketJournal j;
        j.record(1, "MSG_A", 5);
        j.record(2, "MSG_B", 5);
        j.record(3, "MSG_C", 5);

        ItchRetransmissionService svc(j, "REPLAY");
        CHECK(svc.start(0));

        int fd = tcpConnect(svc.boundPort());
        CHECK(fd >= 0);

        // 1. Login.
        auto login = buildLogin("SUB", "PASS");
        CHECK(sendAll(fd, login.data(), login.size()));
        auto loginAck = recvSoupPacket(fd);
        CHECK(loginAck.size() == 3 + SOUP_SIZE_LOGIN_ACCEPTED_PAYLOAD);
        CHECK(loginAck[2] == SOUP_PT_LOGIN_ACCEPTED);

        // 2. Send re-request for seqs 1..3 wrapped in SoupBinTCP
        //    Unsequenced.
        uint8_t inner[ITCH_RETRANSMIT_REQUEST_BYTES];
        buildRetransmitRequest(inner, /*startSeq=*/1, /*count=*/3);
        uint8_t req[3 + ITCH_RETRANSMIT_REQUEST_BYTES];
        soupWriteEnvelope(req, SOUP_PT_UNSEQUENCED_DATA,
                          inner, sizeof(inner));
        CHECK(sendAll(fd, req, sizeof(req)));

        // 3. Receive three Sequenced replay packets.
        std::vector<std::string> received;
        for (int i = 0; i < 3; ++i) {
            auto pkt = recvSoupPacket(fd);
            CHECK(!pkt.empty());
            CHECK(pkt[2] == SOUP_PT_SEQUENCED_DATA);
            received.emplace_back(
                reinterpret_cast<const char*>(pkt.data() + 3),
                pkt.size() - 3);
        }
        CHECK(received[0] == "MSG_A");
        CHECK(received[1] == "MSG_B");
        CHECK(received[2] == "MSG_C");

        ::close(fd);
        svc.stop();

        CHECK(svc.requestsServed() == 1);
        CHECK(svc.messagesReplayedTotal() == 3);
    } END
}

void test_ServiceEmptyJournalReturnsNoReplays() {
    TEST(ServiceEmptyJournalReturnsNoReplays) {
        MoldPacketJournal j;
        ItchRetransmissionService svc(j, "REPLAY");
        CHECK(svc.start(0));

        int fd = tcpConnect(svc.boundPort());
        CHECK(fd >= 0);
        auto login = buildLogin("U", "P");
        sendAll(fd, login.data(), login.size());
        recvSoupPacket(fd);   // discard LoginAccepted

        uint8_t inner[ITCH_RETRANSMIT_REQUEST_BYTES];
        buildRetransmitRequest(inner, /*startSeq=*/1, /*count=*/5);
        uint8_t req[3 + ITCH_RETRANSMIT_REQUEST_BYTES];
        soupWriteEnvelope(req, SOUP_PT_UNSEQUENCED_DATA,
                          inner, sizeof(inner));
        sendAll(fd, req, sizeof(req));

        // Give the service a moment to process — no replay packets
        // will arrive, but we want to confirm the request was
        // accepted (counter incremented).
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        ::close(fd);
        svc.stop();
        CHECK(svc.requestsServed() == 1);
        CHECK(svc.messagesReplayedTotal() == 0
              && "no journaled messages → no replays");
    } END
}

void test_ServiceMissingRangeReturnsNoReplays() {
    TEST(ServiceMissingRangeReturnsNoReplays) {
        // Journal has seqs 100..105; client requests 50..52 (all
        // missing — below the floor).
        MoldPacketJournal j;
        for (uint64_t s = 100; s <= 105; ++s) {
            std::string msg = "M" + std::to_string(s);
            j.record(s, msg.data(),
                     static_cast<uint16_t>(msg.size()));
        }

        ItchRetransmissionService svc(j, "REPLAY");
        CHECK(svc.start(0));

        int fd = tcpConnect(svc.boundPort());
        auto login = buildLogin("U", "P");
        sendAll(fd, login.data(), login.size());
        recvSoupPacket(fd);

        uint8_t inner[ITCH_RETRANSMIT_REQUEST_BYTES];
        buildRetransmitRequest(inner, /*startSeq=*/50, /*count=*/3);
        uint8_t req[3 + ITCH_RETRANSMIT_REQUEST_BYTES];
        soupWriteEnvelope(req, SOUP_PT_UNSEQUENCED_DATA,
                          inner, sizeof(inner));
        sendAll(fd, req, sizeof(req));

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        ::close(fd);
        svc.stop();
        CHECK(svc.messagesReplayedTotal() == 0
              && "requested range entirely missing → no replays");
    } END
}

void test_ServiceRejectsMismatchedSession() {
    TEST(ServiceRejectsMismatchedSession) {
        // The service journals for session "REPLAY". A subscriber that
        // logs in naming a DIFFERENT session is attempting a cross-
        // session replay and must be refused at login — before it can
        // ever issue a re-request against this journal.
        MoldPacketJournal j;
        j.record(1, "MSG_A", 5);
        ItchRetransmissionService svc(j, "REPLAY");
        CHECK(svc.start(0));

        int fd = tcpConnect(svc.boundPort());
        CHECK(fd >= 0);

        auto login = buildLoginSession("SUB", "PASS", "OTHERSESS");
        CHECK(sendAll(fd, login.data(), login.size()));

        auto resp = recvSoupPacket(fd);
        CHECK(!resp.empty());
        CHECK(resp[2] == SOUP_PT_LOGIN_REJECTED
              && "cross-session login must be rejected, not accepted");
        CHECK(resp[3] == SOUP_LOGIN_REJECT_SESSION_NOT_AVAILABLE);

        ::close(fd);
        svc.stop();
        // The rejected client never got far enough to issue a request.
        CHECK(svc.requestsServed() == 0);
    } END
}

void test_ServiceAcceptsMatchingSession() {
    TEST(ServiceAcceptsMatchingSession) {
        // Control for the mismatch test: a login that names the service's
        // OWN session is accepted and replays normally (a blank session —
        // "current" — is already exercised by the other service tests).
        MoldPacketJournal j;
        j.record(1, "MSG_A", 5);
        ItchRetransmissionService svc(j, "REPLAY");
        CHECK(svc.start(0));

        int fd = tcpConnect(svc.boundPort());
        CHECK(fd >= 0);

        auto login = buildLoginSession("SUB", "PASS", "REPLAY");
        CHECK(sendAll(fd, login.data(), login.size()));

        auto ack = recvSoupPacket(fd);
        CHECK(ack.size() == 3 + SOUP_SIZE_LOGIN_ACCEPTED_PAYLOAD);
        CHECK(ack[2] == SOUP_PT_LOGIN_ACCEPTED
              && "matching-session login must be accepted");

        ::close(fd);
        svc.stop();
    } END
}

void test_ServiceCapsOverSizedReplay() {
    TEST(ServiceCapsOverSizedReplay) {
        // A journal holding far more than the per-request cap. A single
        // count==0 ("from startSeq to end of journal") re-request is the
        // unbounded path: without a server-side cap it would stream every
        // journaled message, pinning the connection thread. The service
        // must clamp the work to ITCH_RETRANSMIT_MAX_REPLAY.
        const uint64_t total =
            static_cast<uint64_t>(ITCH_RETRANSMIT_MAX_REPLAY) + 500;
        MoldPacketJournal j(static_cast<size_t>(total) + 16);
        for (uint64_t s = 1; s <= total; ++s) {
            j.record(s, "x", 1);
        }

        ItchRetransmissionService svc(j, "REPLAY");
        CHECK(svc.start(0));

        int fd = tcpConnect(svc.boundPort());
        CHECK(fd >= 0);
        auto login = buildLogin("U", "P");
        sendAll(fd, login.data(), login.size());
        recvSoupPacket(fd);   // discard LoginAccepted

        // count == 0 ⇒ "to end of journal" — the path the cap reins in.
        uint8_t inner[ITCH_RETRANSMIT_REQUEST_BYTES];
        buildRetransmitRequest(inner, /*startSeq=*/1, /*count=*/0);
        uint8_t req[3 + ITCH_RETRANSMIT_REQUEST_BYTES];
        soupWriteEnvelope(req, SOUP_PT_UNSEQUENCED_DATA, inner, sizeof(inner));
        sendAll(fd, req, sizeof(req));

        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        ::close(fd);
        svc.stop();
        CHECK(svc.messagesReplayedTotal() == ITCH_RETRANSMIT_MAX_REPLAY
              && "over-cap request must be clamped to the replay cap");
    } END
}

int main() {
    std::cout << "Running ItchRetransmissionTest\n";

    test_JournalRecordAndReplay();
    test_JournalEvictsOldest();
    test_JournalReplayEmptyRange();
    test_JournalReplayEvictedRange();
    test_JournalContainsReflectsWindow();
    test_JournalReplayCountZeroNearUint64Max();
    test_JournalReplayCountWrapGuard();

    test_RetransmitRequestRoundtrip();
    test_RetransmitRequestRejectsBadTag();
    test_RetransmitRequestRejectsBadLength();

    test_ServiceBindsAndAccepts();
    test_ServiceReplaysJournaledMessages();
    test_ServiceEmptyJournalReturnsNoReplays();
    test_ServiceMissingRangeReturnsNoReplays();
    test_ServiceRejectsMismatchedSession();
    test_ServiceAcceptsMatchingSession();
    test_ServiceCapsOverSizedReplay();

    std::cout << "\n" << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
