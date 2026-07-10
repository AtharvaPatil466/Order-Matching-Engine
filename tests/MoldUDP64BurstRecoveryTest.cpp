// MoldUDP64BurstRecoveryTest — P3-2: large-burst gap recovery.
//
// A subscriber drops 10,000 consecutive packets during a burst and
// recovers them from the TCP retransmission service, while a separate
// live UDP feed keeps flowing. Verifies:
//
//   1. The retransmission service keeps up: all 10,000 dropped frames are
//      replayed, byte-exact and in order, across the per-request cap
//      (ITCH_RETRANSMIT_MAX_REPLAY) via successive re-requests.
//   2. No head-of-line blocking: while the TCP retransmit streams its
//      backlog, the live UDP feed continues delivering — the two paths
//      are independent, so a backed-up retransmit never stalls live data.
//   3. The retransmit backlog does not overflow the journal ring: the
//      journal is sized to hold the burst, so nothing the subscriber
//      needs has been evicted (the oldest, seq 1, is still present).

#include "ItchRetransmissionService.h"
#include "ItchUdpTransport.h"
#include "MoldPacketJournal.h"
#include "OuchProtocol.h"
#include "SoupBinTCP.h"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
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

template <typename F>
static bool waitFor(F pred, int deadlineMs = 5000) {
    auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count() > deadlineMs) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// ─── Socket helpers (mirrors ItchRetransmissionTest) ─────────────────────────

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

bool recvAll(int fd, void* out, size_t len, int deadlineMs = 5000) {
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

}  // namespace

// ─── Tests ───────────────────────────────────────────────────────────────────

void test_BurstBacklogFitsJournalRing() {
    TEST(BurstBacklogFitsJournalRing) {
        // (3) The journal ring must be sized to hold the burst so nothing
        // the subscriber will re-request has been evicted.
        constexpr uint64_t kBurst = 10000;
        MoldPacketJournal journal(1u << 14);   // 16384 > burst

        for (uint64_t s = 1; s <= kBurst; ++s) {
            uint8_t payload[8];
            writeU64BE(payload, s);
            journal.record(s, payload, sizeof(payload));
        }

        CHECK(journal.size() == kBurst);
        CHECK(journal.lowestSeq() == 1
              && "oldest backlog frame must NOT be evicted");
        CHECK(journal.highestSeq() == kBurst);
        CHECK(journal.contains(1));
        CHECK(journal.contains(kBurst));
    } END
}

void test_TenThousandPacketBurstRecovery() {
    TEST(TenThousandPacketBurstRecovery) {
        constexpr uint64_t kBurst = 10000;

        // Journal pre-loaded with the burst the subscriber "dropped".
        MoldPacketJournal journal(1u << 14);
        for (uint64_t s = 1; s <= kBurst; ++s) {
            uint8_t payload[8];
            writeU64BE(payload, s);
            journal.record(s, payload, sizeof(payload));
        }
        CHECK(journal.size() == kBurst);
        CHECK(journal.contains(1) && "backlog head retained (no ring overflow)");

        ItchRetransmissionService svc(journal, "REPLAY");
        CHECK(svc.start(0));

        // ── Independent LIVE UDP feed (must not stall during recovery). ──
        ItchUdpSubscriber liveSub;
        std::atomic<uint64_t> liveDelivered{0};
        liveSub.mold().setOnMessage(
            [&](uint64_t, const uint8_t*, size_t) {
                liveDelivered.fetch_add(1, std::memory_order_relaxed);
            });
        CHECK(liveSub.start("127.0.0.1", 0));

        ItchUdpPublisher livePub("LIVE");
        CHECK(livePub.start("127.0.0.1", liveSub.boundPort()));

        // ── Recovery worker: re-request the whole 10k backlog over TCP,
        //    in ITCH_RETRANSMIT_MAX_REPLAY-sized chunks, on one long-lived
        //    connection. Runs concurrently with the live feed below. ──
        std::atomic<uint64_t> recovered{0};
        std::atomic<bool>     recoveryOk{false};
        std::thread recovery([&] {
            int fd = tcpConnect(svc.boundPort());
            if (fd < 0) return;
            auto login = buildLogin("SUB", "PASS");
            if (!sendAll(fd, login.data(), login.size())) { ::close(fd); return; }
            recvSoupPacket(fd);   // discard LoginAccepted

            uint64_t start = 1;
            bool ok = true;
            while (recovered.load() < kBurst && ok) {
                uint16_t want = static_cast<uint16_t>(std::min<uint64_t>(
                    kBurst - recovered.load(), ITCH_RETRANSMIT_MAX_REPLAY));

                uint8_t inner[ITCH_RETRANSMIT_REQUEST_BYTES];
                buildRetransmitRequest(inner, start, want);
                uint8_t req[3 + ITCH_RETRANSMIT_REQUEST_BYTES];
                soupWriteEnvelope(req, SOUP_PT_UNSEQUENCED_DATA,
                                  inner, sizeof(inner));
                if (!sendAll(fd, req, sizeof(req))) { ok = false; break; }

                for (uint16_t i = 0; i < want; ++i) {
                    auto pkt = recvSoupPacket(fd);
                    if (pkt.size() != 3 + 8 ||
                        pkt[2] != SOUP_PT_SEQUENCED_DATA ||
                        readU64BE(pkt.data() + 3) != start + i) {
                        ok = false; break;
                    }
                    recovered.fetch_add(1, std::memory_order_relaxed);
                }
                start += want;
            }
            recoveryOk.store(ok);
            ::close(fd);
        });

        // ── Drive the live feed WHILE recovery is in flight. If the TCP
        //    retransmit head-of-line-blocked the live path, these would
        //    not arrive until recovery finished (or at all). ──
        constexpr uint64_t kLive = 200;
        for (uint64_t i = 0; i < kLive; ++i) {
            uint8_t payload[8];
            writeU64BE(payload, i);
            livePub.publish(payload, sizeof(payload));
            livePub.flush();
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }

        // (2) Live feed delivered its frames without waiting on the TCP
        //     retransmit — no head-of-line stall.
        CHECK(waitFor([&] { return liveDelivered.load() >= kLive; })
              && "live UDP feed must keep flowing during TCP retransmit");

        // (1) Retransmission service kept up: full 10k recovered, in order.
        CHECK(waitFor([&] { return recovered.load() >= kBurst; }));
        recovery.join();
        CHECK(recoveryOk.load() && "all replayed frames byte-exact and ordered");
        CHECK(recovered.load() == kBurst);
        CHECK(svc.messagesReplayedTotal() == kBurst);

        livePub.stop();
        liveSub.stop();
        svc.stop();
    } END
}

int main() {
    std::cout << "Running MoldUDP64BurstRecoveryTest\n";
    test_BurstBacklogFitsJournalRing();
    test_TenThousandPacketBurstRecovery();
    std::cout << "\n" << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
