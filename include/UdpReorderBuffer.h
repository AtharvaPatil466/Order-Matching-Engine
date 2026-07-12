#pragma once
//
// UdpReorderBuffer — RECEIVER-SIDE gap recovery for the DPDK UDP order-entry
// path (Component 2). DPDK-free and header-only (like UdpSequencer) so the whole
// gap/reorder/NAK decision layer is unit-testable on any platform; DpdkGateway
// composes one and wires the delivery + NAK hooks to its MpscQueue and its
// rte_eth_tx_burst NAK sender.
//
// This REPLACES the old UdpSequencer::observe() behaviour of "accept the
// post-gap datagram and resync forward" (which silently lost the gap). Instead a
// future (out-of-order) datagram is HELD in a fixed ring until the hole ahead of
// it is filled, so a merely-reordered datagram is recovered with NO NAK, and a
// genuinely-lost one triggers a NAK for exactly the newly-missing range.
//
// The "NAK only the NEW gap" rule (spec) is driven by highestSeen_, the highest
// sequence ever observed. When a future datagram at `seq` arrives, everything in
// (highestSeen_, seq) is newly discovered as missing — earlier holes were already
// NAK'd when the datagrams past them first arrived. So the NAK range is
// [highestSeen_+1, seq), never the whole [expected_, seq). Example: after holes
// at 3,4 were NAK'd (on receiving 5), receiving 7 NAKs only [6,7), not [3,7).
//
// Threading: single-consumer. onDatagram() is called only from the DPDK poll
// thread. No internal synchronization (a lock would defeat the isolated poll
// core). Read counters from another thread as approximate telemetry only.
//
// No heap on the hot path: the ring is a fixed inline array sized at compile
// time; delivery of a not-yet-buffered in-order datagram uses a stack RxMessage.

#include "RxMessage.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>

namespace OrderMatcher {

// Depth defaults to 64 (power of two). Templated so a test can pin a small depth
// (e.g. 8) to exercise the reorder-window-overflow path without a rebuild.
template <uint32_t Depth = 64>
class UdpReorderBuffer {
    static_assert(Depth > 0 && (Depth & (Depth - 1)) == 0,
                  "kReorderDepth must be a power of two");

public:
    // Deliver an in-order datagram's payload to the next stage (DpdkGateway pushes
    // it onto the MpscQueue). Fires for direct in-order arrivals AND for datagrams
    // drained out of the reorder ring, always in ascending sequence order.
    using DeliverFn = std::function<void(const RxMessage&)>;
    // Request retransmission of [seqStart, seqStart+count). DpdkGateway wires this
    // to sendNak (rte_eth_tx_burst). Fire-and-forget: the hook's own success/drop
    // accounting (tx_naks_sent / tx_nak_drops) lives in the hook implementation.
    using NakFn = std::function<void(uint64_t seqStart, uint32_t count)>;

    static constexpr uint32_t kReorderDepth = Depth;

    UdpReorderBuffer() = default;

    // Pin the first expected sequence (e.g. a session that always starts at 1). If
    // omitted, the first observed datagram becomes the baseline (no startup gap).
    explicit UdpReorderBuffer(uint64_t firstExpected)
        : expected_(firstExpected),
          highestSeen_(firstExpected == 0 ? 0 : firstExpected - 1),
          synchronized_(true) {}

    void setDeliverHook(DeliverFn fn) { deliverHook_ = std::move(fn); }
    void setNakHook(NakFn fn) { nakHook_ = std::move(fn); }

    // Feed one received datagram: its 64-bit sequence and its OUCH payload (the
    // bytes AFTER the sequence header). Handles in-order delivery, buffering of
    // future datagrams, draining once a hole fills, NAK emission for newly-missing
    // ranges, and duplicate/overflow accounting. Never throws, never allocates.
    void onDatagram(uint64_t seq, const void* data, uint16_t len) {
        if (len > kMaxOrderBytes) {
            std::fprintf(stderr,
                         "[reorder] datagram seq=%llu len=%u truncated to %u\n",
                         static_cast<unsigned long long>(seq),
                         static_cast<unsigned>(len),
                         static_cast<unsigned>(kMaxOrderBytes));
            len = kMaxOrderBytes;
        }

        // Cold start: adopt the first sequence as the baseline so a session that
        // begins at, say, 1'000'000 does not report a million-wide gap.
        if (!synchronized_) {
            synchronized_ = true;
            expected_ = seq;
            highestSeen_ = (seq == 0) ? 0 : seq - 1;
        }

        if (seq < expected_) {                       // already delivered / late
            ++duplicates_;
            return;
        }

        if (seq == expected_) {                      // next in order
            RxMessage tmp;
            tmp.len = len;
            std::memcpy(tmp.bytes, data, len);
            deliver(tmp);
            ++expected_;
            if (seq > highestSeen_) highestSeen_ = seq;
            drainContiguous();
            return;
        }

        // seq > expected_: a gap exists. NAK ONLY the newly-discovered missing
        // range (highestSeen_, seq) — earlier holes were NAK'd already.
        if (seq > highestSeen_ + 1) {
            const uint64_t nakStart = highestSeen_ + 1;
            const uint32_t nakCount = static_cast<uint32_t>(seq - nakStart);
            ++gapsDetected_;
            gapDatagrams_ += nakCount;
            if (nakHook_) nakHook_(nakStart, nakCount);
        }

        bufferFuture(seq, data, len);
        if (seq > highestSeen_) highestSeen_ = seq;
    }

    // ── Counters (5 of the 7 required live here; tx_naks_sent / tx_nak_drops are
    //    owned by the NAK hook implementation in DpdkGateway) ──────────────────
    uint64_t gapsDetected()     const { return gapsDetected_; }     // rx_gaps_detected
    uint64_t gapDatagrams()     const { return gapDatagrams_; }     // rx_gap_datagrams
    uint64_t reorderBuffered()  const { return reorderBuffered_; }  // rx_reorder_buffered
    uint64_t reorderRecovered() const { return reorderRecovered_; } // rx_reorder_recovered
    uint64_t reorderOverflow()  const { return reorderOverflow_; }  // rx_reorder_overflow
    uint64_t duplicates()       const { return duplicates_; }
    uint64_t expected()         const { return expected_; }

private:
    static constexpr uint64_t kMask = Depth - 1;

    struct Slot {
        uint64_t  expectedSeq{0};  // 0 = empty; otherwise the seq this slot holds
        RxMessage msg;
    };

    void deliver(const RxMessage& m) {
        if (deliverHook_) deliverHook_(m);
    }

    // Store a future datagram into its slot if within the window and the slot is
    // free or stale. Outside the window, or colliding with a live in-window
    // occupant, it is dropped and counted as an overflow.
    void bufferFuture(uint64_t seq, const void* data, uint16_t len) {
        if (seq >= expected_ + Depth) {              // outside reorder window
            ++reorderOverflow_;
            return;
        }
        Slot& s = ring_[seq & kMask];
        if (s.expectedSeq == seq) {                  // already buffered (dup future)
            ++duplicates_;
            return;
        }
        // A slot is stale if it holds a seq already delivered/abandoned
        // (< expected_). Any occupant that is itself a live in-window future would
        // be a real collision — impossible for two distinct in-window seqs over a
        // Depth-sized ring — so treat that as overflow rather than clobber it.
        const bool occupiedLive =
            s.expectedSeq != 0 && s.expectedSeq >= expected_;
        if (occupiedLive) {
            ++reorderOverflow_;
            return;
        }
        s.expectedSeq = seq;
        s.msg.len = len;
        std::memcpy(s.msg.bytes, data, len);
        ++reorderBuffered_;
    }

    // After an in-order delivery, deliver any contiguous run now sitting in the
    // ring (the datagrams whose hole just closed).
    void drainContiguous() {
        for (;;) {
            Slot& s = ring_[expected_ & kMask];
            if (s.expectedSeq != expected_) break;   // hole — stop draining
            deliver(s.msg);
            s.expectedSeq = 0;                        // free the slot
            ++reorderRecovered_;
            ++expected_;
        }
    }

    uint64_t expected_{0};
    uint64_t highestSeen_{0};
    bool     synchronized_{false};

    uint64_t gapsDetected_{0};
    uint64_t gapDatagrams_{0};
    uint64_t reorderBuffered_{0};
    uint64_t reorderRecovered_{0};
    uint64_t reorderOverflow_{0};
    uint64_t duplicates_{0};

    Slot      ring_[Depth];
    DeliverFn deliverHook_;
    NakFn     nakHook_;
};

}  // namespace OrderMatcher
