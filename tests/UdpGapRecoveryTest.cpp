// UdpGapRecoveryTest — Option A gap recovery on the DPDK UDP order-entry path.
//
// Exercises the two DPDK-free units that carry the gap-recovery logic:
//   * UdpReorderBuffer  (receiver): in-order delivery, hold-and-drain of
//     out-of-order datagrams, NAK-only-the-new-gap, duplicate/overflow accounting.
//   * UdpRetransmitBuffer (sender): record + replay with a bounded window.
//
// Both are header-only and DPDK-independent, so all seven tests run on CI with
// NO NIC. The one path that genuinely needs DPDK — DpdkGateway::sendNak building
// an mbuf and calling rte_eth_tx_burst — is exercised here through the NAK hook
// SEAM: Test 7 makes the hook report a drop (as sendNak does when the TX mbuf
// pool is exhausted) and asserts the RX path keeps running. The real
// rte_pktmbuf_alloc/rte_eth_tx_burst behaviour is validated on hardware CI behind
// OB_HAVE_DPDK.

#include "RxMessage.h"
#include "UdpReorderBuffer.h"
#include "UdpRetransmitBuffer.h"
#include "UdpSequencer.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

using namespace OrderMatcher;

namespace {

// A test datagram's payload is just its 8-byte big-endian sequence, so the
// delivered stream can be checked for exact order and content.
void encodeSeqPayload(uint64_t seq, uint8_t out[8]) {
    UdpSequencer::writeSeqBE(out, seq);
}

// Receiver-side harness: a reorder buffer wired to recording deliver + NAK hooks.
template <uint32_t Depth>
struct RxHarness {
    UdpReorderBuffer<Depth> rb;
    std::vector<uint64_t> delivered;                    // seq of each delivered datagram, in order
    std::vector<std::pair<uint64_t, uint32_t>> naks;    // (seqStart, count) per NAK
    uint64_t txNaksSent = 0;
    uint64_t txNakDrops = 0;
    bool nakAlwaysDrops = false;                         // Test 7: model TX pool full

    RxHarness() {
        rb.setDeliverHook([this](const RxMessage& m) {
            assert(m.len >= 8 && "test payloads carry an 8-byte seq");
            delivered.push_back(UdpSequencer::readSeqBE(m.bytes));
        });
        rb.setNakHook([this](uint64_t start, uint32_t count) {
            naks.push_back({start, count});
            // Mirror DpdkGateway::sendNak's success/drop accounting behind the seam.
            if (nakAlwaysDrops) ++txNakDrops; else ++txNaksSent;
        });
    }

    void feed(uint64_t seq) {
        uint8_t payload[8];
        encodeSeqPayload(seq, payload);
        rb.onDatagram(seq, payload, sizeof(payload));
    }
};

bool orderIs(const std::vector<uint64_t>& got, std::initializer_list<uint64_t> want) {
    return got.size() == want.size() &&
           std::equal(got.begin(), got.end(), want.begin());
}

// ── Test 1 ───────────────────────────────────────────────────────────────────
void testInOrderDelivery() {
    std::printf("Running testInOrderDelivery...\n");
    RxHarness<64> h;
    for (uint64_t s = 1; s <= 5; ++s) h.feed(s);

    assert(orderIs(h.delivered, {1, 2, 3, 4, 5}));
    assert(h.naks.empty() && "no gap -> no NAK");
    assert(h.rb.reorderBuffered() == 0);
    assert(h.rb.reorderRecovered() == 0);
    std::printf("testInOrderDelivery PASSED\n");
}

// ── Test 2 ───────────────────────────────────────────────────────────────────
void testSingleGap() {
    std::printf("Running testSingleGap...\n");
    RxHarness<64> h;
    h.feed(1);
    h.feed(2);
    assert(orderIs(h.delivered, {1, 2}) && "1,2 delivered immediately");

    h.feed(4);  // gap at 3: 4 is buffered, NAK(3,1)
    assert(orderIs(h.delivered, {1, 2}) && "4 not delivered while 3 is missing");
    assert(h.rb.reorderBuffered() == 1);
    assert(h.naks.size() == 1);
    assert(h.naks[0].first == 3 && h.naks[0].second == 1);

    h.feed(3);  // retransmit fills the hole: deliver 3, then drain 4
    assert(h.rb.reorderRecovered() == 1);
    assert(orderIs(h.delivered, {1, 2, 3, 4}));
    std::printf("testSingleGap PASSED\n");
}

// ── Test 3 ───────────────────────────────────────────────────────────────────
void testMultipleGaps() {
    std::printf("Running testMultipleGaps...\n");
    RxHarness<64> h;
    h.feed(1);
    h.feed(2);
    h.feed(5);  // holes 3,4 -> NAK(3,2), buffer 5
    h.feed(7);  // hole 6   -> NAK(6,1), buffer 7 (3,4 already NAK'd, not re-NAK'd)

    auto hasNak = [&](uint64_t start, uint32_t count) {
        return std::find(h.naks.begin(), h.naks.end(),
                         std::make_pair(start, count)) != h.naks.end();
    };
    assert(hasNak(3, 2) && "NAK for gap [3,4]");
    assert(hasNak(6, 1) && "NAK for gap [6]");
    assert(h.naks.size() == 2 && "only the two new gaps NAK'd");

    // Fill in reverse order: the later gap (6) first, then the earlier gap (3,4).
    h.feed(6);
    h.feed(3);
    h.feed(4);
    assert(orderIs(h.delivered, {1, 2, 3, 4, 5, 6, 7}));
    std::printf("testMultipleGaps PASSED\n");
}

// ── Test 4 ───────────────────────────────────────────────────────────────────
void testReorderWindowOverflow() {
    std::printf("Running testReorderWindowOverflow...\n");
    RxHarness<8> h;  // kReorderDepth = 8 for this test
    h.feed(1);       // delivered; expected -> 2
    h.feed(10);      // gap of 8 (seqs 2..9) is at/over the window edge -> overflow

    assert(h.rb.reorderOverflow() == 1 && "seq 10 outside the 8-slot window");
    assert(h.naks.size() == 1);
    assert(h.naks[0].first == 2 && h.naks[0].second == 8 && "NAK for [2..9]");
    std::printf("testReorderWindowOverflow PASSED\n");
}

// ── Test 5 ───────────────────────────────────────────────────────────────────
void testDuplicateDropped() {
    std::printf("Running testDuplicateDropped...\n");
    RxHarness<64> h;
    h.feed(1);
    h.feed(2);
    h.feed(3);
    h.feed(2);  // already delivered -> duplicate, dropped, no NAK

    assert(orderIs(h.delivered, {1, 2, 3}) && "duplicate not re-delivered");
    assert(h.rb.duplicates() == 1);
    assert(h.naks.empty() && "no NAK for a duplicate");
    std::printf("testDuplicateDropped PASSED\n");
}

// ── Test 6 ───────────────────────────────────────────────────────────────────
void testRetransmitBufferMiss() {
    std::printf("Running testRetransmitBufferMiss...\n");
    constexpr uint32_t kDepth = 4096;
    UdpRetransmitBuffer<kDepth> tb;

    for (uint64_t s = 1; s <= kDepth; ++s) {
        uint8_t p[8];
        encodeSeqPayload(s, p);
        tb.record(s, p, sizeof(p));
    }

    std::vector<uint64_t> replayed;
    auto sendFn = [&](const void* d, uint16_t len) {
        assert(len >= 8);
        replayed.push_back(UdpSequencer::readSeqBE(static_cast<const uint8_t*>(d)));
    };

    uint32_t n = tb.replay(1, 5, sendFn);
    assert(n == 5);
    assert(orderIs(replayed, {1, 2, 3, 4, 5}));

    // seq kDepth+1 maps to slot 1 (== 1 & mask) and overwrites the seq-1 entry.
    uint8_t p[8];
    encodeSeqPayload(kDepth + 1, p);
    tb.record(kDepth + 1, p, sizeof(p));

    replayed.clear();
    uint32_t n2 = tb.replay(1, 1, sendFn);  // seq 1 has been overwritten
    assert(n2 == 0 && "seq 1 no longer recoverable");
    assert(tb.misses() == 1 && "retransmit miss logged, no crash");
    std::printf("testRetransmitBufferMiss PASSED\n");
}

// ── Test 7 ───────────────────────────────────────────────────────────────────
// Models "TX mbuf pool exhausted" via the NAK seam (real rte_eth_tx_burst is
// hardware-gated). The contract under test: a dropped NAK is counted and the RX
// path keeps processing — no stall, no exception.
void testNakTxDropDoesNotStallRx() {
    std::printf("Running testNakTxDropDoesNotStallRx...\n");
    RxHarness<64> h;
    h.nakAlwaysDrops = true;  // every NAK "drops" as if the TX pool were full

    h.feed(1);  // delivered normally
    h.feed(3);  // gap at 2 -> NAK(2,1) attempted, dropped; seq 3 still buffered

    assert(h.txNakDrops == 1);
    assert(h.txNaksSent == 0);
    assert(orderIs(h.delivered, {1}) && "seq 1 processed despite NAK drop");
    assert(h.rb.reorderBuffered() == 1 && "seq 3 processed (buffered) despite NAK drop");
    std::printf("testNakTxDropDoesNotStallRx PASSED\n");
}

}  // namespace

int main() {
    std::printf("\n=== UDP Gap-Recovery Tests ===\n");
    testInOrderDelivery();
    testSingleGap();
    testMultipleGaps();
    testReorderWindowOverflow();
    testDuplicateDropped();
    testRetransmitBufferMiss();
    testNakTxDropDoesNotStallRx();
    std::printf("\nUdpGapRecoveryTest passed\n");
    return 0;
}
