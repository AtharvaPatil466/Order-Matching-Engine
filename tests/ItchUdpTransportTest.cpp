// ItchUdpTransportTest — exercises the UDP multicast transport for
// ITCH-over-MoldUDP64 end-to-end over real loopback sockets.
//
// The codec (MoldUDP64) and protocol semantics (gap detection,
// retransmit handling) are already covered in MoldUDP64Test. This
// test pins the OS-level wiring:
//
//   1. Publisher actually sends UDP datagrams reachable by a
//      subscriber on loopback.
//   2. Multiple application messages batched into one datagram
//      round-trip intact.
//   3. Heartbeat datagrams are delivered and parsed.
//   4. EndOfSession datagrams fire the subscriber's onEos.
//
// Tests use unicast 127.0.0.1 (not multicast) for portability —
// real multicast requires per-OS group routing config that varies
// between Linux/macOS/CI.

#include "ItchProtocol.h"
#include "ItchUdpTransport.h"
#include "MoldUDP64.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
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

// Wait until `predicate` returns true, polling every 10ms up to a
// deadline. UDP is async — the subscriber's recv thread may not
// have processed the datagram by the time the publisher's send()
// returns, so we busy-poll with a sensible cap.
template <typename F>
bool waitFor(F predicate, int deadlineMs = 1000) {
    auto start = std::chrono::steady_clock::now();
    while (!predicate()) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count() > deadlineMs) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

void test_UdpSingleMessageRoundtrip() {
    TEST(UdpSingleMessageRoundtrip) {
        ItchUdpSubscriber sub;
        std::vector<std::vector<uint8_t>> received;
        std::mutex mtx;
        sub.mold().setOnMessage(
            [&](uint64_t /*seq*/, const uint8_t* d, size_t n) {
                std::lock_guard<std::mutex> lk(mtx);
                received.emplace_back(d, d + n);
            });
        CHECK(sub.start("127.0.0.1", /*port=*/0));
        uint16_t port = sub.boundPort();
        CHECK(port != 0);

        ItchUdpPublisher pub("FEED");
        CHECK(pub.start("127.0.0.1", port));

        const char* msg = "HELLO_ITCH";
        pub.publish(msg, std::strlen(msg));
        pub.flush();

        CHECK(waitFor([&] { std::lock_guard<std::mutex> lk(mtx); return !received.empty(); }));
        {
            std::lock_guard<std::mutex> lk(mtx);
            CHECK(received.size() == 1);
            CHECK(std::string(received[0].begin(), received[0].end()) == "HELLO_ITCH");
        }

        pub.stop();
        sub.stop();
    } END
}

void test_UdpBatchedMessagesInOnePacket() {
    TEST(UdpBatchedMessagesInOnePacket) {
        ItchUdpSubscriber sub;
        std::vector<uint64_t> seqs;
        std::mutex mtx;
        sub.mold().setOnMessage(
            [&](uint64_t seq, const uint8_t*, size_t) {
                std::lock_guard<std::mutex> lk(mtx);
                seqs.push_back(seq);
            });
        CHECK(sub.start("127.0.0.1", 0));
        uint16_t port = sub.boundPort();

        ItchUdpPublisher pub("FEED");
        CHECK(pub.start("127.0.0.1", port));

        // Batch three messages into a single flush — they should all
        // arrive in a single datagram and be delivered in order.
        pub.publish("A", 1);
        pub.publish("B", 1);
        pub.publish("C", 1);
        pub.flush();

        CHECK(waitFor([&] { std::lock_guard<std::mutex> lk(mtx); return seqs.size() == 3; }));
        {
            std::lock_guard<std::mutex> lk(mtx);
            CHECK(seqs[0] == 1 && seqs[1] == 2 && seqs[2] == 3);
            CHECK(pub.datagramsSent() == 1
                  && "three batched messages → one datagram on the wire");
        }

        pub.stop(); sub.stop();
    } END
}

void test_UdpHeartbeatDelivered() {
    TEST(UdpHeartbeatDelivered) {
        ItchUdpSubscriber sub;
        CHECK(sub.start("127.0.0.1", 0));

        ItchUdpPublisher pub("F");
        CHECK(pub.start("127.0.0.1", sub.boundPort()));

        pub.sendHeartbeat();

        CHECK(waitFor([&] { return sub.mold().heartbeatsReceived() >= 1; }));

        pub.stop(); sub.stop();
    } END
}

void test_UdpEndOfSession() {
    TEST(UdpEndOfSession) {
        ItchUdpSubscriber sub;
        std::atomic<bool> eosFired{false};
        sub.mold().setOnEndOfSession([&] { eosFired.store(true); });
        CHECK(sub.start("127.0.0.1", 0));

        ItchUdpPublisher pub("F");
        CHECK(pub.start("127.0.0.1", sub.boundPort()));
        pub.sendEndOfSession();

        CHECK(waitFor([&] { return eosFired.load(); }));

        pub.stop(); sub.stop();
    } END
}

void test_UdpItchAddOrderEndToEnd() {
    TEST(UdpItchAddOrderEndToEnd) {
        // Real ITCH frame over real UDP through MoldUDP64. Confirms
        // the bytes preserve byte-exactly through the whole stack.
        ItchUdpSubscriber sub;
        std::vector<uint8_t> received;
        std::mutex mtx;
        sub.mold().setOnMessage(
            [&](uint64_t, const uint8_t* d, size_t n) {
                std::lock_guard<std::mutex> lk(mtx);
                received.assign(d, d + n);
            });
        CHECK(sub.start("127.0.0.1", 0));

        ItchUdpPublisher pub("ITCH1");
        CHECK(pub.start("127.0.0.1", sub.boundPort()));

        uint8_t itch[ITCH_SIZE_ADD_ORDER];
        encodeAddOrder(itch, /*locate=*/7, /*tracking=*/0,
                       /*ts=*/0x010203040506ULL, /*ref=*/12345,
                       Side::Sell, /*shares=*/250, /*stock=*/42,
                       /*price=*/2500);
        pub.publish(itch, sizeof(itch));
        pub.flush();

        CHECK(waitFor([&] { std::lock_guard<std::mutex> lk(mtx); return received.size() == ITCH_SIZE_ADD_ORDER; }));
        // Wire bytes match exactly: type byte, order ref, shares.
        {
            std::lock_guard<std::mutex> lk(mtx);
            CHECK(received[0] == ITCH_MT_ADD_ORDER);
            CHECK(readU64BE(received.data() + 11) == 12345ULL);
            CHECK(readU32BE(received.data() + 20) == 250);
            CHECK(received[19] == 'S');
        }

        pub.stop(); sub.stop();
    } END
}

int main() {
    std::cout << "Running ItchUdpTransportTest\n";

    test_UdpSingleMessageRoundtrip();
    test_UdpBatchedMessagesInOnePacket();
    test_UdpHeartbeatDelivered();
    test_UdpEndOfSession();
    test_UdpItchAddOrderEndToEnd();

    std::cout << "\n" << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
