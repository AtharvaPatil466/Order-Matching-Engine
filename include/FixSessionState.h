#pragma once

// FixSessionState — engine-agnostic FIX 4.x session-layer state machine.
//
// This is the SESSION layer that sits between the wire codec and the
// application: it owns sequence-number tracking, heartbeat / test-request
// timing, gap detection, and recovery. It deliberately knows nothing about
// FIX bytes, the order book, or the matching engine. The transport feeds it
// a MINIMAL PARSED VIEW of session-level fields (message kind, MsgSeqNum,
// PossDupFlag, NewSeqNo, TestReqID) and a millisecond clock; the state
// machine answers with the session messages to emit and whether the inbound
// application payload is safe to hand to the engine.
//
// Why a separate type from the existing FixSession (include/FixSession.h):
// that class is a concrete, engine-coupled FIX-over-TCP gateway adapter
// (it owns a framer and dispatches into MatchingEngine). This type is the
// pure, deterministic protocol kernel — no I/O, no engine, unit-testable in
// isolation — that such a gateway could compose. Keeping them distinct lets
// the session logic be tested and reasoned about without standing up a wire.
//
// Sequence-number rules (FIX 4.2 / 4.4 session layer):
//   seq == expected -> accept; advance expected.
//   seq  > expected -> GAP: emit ResendRequest(begin=expected, end=0
//                      meaning "through infinity"); do NOT advance; do NOT
//                      accept the app message yet.
//   seq  < expected -> if PossDupFlag, it's a harmless duplicate -> ignore;
//                      else a fatal sequence error -> emit Logout (the safe
//                      default: a too-low non-dup seqnum means the session
//                      is unrecoverable and must be torn down).
//
// Determinism: every transition is a pure function of (current state,
// inbound view) or (current state, clock). No wall-clock reads, no
// allocation beyond the small outbound vector. Time is supplied by the
// caller in milliseconds so replay and tests are bit-reproducible.

#include "Types.h"

#include <cstdint>
#include <vector>

namespace OrderMatcher {

// Inbound message classification — the only thing the session layer needs
// to know about a frame's type. The transport maps FIX MsgType(35) to this.
enum class FixMsgKind : uint8_t {
    Logon,         // 35=A
    Logout,        // 35=5
    Heartbeat,     // 35=0
    TestRequest,   // 35=1
    ResendRequest, // 35=2
    SequenceReset, // 35=4
    Reject,        // 35=3
    Application    // any business message (D/F/G/8/...)
};

// Outbound session message the state machine asks the transport to send.
// Application-level replies are emitted elsewhere; this layer only drives
// the session protocol itself.
enum class FixOut : uint8_t {
    Logon,
    Logout,
    Heartbeat,
    TestRequest,
    ResendRequest,
    SequenceReset,
    Reject
};

// Result of handling one inbound message or one clock poll.
struct FixSessionResult {
    // The inbound APPLICATION message is in-order and the session is
    // logged on — the transport should now hand it to the engine. Only
    // ever true for FixMsgKind::Application arriving with seq == expected.
    bool acceptApp = false;

    // Session messages to send, in emission order. May be empty.
    std::vector<FixOut> outbound;

    // Populated when `outbound` contains a ResendRequest. resendTo == 0
    // is the FIX convention for "through infinity" (request everything
    // from resendFrom onward).
    uint64_t resendFrom = 0;
    uint64_t resendTo = 0;

    // Populated when `outbound` contains a SequenceReset (our gap-fill
    // reply to an inbound ResendRequest). The peer should advance its
    // expected-from-us counter to this value.
    uint64_t resetNewSeqNo = 0;

    // Echoed back when we answer an inbound TestRequest with a Heartbeat,
    // or when we originate a TestRequest. The transport stamps the
    // outbound Heartbeat/TestRequest with this TestReqID.
    uint64_t testReqId = 0;
};

// Session connection state. We model the minimal two-state lifecycle the
// sequence/heartbeat logic needs; full FIX adds LogonSent/LogoutSent for
// the active (initiator) side, which a transport can layer on top.
enum class FixSessionState_t : uint8_t {
    LoggedOut,
    LoggedOn
};

class FixSessionState {
public:
    // heartBtIntSec is the negotiated HeartBtInt(108). A value of 0 is
    // permitted by FIX (disables heartbeats); we honor that by never
    // emitting time-driven Heartbeats/TestRequests when it is 0.
    explicit FixSessionState(uint32_t heartBtIntSec)
        : heartBtIntSec_(heartBtIntSec) {}

    // Handle one inbound, already-parsed session-level view.
    //   kind      : classified MsgType
    //   msgSeqNum : MsgSeqNum(34)
    //   possDup   : PossDupFlag(43) == 'Y'
    //   newSeqNo  : NewSeqNo(36) — only meaningful for SequenceReset
    //   testReqId : TestReqID(112) — only meaningful for TestRequest
    FixSessionResult onInbound(FixMsgKind kind, uint64_t msgSeqNum, bool possDup,
                               uint64_t newSeqNo = 0, uint64_t testReqId = 0) {
        FixSessionResult res;

        // Logon is special: it both (re)establishes the session and is
        // itself a sequenced message. Establish first, then run it through
        // the normal sequence check so a Logon arriving with a gap still
        // triggers recovery.
        if (kind == FixMsgKind::Logon && state_ == FixSessionState_t::LoggedOut) {
            state_ = FixSessionState_t::LoggedOn;
            res.outbound.push_back(FixOut::Logon);
            // The Logon reply is outbound traffic — reset the heartbeat
            // timer so we don't immediately emit a redundant Heartbeat.
            markOutbound();
        }

        // Sequence validation governs every sequenced inbound message.
        // A SequenceReset is the one message allowed to be processed even
        // when its own seqnum looks like a gap, because its entire purpose
        // is to repair the expected counter; handle it before the gap test.
        if (kind == FixMsgKind::SequenceReset) {
            // GapFill or Reset: jump expected forward to NewSeqNo. Never
            // move it backward — a reset that tries to lower expected is a
            // protocol error we defend against by ignoring the regression.
            if (newSeqNo > expectedInboundSeqNum_) {
                expectedInboundSeqNum_ = newSeqNo;
            }
            markInbound();
            return res;
        }

        if (msgSeqNum == expectedInboundSeqNum_) {
            // In order. Advance, then act on the message kind.
            ++expectedInboundSeqNum_;
            markInbound();
            applyInOrder(kind, testReqId, res);
            return res;
        }

        if (msgSeqNum > expectedInboundSeqNum_) {
            // GAP. Request a resend from our expected seqnum through
            // infinity (endSeqNo == 0). Do NOT advance expected and do NOT
            // accept the application payload — it is buffered/replayed by
            // the peer. Receiving SOMETHING still counts as liveness.
            res.outbound.push_back(FixOut::ResendRequest);
            res.resendFrom = expectedInboundSeqNum_;
            res.resendTo = 0;  // 0 == through infinity
            markInbound();
            markOutbound();
            return res;
        }

        // msgSeqNum < expectedInboundSeqNum_.
        if (possDup) {
            // Already processed; a benign retransmission. Silently ignore.
            markInbound();
            return res;
        }
        // Too-low without PossDup is a fatal sequence error. Tear down.
        res.outbound.push_back(FixOut::Logout);
        state_ = FixSessionState_t::LoggedOut;
        markInbound();
        markOutbound();
        return res;
    }

    // Clock poll. nowMs is a monotonic millisecond timestamp supplied by
    // the caller. Emits a Heartbeat when no outbound traffic has flowed for
    // heartBtInt, and a TestRequest when no inbound traffic has been seen
    // for ~2.4x heartBtInt (the FIX-recommended liveness threshold). When
    // both thresholds are crossed in one poll, the Heartbeat is emitted
    // first (it is the shorter, lower-severity timer).
    FixSessionResult onClock(uint64_t nowMs) {
        FixSessionResult res;
        if (state_ != FixSessionState_t::LoggedOn || heartBtIntSec_ == 0) {
            return res;
        }

        // Seed the timers on the first poll so an immediate poll after
        // logon doesn't spuriously fire before any interval has elapsed.
        if (!clockSeeded_) {
            lastOutboundMs_ = nowMs;
            lastInboundMs_ = nowMs;
            clockSeeded_ = true;
            return res;
        }

        // Apply timer resets deferred from onInbound (which has no clock): an
        // inbound message refreshes the inbound-idle timer, and any outbound
        // traffic refreshes the heartbeat timer, both relative to this poll.
        if (inboundFresh_)  { lastInboundMs_  = nowMs; inboundFresh_  = false; }
        if (outboundFresh_) { lastOutboundMs_ = nowMs; outboundFresh_ = false; }

        const uint64_t hbMs = static_cast<uint64_t>(heartBtIntSec_) * 1000ull;
        // 2.4x heartBtInt, computed in integer ms (12/5) to stay
        // deterministic and avoid floating point in the hot path.
        const uint64_t testMs = hbMs * 12ull / 5ull;

        // Heartbeat on outbound idle.
        if (nowMs - lastOutboundMs_ >= hbMs) {
            res.outbound.push_back(FixOut::Heartbeat);
            markOutbound(nowMs);
        }

        // TestRequest on inbound idle. Guarded so we issue at most one
        // outstanding TestRequest until the peer's response (any inbound
        // message) resets the inbound timer — avoids flooding a dead peer.
        if (nowMs - lastInboundMs_ >= testMs && !testRequestOutstanding_) {
            res.outbound.push_back(FixOut::TestRequest);
            res.testReqId = ++testReqIdCounter_;
            testRequestOutstanding_ = true;
            markOutbound(nowMs);
        }

        return res;
    }

    // Allocate the next outbound MsgSeqNum(34). The transport calls this
    // for every message it actually sends so that this object remains the
    // single source of truth for the outbound sequence.
    uint64_t allocateOutboundSeqNum() { return nextOutboundSeqNum_++; }

    uint64_t expectedInboundSeqNum() const { return expectedInboundSeqNum_; }
    uint64_t nextOutboundSeqNum() const { return nextOutboundSeqNum_; }
    bool isLoggedOn() const { return state_ == FixSessionState_t::LoggedOn; }
    FixSessionState_t state() const { return state_; }
    uint32_t heartBtIntSec() const { return heartBtIntSec_; }

private:
    // Dispatch for a message that passed the in-order sequence check.
    void applyInOrder(FixMsgKind kind, uint64_t testReqId, FixSessionResult& res) {
        switch (kind) {
        case FixMsgKind::Application:
            // In-order business message — safe to hand to the engine.
            res.acceptApp = true;
            return;

        case FixMsgKind::TestRequest:
            // Reply with a Heartbeat echoing the peer's TestReqID so the
            // peer can correlate the response and confirm we're alive.
            res.outbound.push_back(FixOut::Heartbeat);
            res.testReqId = testReqId;
            markOutbound();
            return;

        case FixMsgKind::ResendRequest:
            // We do not retain sent messages for true replay, so we answer
            // with a SequenceReset-GapFill that advances the peer past the
            // requested range up to our current outbound point. Modeling
            // real replay is out of scope; gap-fill is protocol-sufficient.
            res.outbound.push_back(FixOut::SequenceReset);
            res.resetNewSeqNo = nextOutboundSeqNum_;
            markOutbound();
            return;

        case FixMsgKind::Logout:
            // Acknowledge and tear down.
            res.outbound.push_back(FixOut::Logout);
            state_ = FixSessionState_t::LoggedOut;
            markOutbound();
            return;

        case FixMsgKind::Heartbeat:
        case FixMsgKind::Reject:
            // Pure liveness / informational — no response required. The
            // inbound timer was already reset by markInbound().
            return;

        case FixMsgKind::Logon:
            // A Logon while already LoggedOn (sequence-valid) needs no
            // additional session action here; the establish branch in
            // onInbound handled the LoggedOut->LoggedOn case.
            return;

        case FixMsgKind::SequenceReset:
            // Handled earlier in onInbound before the sequence test.
            return;
        }
    }

    // Any inbound message proves the peer is alive; reset the inbound
    // idle timer and clear an outstanding TestRequest.
    void markInbound() {
        // onInbound has no clock, so defer the timer reset: flag the inbound
        // timer "fresh" and let the next onClock re-seed it to that nowMs.
        testRequestOutstanding_ = false;
        inboundFresh_ = true;
    }

    // Outbound emission resets the heartbeat timer. When a concrete nowMs
    // is available (onClock path) we use it; otherwise (onInbound path) we
    // mark the timer fresh and let the next onClock re-seed it.
    void markOutbound() { outboundFresh_ = true; }
    void markOutbound(uint64_t nowMs) { lastOutboundMs_ = nowMs; outboundFresh_ = false; }

    uint32_t heartBtIntSec_;
    FixSessionState_t state_{FixSessionState_t::LoggedOut};

    // Sequence numbers both start at 1 per FIX (the first message of a
    // session carries MsgSeqNum 1).
    uint64_t expectedInboundSeqNum_{1};
    uint64_t nextOutboundSeqNum_{1};

    // Heartbeat / test-request timing state (milliseconds).
    bool clockSeeded_{false};
    uint64_t lastOutboundMs_{0};
    uint64_t lastInboundMs_{0};
    bool testRequestOutstanding_{false};
    uint64_t testReqIdCounter_{0};

    // "Fresh" flags let onInbound (which has no clock) signal that a timer
    // should be re-seeded at the next onClock observation, keeping the
    // timing model deterministic without threading a clock through every
    // inbound call.
    bool inboundFresh_{false};
    bool outboundFresh_{false};
};

}  // namespace OrderMatcher
