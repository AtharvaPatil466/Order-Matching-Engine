#pragma once
//
// UdpSequencer — 64-bit per-datagram sequence tracking with gap detection and
// a resend-request hook, for co-located LAN UDP order entry.
//
// Why this exists
// ---------------
// The DPDK data plane (see DpdkGateway) receives OUCH order-entry messages over
// raw UDP. UDP gives us kernel-bypass, message-atomic datagrams, and no
// head-of-line blocking — but, unlike TCP, it does NOT guarantee in-order,
// gap-free delivery. On a co-located LAN, loss is rare but not impossible
// (buffer overrun on a burst, a transient link error). Because the receiver
// feeds every datagram's payload into a SINGLE, PERSISTENT OuchSession parser,
// a lost or reordered datagram would corrupt the OUCH frame stream. So we stamp
// every datagram with a monotonically increasing 64-bit sequence number and
// verify it before delivery.
//
// Design
// ------
// Pure logic, header-only, and free of any DPDK dependency, so it is
// unit-testable on ANY platform — including the default macOS build where
// OB_HAVE_DPDK is undefined and none of the DPDK path compiles. The
// classification is a stand-alone `constexpr` pure function (`classifySeq`);
// the stateful `UdpSequencer` wraps it, tracks the next-expected sequence, fires
// a resend-request hook on a gap, and keeps counters.
//
// Wire model
// ----------
// Every order-entry datagram carries a big-endian 64-bit sequence number as the
// first 8 bytes of the UDP payload, ahead of the OUCH frame(s). A received
// sequence is classified against the next-expected one:
//   received == expected -> InOrder   : accept, expected := received + 1
//   received  > expected -> Gap       : [expected, received) missing -> request
//                                       resend, still accept this datagram, then
//                                       expected := received + 1 (resync forward)
//   received  < expected -> Duplicate : already delivered (or a stale resend) ->
//                                       drop, do not re-deliver
//
// Threading
// ---------
// Single-consumer: `observe()` is called only from the DPDK poll thread. It is
// intentionally NOT internally synchronized — adding a lock would defeat the
// point of the isolated poll core. Read the counters from another thread only
// as approximate telemetry.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

namespace OrderMatcher {

// How a received sequence number relates to the expected one.
enum class SeqStatus { InOrder, Gap, Duplicate };

// Result of classifying one received sequence number. A plain value type so the
// classification can be asserted directly in unit tests with no I/O or state.
struct SeqObservation {
    SeqStatus status{SeqStatus::InOrder};
    uint64_t  expected{0};    // next-expected seq at the moment of observation
    uint64_t  received{0};    // the seq we actually saw
    uint64_t  gapFrom{0};     // first missing seq  (valid iff status == Gap)
    uint64_t  gapToExcl{0};   // one-past-last missing seq == received (iff Gap)
    bool      accept{false};  // should the payload be delivered to the parser?

    // Number of datagrams presumed lost in this gap (0 unless status == Gap).
    constexpr uint64_t gapCount() const {
        return status == SeqStatus::Gap ? (gapToExcl - gapFrom) : 0;
    }
};

// Pure, side-effect-free classification of `received` against `expected`.
// Deliberately free-standing and `constexpr` so it can be exhaustively
// unit-tested without constructing a UdpSequencer.
constexpr SeqObservation classifySeq(uint64_t expected, uint64_t received) {
    if (received == expected) {
        return SeqObservation{SeqStatus::InOrder, expected, received,
                              /*gapFrom=*/0, /*gapToExcl=*/0, /*accept=*/true};
    }
    if (received > expected) {
        return SeqObservation{SeqStatus::Gap, expected, received,
                              /*gapFrom=*/expected, /*gapToExcl=*/received,
                              /*accept=*/true};
    }
    return SeqObservation{SeqStatus::Duplicate, expected, received,
                          /*gapFrom=*/0, /*gapToExcl=*/0, /*accept=*/false};
}

class UdpSequencer {
public:
    // Called with the half-open range [fromSeqInclusive, toSeqExclusive) of
    // sequence numbers that appear to be missing. On a co-located LAN the
    // gateway would emit a resend/NAK request to the order-entry client.
    using ResendRequestFn =
        std::function<void(uint64_t fromSeqInclusive, uint64_t toSeqExclusive)>;

    UdpSequencer() = default;

    // Pin the first expected sequence up front (e.g. a session that always
    // starts at 1). If you don't, the FIRST observed datagram becomes the
    // baseline and is treated as in-order (no spurious startup gap).
    explicit UdpSequencer(uint64_t firstExpected)
        : expected_(firstExpected), synchronized_(true) {}

    void setResendRequestHook(ResendRequestFn fn) { resendHook_ = std::move(fn); }

    // Observe one received datagram's sequence number. Applies the pure
    // classification, fires the resend hook on a gap, advances the expected
    // sequence, updates counters, and returns the full observation. The caller
    // delivers the datagram payload to the parser iff the returned `accept`.
    SeqObservation observe(uint64_t received) {
        // Cold start: adopt the first sequence we ever see as the baseline so a
        // session that begins at, say, 1'000'000 does not report a million-wide
        // gap on its very first datagram.
        if (!synchronized_) {
            synchronized_ = true;
            expected_ = received;
        }

        const SeqObservation obs = classifySeq(expected_, received);
        switch (obs.status) {
        case SeqStatus::InOrder:
            ++inOrder_;
            expected_ = received + 1;
            break;
        case SeqStatus::Gap:
            ++gaps_;
            missing_ += obs.gapCount();
            if (resendHook_) resendHook_(obs.gapFrom, obs.gapToExcl);
            // Resync forward: we accept the datagram we DID receive and expect
            // the next one after it. The missing range is left to the resend
            // path; we do not stall the live stream waiting for it.
            expected_ = received + 1;
            break;
        case SeqStatus::Duplicate:
            ++duplicates_;
            break;
        }
        return obs;
    }

    // Re-baseline (e.g. after a session reset / logon). Counters are preserved
    // so lifetime telemetry survives a resync; call reset() to zero them too.
    void resync(uint64_t nextExpected) {
        expected_ = nextExpected;
        synchronized_ = true;
    }

    void reset() {
        expected_ = 0;
        synchronized_ = false;
        inOrder_ = gaps_ = missing_ = duplicates_ = 0;
    }

    uint64_t expected()   const { return expected_; }
    bool     synchronized() const { return synchronized_; }
    uint64_t inOrder()    const { return inOrder_; }
    uint64_t gaps()       const { return gaps_; }      // number of gap EVENTS
    uint64_t missing()    const { return missing_; }   // total datagrams presumed lost
    uint64_t duplicates() const { return duplicates_; }

    // Size, in bytes, of the on-wire sequence header that prefixes the OUCH
    // payload in every datagram.
    static constexpr size_t kSeqHeaderBytes = 8;

    // Pure big-endian 8-byte read of the on-wire sequence header. Caller must
    // guarantee at least kSeqHeaderBytes are readable at `p`.
    static uint64_t readSeqBE(const uint8_t* p) {
        return (static_cast<uint64_t>(p[0]) << 56) |
               (static_cast<uint64_t>(p[1]) << 48) |
               (static_cast<uint64_t>(p[2]) << 40) |
               (static_cast<uint64_t>(p[3]) << 32) |
               (static_cast<uint64_t>(p[4]) << 24) |
               (static_cast<uint64_t>(p[5]) << 16) |
               (static_cast<uint64_t>(p[6]) <<  8) |
               (static_cast<uint64_t>(p[7]));
    }

    // Pure big-endian 8-byte write of a sequence header (for the sender side /
    // for tests that synthesize datagrams). Writes exactly kSeqHeaderBytes.
    static void writeSeqBE(uint8_t* p, uint64_t seq) {
        p[0] = static_cast<uint8_t>(seq >> 56);
        p[1] = static_cast<uint8_t>(seq >> 48);
        p[2] = static_cast<uint8_t>(seq >> 40);
        p[3] = static_cast<uint8_t>(seq >> 32);
        p[4] = static_cast<uint8_t>(seq >> 24);
        p[5] = static_cast<uint8_t>(seq >> 16);
        p[6] = static_cast<uint8_t>(seq >>  8);
        p[7] = static_cast<uint8_t>(seq);
    }

private:
    uint64_t        expected_{0};
    bool            synchronized_{false};
    uint64_t        inOrder_{0};
    uint64_t        gaps_{0};
    uint64_t        missing_{0};
    uint64_t        duplicates_{0};
    ResendRequestFn resendHook_;
};

}  // namespace OrderMatcher
