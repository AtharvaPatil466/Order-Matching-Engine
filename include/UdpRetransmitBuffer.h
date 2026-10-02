#pragma once
//
// UdpRetransmitBuffer — SENDER-SIDE retransmit window for the DPDK UDP
// order-entry path. Standalone and DPDK-free: it does NOT touch DpdkGateway or
// the receiver path (Component 5 constraint — sender and receiver stay cleanly
// separated). The only shared type is the RxMessage payload POD.
//
// The sender records every datagram it transmits into a fixed ring keyed by
// sequence number. When it receives a NAK (a resend request for a range of
// missing sequences), it replays the recorded payloads for that range.
//
// Retransmit window limit (documented, by design): the ring holds the last
// kRetransmitDepth datagrams. If the sender has moved more than kRetransmitDepth
// sequences ahead of the oldest sequence a receiver still NAKs for, that datagram
// has been overwritten and is unrecoverable — replay() reports a "retransmit
// miss" for it and continues. Size the depth for the worst-case in-flight window
// of the link.
//
// Threading: single-threaded. The sender's TX thread exclusively owns this
// object; record() and replay() are never called concurrently. No locks, no
// atomics — that is a precondition, not an oversight.

#include "RxMessage.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>

namespace OrderMatcher {

// Depth defaults to 4096 (power of two). Templated so a test can pin a small
// depth without a rebuild.
template <uint32_t Depth = 4096>
class UdpRetransmitBuffer {
    static_assert(Depth > 0 && (Depth & (Depth - 1)) == 0,
                  "kRetransmitDepth must be a power of two");

public:
    // The caller supplies the actual UDP send mechanism (build frame + tx). Takes
    // the recorded payload bytes and length.
    using TxFn = std::function<void(const void* data, uint16_t len)>;

    static constexpr uint32_t kRetransmitDepth = Depth;

    // Record a transmitted datagram. Overwrites slot[seq & mask] silently — once
    // the ring wraps, the old occupant of this slot is gone (that is the window
    // limit above). A payload longer than kMaxOrderBytes is truncated and logged;
    // never a partial store beyond the bound.
    void record(uint64_t seq, const void* data, uint16_t len) {
        Slot& s = ring_[seq & kMask];
        uint16_t n = len;
        if (n > kMaxOrderBytes) {
            std::fprintf(stderr,
                         "[retransmit] record seq=%llu len=%u truncated to %u\n",
                         static_cast<unsigned long long>(seq),
                         static_cast<unsigned>(len),
                         static_cast<unsigned>(kMaxOrderBytes));
            n = kMaxOrderBytes;
        }
        s.seq = seq;
        s.payload.len = n;
        std::memcpy(s.payload.bytes, data, n);
        if (seq > newest_) newest_ = seq;
    }

    // Replay [seqStart, seqStart+count). For each sequence whose slot still holds
    // it, invoke sendFn with the recorded payload. A sequence the ring has since
    // overwritten counts as a miss (misses()); one not yet sent is skipped.
    // Returns the number of datagrams actually replayed.
    //
    // The count comes off the wire in a NAK, so the work is bounded by the ring,
    // not by the count: only the newest Depth sequences can be held, the part of
    // the range below them is counted as missed without walking it, and nothing
    // is printed per miss. (A 4-billion count used to be walked one sequence and
    // one stderr line at a time on the TX thread — MD-17.)
    uint32_t replay(uint64_t seqStart, uint32_t count, const TxFn& sendFn) {
        if (count == 0 || newest_ == 0) return 0;
        const uint64_t span = static_cast<uint64_t>(count) - 1;
        const uint64_t reqLast = (seqStart > UINT64_MAX - span) ? UINT64_MAX : seqStart + span;
        const uint64_t oldest = (newest_ >= Depth) ? newest_ - Depth + 1 : 1;
        if (seqStart < oldest) misses_ += std::min(reqLast, oldest - 1) - seqStart + 1;

        uint32_t replayed = 0;
        const uint64_t last = std::min(reqLast, newest_);
        for (uint64_t seq = std::max(seqStart, oldest); seq <= last; ++seq) {
            const Slot& s = ring_[seq & kMask];
            if (s.seq == seq) {                       // slot still holds this seq
                sendFn(s.payload.bytes, s.payload.len);
                ++replayed;
            } else {
                ++misses_;
            }
        }
        return replayed;
    }

    uint64_t misses() const { return misses_; }

private:
    static constexpr uint64_t kMask = Depth - 1;

    // seq == 0 is the empty sentinel (valid datagram sequences start at 1, per the
    // UdpSequencer wire model), so a default-constructed slot never false-matches.
    struct Slot {
        uint64_t  seq{0};
        RxMessage payload;
    };

    Slot     ring_[Depth];
    uint64_t newest_{0};   // highest sequence recorded
    uint64_t misses_{0};
};

}  // namespace OrderMatcher
