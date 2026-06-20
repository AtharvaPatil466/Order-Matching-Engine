// TestFixSessionState — FIX session-layer sequence/recovery + heartbeat logic.

#include "FixSessionState.h"

#include <cassert>
#include <iostream>

using namespace OrderMatcher;

static int tests_passed = 0;
#define SECTION(name) std::cout << "\n  " << name << "..." << std::flush
#define PASS()        do { ++tests_passed; std::cout << " PASS" << std::endl; } while (0)

static bool has(const FixSessionResult& r, FixOut o) {
    for (FixOut x : r.outbound) if (x == o) return true;
    return false;
}

// Inbound sequence handling is the safety core: in-order accept, gap -> resend
// (no out-of-order accept), duplicate ignore, test-request -> heartbeat,
// sequence-reset, logout teardown.
void test_sequence_recovery() {
    SECTION("logon / in-order / gap-resend / dup / testreq / seqreset / logout");
    FixSessionState s(30);

    FixSessionResult r = s.onInbound(FixMsgKind::Logon, 1, false);
    assert(s.isLoggedOn());
    assert(has(r, FixOut::Logon));
    assert(s.expectedInboundSeqNum() == 2);

    r = s.onInbound(FixMsgKind::Application, 2, false);
    assert(r.acceptApp);
    assert(s.expectedInboundSeqNum() == 3);

    // Gap: seq 5 with expected 3 -> ResendRequest(from=3, through-infinity),
    // do NOT accept, do NOT advance.
    r = s.onInbound(FixMsgKind::Application, 5, false);
    assert(!r.acceptApp);
    assert(has(r, FixOut::ResendRequest));
    assert(r.resendFrom == 3 && r.resendTo == 0);
    assert(s.expectedInboundSeqNum() == 3);

    // Duplicate (seq 2 < expected 3, PossDup) -> ignored, no teardown.
    r = s.onInbound(FixMsgKind::Application, 2, true);
    assert(!r.acceptApp);
    assert(s.isLoggedOn());
    assert(s.expectedInboundSeqNum() == 3);

    // In-order TestRequest -> Heartbeat echoing the TestReqID.
    r = s.onInbound(FixMsgKind::TestRequest, 3, false, /*newSeqNo=*/0, /*testReqId=*/99);
    assert(has(r, FixOut::Heartbeat));
    assert(r.testReqId == 99);
    assert(s.expectedInboundSeqNum() == 4);

    // SequenceReset jumps expected forward.
    r = s.onInbound(FixMsgKind::SequenceReset, 0, false, /*newSeqNo=*/10);
    assert(s.expectedInboundSeqNum() == 10);

    // In-order Logout tears the session down.
    r = s.onInbound(FixMsgKind::Logout, 10, false);
    assert(has(r, FixOut::Logout));
    assert(!s.isLoggedOn());
    PASS();
}

// A non-PossDup too-low sequence is a fatal error -> Logout.
void test_fatal_low_seqnum() {
    SECTION("too-low non-dup sequence tears the session down");
    FixSessionState s(30);
    s.onInbound(FixMsgKind::Logon, 1, false);     // expected -> 2
    s.onInbound(FixMsgKind::Application, 2, false); // expected -> 3
    FixSessionResult r = s.onInbound(FixMsgKind::Application, 1, /*possDup=*/false);
    assert(has(r, FixOut::Logout));
    assert(!s.isLoggedOn());
    PASS();
}

// Heartbeat fires on outbound idle once logged on.
void test_heartbeat_on_idle() {
    SECTION("a heartbeat is emitted after an idle interval");
    FixSessionState s(1);                  // 1s heartbeat interval
    s.onInbound(FixMsgKind::Logon, 1, false);
    s.onClock(0);                          // seed the timers
    bool sawHeartbeat = false;
    for (uint64_t t : {1000ull, 2000ull, 3000ull, 4000ull}) {
        if (has(s.onClock(t), FixOut::Heartbeat)) sawHeartbeat = true;
    }
    assert(sawHeartbeat);
    PASS();
}

int main() {
    std::cout << "Running FixSessionState tests..." << std::endl;
    test_sequence_recovery();
    test_fatal_low_seqnum();
    test_heartbeat_on_idle();
    std::cout << "\nAll " << tests_passed << " FixSessionState test sections passed." << std::endl;
    return 0;
}
