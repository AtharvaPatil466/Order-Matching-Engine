// MoldUDP64Test — exercises the multicast wrapper for ITCH-style
// market data feeds: header codec, batched publisher, gap-detecting
// subscriber, and end-to-end through ItchPublisher.
//
// Coverage:
//   Codec
//     1. Header roundtrip with session trimming
//     2. Header rejects too-short buffer
//
//   Publisher
//     3. Single message → single packet with count=1
//     4. Batch fills until MTU forces flush
//     5. Heartbeat emits count=0 packet with next-expected seq
//     6. EndOfSession emits count=0xFFFF
//     7. Sequence numbers advance across multiple flushes
//
//   Subscriber
//     8. In-order delivery via onMessage callback
//     9. Gap detection fires onGapDetected with the expected window
//    10. Duplicate / retransmitted packets are skipped, not redelivered
//    11. Session-ID mismatch is reported, not delivered
//    12. EndOfSession fires onEndOfSession
//    13. Heartbeat with advanced seq detects silent gap
//
//   Integration
//    14. ITCH AddOrder → publisher → wire → subscriber → AddOrder
//        bytes preserved exactly.

#include "ItchProtocol.h"
#include "MoldUDP64.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
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

// ─── Codec tests ────────────────────────────────────────────────────────────

void test_HeaderRoundtrip() {
    TEST(HeaderRoundtrip) {
        uint8_t buf[MOLD_HEADER_BYTES];
        moldWriteHeader(buf, "SESS01", /*seq=*/12345ULL, /*count=*/7);

        MoldHeader hdr;
        CHECK(moldReadHeader(buf, sizeof(buf), hdr));
        CHECK(hdr.session == "SESS01");  // trailing spaces trimmed
        CHECK(hdr.sequenceNumber == 12345ULL);
        CHECK(hdr.messageCount == 7);
    } END
}

void test_HeaderRejectsShortBuffer() {
    TEST(HeaderRejectsShortBuffer) {
        uint8_t buf[5] = {0};
        MoldHeader hdr;
        CHECK(!moldReadHeader(buf, sizeof(buf), hdr));
    } END
}

// ─── Publisher tests ────────────────────────────────────────────────────────

void test_PublisherEmitsSingleMessagePacket() {
    TEST(PublisherEmitsSingleMessagePacket) {
        std::vector<std::vector<uint8_t>> wire;
        MoldUDP64Publisher pub("SESS", [&](std::string_view b) {
            wire.emplace_back(b.begin(), b.end());
        });

        uint8_t msg[10] = {1,2,3,4,5,6,7,8,9,10};
        uint64_t seq = pub.addMessage(msg, sizeof(msg));
        CHECK(seq == 1);                       // first message
        CHECK(pub.pendingCount() == 1);
        pub.flush();

        CHECK(wire.size() == 1);
        const auto& p = wire[0];
        CHECK(p.size() == MOLD_HEADER_BYTES + 2 + 10);

        MoldHeader hdr;
        CHECK(moldReadHeader(p.data(), p.size(), hdr));
        CHECK(hdr.sequenceNumber == 1);
        CHECK(hdr.messageCount == 1);
        CHECK(readU16BE(&p[MOLD_HEADER_BYTES]) == 10);
        CHECK(std::memcmp(&p[MOLD_HEADER_BYTES + 2], msg, 10) == 0);
    } END
}

void test_PublisherAutoFlushesAtMtu() {
    TEST(PublisherAutoFlushesAtMtu) {
        std::vector<size_t> packetSizes;
        // Small MTU so we can pack predictably. Each message is
        // 2 (length prefix) + 30 (payload) = 32 bytes of batch data.
        // Header is 20 bytes. So packets larger than 100B carry at
        // most floor((100 - 20) / 32) = 2 messages.
        MoldUDP64Publisher pub("S", [&](std::string_view b) {
            packetSizes.push_back(b.size());
        }, /*mtu=*/100);

        uint8_t msg[30] = {0};
        // Send 5 messages — should result in 3 packets (2, 2, 1).
        for (int i = 0; i < 5; ++i) {
            std::memset(msg, char('A' + i), sizeof(msg));
            pub.addMessage(msg, sizeof(msg));
        }
        pub.flush();

        CHECK(packetSizes.size() == 3
              && "5 messages must batch into 3 packets at this MTU");
        CHECK(packetSizes[0] == 20 + 2 * (2 + 30));  // header + 2 msgs
        CHECK(packetSizes[1] == 20 + 2 * (2 + 30));
        CHECK(packetSizes[2] == 20 + 1 * (2 + 30));
    } END
}

void test_PublisherHeartbeat() {
    TEST(PublisherHeartbeat) {
        std::vector<std::vector<uint8_t>> wire;
        MoldUDP64Publisher pub("S", [&](std::string_view b) {
            wire.emplace_back(b.begin(), b.end());
        });

        pub.sendHeartbeat();
        CHECK(wire.size() == 1);
        const auto& p = wire[0];
        CHECK(p.size() == MOLD_HEADER_BYTES);  // header only
        MoldHeader hdr;
        moldReadHeader(p.data(), p.size(), hdr);
        CHECK(hdr.messageCount == MOLD_HEARTBEAT);
        CHECK(hdr.sequenceNumber == 1
              && "heartbeat carries the next-to-be-published seq");
    } END
}

void test_PublisherEndOfSession() {
    TEST(PublisherEndOfSession) {
        std::vector<std::vector<uint8_t>> wire;
        MoldUDP64Publisher pub("S", [&](std::string_view b) {
            wire.emplace_back(b.begin(), b.end());
        });

        pub.sendEndOfSession();
        CHECK(wire.size() == 1);
        MoldHeader hdr;
        moldReadHeader(wire[0].data(), wire[0].size(), hdr);
        CHECK(hdr.messageCount == MOLD_END_OF_SESSION);
    } END
}

void test_PublisherSequenceAdvances() {
    TEST(PublisherSequenceAdvances) {
        std::vector<std::vector<uint8_t>> wire;
        MoldUDP64Publisher pub("S", [&](std::string_view b) {
            wire.emplace_back(b.begin(), b.end());
        });

        uint8_t msg[4] = {0};
        uint64_t s1 = pub.addMessage(msg, 4); pub.flush();
        uint64_t s2 = pub.addMessage(msg, 4);
        uint64_t s3 = pub.addMessage(msg, 4); pub.flush();

        CHECK(s1 == 1);
        CHECK(s2 == 2);
        CHECK(s3 == 3);
        CHECK(pub.nextSequence() == 4);
    } END
}

// ─── Subscriber tests ───────────────────────────────────────────────────────

namespace {

struct DeliveryLog {
    struct Entry { uint64_t seq; std::vector<uint8_t> data; };
    std::vector<Entry> messages;
    std::vector<std::pair<uint64_t,uint64_t>> gaps;
    int eos = 0;

    void onMessage(uint64_t seq, const uint8_t* d, size_t n) {
        messages.push_back({seq, std::vector<uint8_t>(d, d + n)});
    }
    void onGap(uint64_t exp, uint64_t got) { gaps.push_back({exp, got}); }
    void onEos() { ++eos; }
};

void wire(MoldUDP64Subscriber& sub, const std::vector<uint8_t>& pkt) {
    sub.feedPacket(pkt.data(), pkt.size());
}

}  // namespace

void test_SubscriberInOrderDelivery() {
    TEST(SubscriberInOrderDelivery) {
        std::vector<std::vector<uint8_t>> wireBuf;
        MoldUDP64Publisher pub("FEED", [&](std::string_view b) {
            wireBuf.emplace_back(b.begin(), b.end());
        });
        pub.addMessage("HELLO", 5);
        pub.addMessage("WORLD", 5);
        pub.flush();

        MoldUDP64Subscriber sub;
        DeliveryLog log;
        sub.setOnMessage([&](uint64_t s, const uint8_t* d, size_t n) {
            log.onMessage(s, d, n);
        });
        sub.setOnGapDetected([&](uint64_t a, uint64_t b) { log.onGap(a, b); });

        for (const auto& pkt : wireBuf) wire(sub, pkt);
        CHECK(log.messages.size() == 2);
        CHECK(log.messages[0].seq == 1);
        CHECK(std::string(log.messages[0].data.begin(),
                          log.messages[0].data.end()) == "HELLO");
        CHECK(log.messages[1].seq == 2);
        CHECK(log.gaps.empty());
        CHECK(sub.nextExpectedSequence() == 3);
    } END
}

void test_SubscriberGapDetection() {
    TEST(SubscriberGapDetection) {
        std::vector<std::vector<uint8_t>> wireBuf;
        MoldUDP64Publisher pub("FEED", [&](std::string_view b) {
            wireBuf.emplace_back(b.begin(), b.end());
        });

        // Send 3 packets; we drop packet #2 to simulate UDP loss.
        pub.addMessage("A", 1); pub.flush();   // seq=1
        pub.addMessage("B", 1); pub.flush();   // seq=2
        pub.addMessage("C", 1); pub.flush();   // seq=3

        MoldUDP64Subscriber sub;
        DeliveryLog log;
        sub.setOnMessage([&](uint64_t s, const uint8_t* d, size_t n) {
            log.onMessage(s, d, n);
        });
        sub.setOnGapDetected([&](uint64_t a, uint64_t b) { log.onGap(a, b); });

        wire(sub, wireBuf[0]);                  // seq=1 ok
        // Drop wireBuf[1] (seq=2).
        wire(sub, wireBuf[2]);                  // seq=3 — gap!

        CHECK(log.messages.size() == 2);
        CHECK(log.messages[0].seq == 1);
        CHECK(log.messages[1].seq == 3);
        CHECK(log.gaps.size() == 1);
        CHECK(log.gaps[0].first == 2);          // expected 2
        CHECK(log.gaps[0].second == 3);         // got 3
        CHECK(sub.gapsObserved() == 1);
    } END
}

void test_SubscriberIgnoresRetransmission() {
    TEST(SubscriberIgnoresRetransmission) {
        std::vector<std::vector<uint8_t>> wireBuf;
        MoldUDP64Publisher pub("FEED", [&](std::string_view b) {
            wireBuf.emplace_back(b.begin(), b.end());
        });
        pub.addMessage("X", 1); pub.flush();    // seq=1
        pub.addMessage("Y", 1); pub.flush();    // seq=2

        MoldUDP64Subscriber sub;
        DeliveryLog log;
        sub.setOnMessage([&](uint64_t s, const uint8_t* d, size_t n) {
            log.onMessage(s, d, n);
        });

        wire(sub, wireBuf[0]);
        wire(sub, wireBuf[1]);
        // Replay packet #1 — must NOT be delivered again.
        wire(sub, wireBuf[0]);

        CHECK(log.messages.size() == 2
              && "retransmitted-prior packet must not redeliver");
    } END
}

void test_SubscriberSessionMismatch() {
    TEST(SubscriberSessionMismatch) {
        std::vector<std::vector<uint8_t>> wireBuf;
        MoldUDP64Publisher pub("WRONG", [&](std::string_view b) {
            wireBuf.emplace_back(b.begin(), b.end());
        });
        pub.addMessage("X", 1); pub.flush();

        MoldUDP64Subscriber sub;
        sub.setExpectedSession("RIGHT");
        DeliveryLog log;
        sub.setOnMessage([&](uint64_t s, const uint8_t* d, size_t n) {
            log.onMessage(s, d, n);
        });

        // feedPacket returns false on session mismatch; no message
        // delivered; counter increments.
        CHECK(!sub.feedPacket(wireBuf[0].data(), wireBuf[0].size()));
        CHECK(log.messages.empty());
        CHECK(sub.sessionMismatches() == 1);
    } END
}

void test_SubscriberEndOfSession() {
    TEST(SubscriberEndOfSession) {
        std::vector<std::vector<uint8_t>> wireBuf;
        MoldUDP64Publisher pub("S", [&](std::string_view b) {
            wireBuf.emplace_back(b.begin(), b.end());
        });
        pub.sendEndOfSession();

        MoldUDP64Subscriber sub;
        DeliveryLog log;
        sub.setOnEndOfSession([&] { log.onEos(); });
        wire(sub, wireBuf[0]);
        CHECK(log.eos == 1);
    } END
}

void test_SubscriberHeartbeatDetectsSilentGap() {
    TEST(SubscriberHeartbeatDetectsSilentGap) {
        // A subscriber that misses a whole burst of packets receives
        // a much-later heartbeat; the seq field on the heartbeat is
        // higher than expected, indicating a silent gap.
        std::vector<std::vector<uint8_t>> wireBuf;
        MoldUDP64Publisher pub("S", [&](std::string_view b) {
            wireBuf.emplace_back(b.begin(), b.end());
        });
        pub.addMessage("A", 1); pub.flush();   // seq=1
        pub.addMessage("B", 1); pub.flush();   // seq=2
        pub.sendHeartbeat();                   // heartbeat with nextSeq=3
        pub.addMessage("C", 1); pub.flush();   // seq=3

        MoldUDP64Subscriber sub;
        DeliveryLog log;
        sub.setOnMessage([&](uint64_t s, const uint8_t* d, size_t n) {
            log.onMessage(s, d, n);
        });
        sub.setOnGapDetected([&](uint64_t a, uint64_t b) { log.onGap(a, b); });

        // Drop everything BUT the heartbeat: subscriber sees an empty
        // packet whose seq is 3, while it was expecting 1.
        wire(sub, wireBuf[2]);

        CHECK(log.gaps.size() == 1);
        CHECK(log.gaps[0].first == 1);
        CHECK(log.gaps[0].second == 3);
        CHECK(sub.nextExpectedSequence() == 3);

        // The post-heartbeat real packet (seq=3) arrives; delivered.
        wire(sub, wireBuf[3]);
        CHECK(log.messages.size() == 1);
        CHECK(log.messages[0].seq == 3);
    } END
}

// ─── Integration: ITCH AddOrder via MoldUDP64 ───────────────────────────────

void test_ItchAddOrderRoundtripsThroughMold() {
    TEST(ItchAddOrderRoundtripsThroughMold) {
        std::vector<std::vector<uint8_t>> wireBuf;
        MoldUDP64Publisher pub("ITCH1", [&](std::string_view b) {
            wireBuf.emplace_back(b.begin(), b.end());
        });

        uint8_t itch[ITCH_SIZE_ADD_ORDER];
        encodeAddOrder(itch, /*locate=*/7, /*tracking=*/0,
                       /*ts=*/0x010203040506ULL, /*ref=*/99,
                       Side::Buy, /*shares=*/100, /*stock=*/42,
                       /*price=*/1000);
        pub.addMessage(itch, sizeof(itch));
        pub.flush();

        MoldUDP64Subscriber sub;
        sub.setExpectedSession("ITCH1");
        std::vector<uint8_t> received;
        uint8_t receivedType = 0;
        sub.setOnMessage([&](uint64_t, const uint8_t* d, size_t n) {
            received.assign(d, d + n);
            if (n > 0) receivedType = d[0];
        });

        CHECK(sub.feedPacket(wireBuf[0].data(), wireBuf[0].size()));
        CHECK(received.size() == ITCH_SIZE_ADD_ORDER);
        CHECK(receivedType == ITCH_MT_ADD_ORDER);

        // Parse the inner ITCH header out to confirm fields preserved
        // byte-exactly through the mold envelope.
        CHECK(readU64BE(received.data() + 11) == 99ULL);  // order ref
        CHECK(readU32BE(received.data() + 20) == 100);    // shares
    } END
}

int main() {
    std::cout << "Running MoldUDP64Test\n";

    test_HeaderRoundtrip();
    test_HeaderRejectsShortBuffer();

    test_PublisherEmitsSingleMessagePacket();
    test_PublisherAutoFlushesAtMtu();
    test_PublisherHeartbeat();
    test_PublisherEndOfSession();
    test_PublisherSequenceAdvances();

    test_SubscriberInOrderDelivery();
    test_SubscriberGapDetection();
    test_SubscriberIgnoresRetransmission();
    test_SubscriberSessionMismatch();
    test_SubscriberEndOfSession();
    test_SubscriberHeartbeatDetectsSilentGap();

    test_ItchAddOrderRoundtripsThroughMold();

    std::cout << "\n" << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
