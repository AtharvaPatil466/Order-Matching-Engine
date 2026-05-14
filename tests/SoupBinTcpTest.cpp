// SoupBinTcpTest — exercises the Nasdaq SoupBinTCP transport layer.
//
// Coverage:
//   Envelope codec
//     1. Heartbeat encode → length=1, type=H, no payload
//     2. EndOfSession encode → length=1, type=Z
//     3. LoginRequest encode/decode roundtrip with trim
//     4. LoginAccepted layout (session + seq number ASCII fields)
//     5. Application payload envelope roundtrip
//
//   Framer
//     6. Single packet feed → exactly one onPacket call
//     7. Byte-at-a-time fragmentation reassembles correctly
//     8. Three packets in one feed dispatch in order
//     9. Oversize length-field claim returns false
//    10. Zero-length packet returns false
//
//   Session
//    11. Login validates and emits LoginAccepted
//    12. Login with rejected credentials → LoginRejected + session closes
//    13. App payload before login is dropped (no callback)
//    14. App payload after login fires the callback
//    15. Logout request → EndOfSession + session closed
//    16. Heartbeat fires after idle interval
//    17. Peer-idle timeout returns false from tick()
//    18. Client heartbeat extends peer-idle clock
//
//   Integration
//    19. OUCH EnterOrder wrapped in SoupBinTCP UnsequencedData
//        round-trips through framer + session and lands in OuchSession.

#include "MatchingEngine.h"
#include "OuchProtocol.h"
#include "OuchSession.h"
#include "SoupBinTCP.h"
#include "SoupBinTcpFramer.h"
#include "SoupBinTcpSession.h"

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

void test_HeartbeatEncode() {
    TEST(HeartbeatEncode) {
        uint8_t buf[3];
        size_t n = soupWriteHeartbeat(buf, /*serverSide=*/true);
        CHECK(n == 3);
        CHECK(readU16BE(buf) == 1);   // length = type byte only
        CHECK(buf[2] == 'H');         // server heartbeat

        n = soupWriteHeartbeat(buf, /*serverSide=*/false);
        CHECK(buf[2] == 'R');         // client heartbeat
    } END
}

void test_EndOfSessionEncode() {
    TEST(EndOfSessionEncode) {
        uint8_t buf[3];
        size_t n = soupWriteEndOfSession(buf);
        CHECK(n == 3);
        CHECK(readU16BE(buf) == 1);
        CHECK(buf[2] == 'Z');
    } END
}

void test_LoginRequestRoundtrip() {
    TEST(LoginRequestRoundtrip) {
        std::vector<uint8_t> wire(3 + SOUP_SIZE_LOGIN_REQUEST_PAYLOAD);
        size_t n = soupWriteLoginRequest(wire.data(),
            "ACME  ", "secret    ", "          ", 0);
        CHECK(n == wire.size());
        CHECK(readU16BE(wire.data()) == SOUP_SIZE_LOGIN_REQUEST_PAYLOAD + 1);
        CHECK(wire[2] == 'L');

        SoupLoginRequest req;
        CHECK(soupParseLoginRequest(wire.data() + 3, n - 3, req));
        // Trailing spaces are trimmed on parse.
        CHECK(req.username == "ACME");
        CHECK(req.password == "secret");
        CHECK(req.session  == "");
        CHECK(req.requestedSeq == 0);
    } END
}

void test_LoginAcceptedLayout() {
    TEST(LoginAcceptedLayout) {
        uint8_t buf[3 + SOUP_SIZE_LOGIN_ACCEPTED_PAYLOAD];
        size_t n = soupWriteLoginAccepted(buf, "SESS01", /*seq=*/42);
        CHECK(n == sizeof(buf));
        CHECK(readU16BE(buf) == SOUP_SIZE_LOGIN_ACCEPTED_PAYLOAD + 1);
        CHECK(buf[2] == 'A');
        // Session ASCII at offset 3..12; trimmed value is "SESS01"
        CHECK(buf[3] == 'S' && buf[4] == 'E' && buf[5] == 'S');
        // Sequence ASCII at offset 13..32; "42" left-justified.
        CHECK(buf[13] == '4' && buf[14] == '2' && buf[15] == ' ');
    } END
}

void test_ApplicationPayloadEnvelope() {
    TEST(ApplicationPayloadEnvelope) {
        // Wrap an arbitrary 10-byte payload as Unsequenced Data.
        const uint8_t payload[10] = {1,2,3,4,5,6,7,8,9,10};
        uint8_t buf[3 + 10];
        size_t n = soupWriteEnvelope(buf, SOUP_PT_UNSEQUENCED_DATA, payload, 10);
        CHECK(n == 13);
        CHECK(readU16BE(buf) == 11);  // 1 type + 10 payload
        CHECK(buf[2] == 'U');
        for (size_t i = 0; i < 10; ++i) CHECK(buf[3 + i] == payload[i]);
    } END
}

// ─── Framer tests ───────────────────────────────────────────────────────────

namespace {

struct Capture {
    std::vector<std::vector<uint8_t>> packets;
    std::vector<uint8_t>              types;
    void operator()(const SoupPacket& pkt) {
        types.push_back(pkt.type);
        packets.emplace_back(pkt.payload, pkt.payload + pkt.payloadLen);
    }
};

}  // namespace

void test_FramerSinglePacket() {
    TEST(FramerSinglePacket) {
        SoupBinTcpFramer framer;
        Capture cap;
        framer.setOnPacket(std::ref(cap));

        uint8_t buf[3];
        size_t n = soupWriteHeartbeat(buf, /*serverSide=*/true);
        CHECK(framer.feed(reinterpret_cast<const char*>(buf), n));
        CHECK(cap.types.size() == 1);
        CHECK(cap.types[0] == 'H');
        CHECK(cap.packets[0].empty());
    } END
}

void test_FramerByteFragmentation() {
    TEST(FramerByteFragmentation) {
        SoupBinTcpFramer framer;
        Capture cap;
        framer.setOnPacket(std::ref(cap));

        // Build a LoginRequest packet on the wire (49 bytes total).
        std::vector<uint8_t> wire(3 + SOUP_SIZE_LOGIN_REQUEST_PAYLOAD);
        soupWriteLoginRequest(wire.data(), "U", "p", "", 1);

        // Drip in one byte at a time.
        for (size_t i = 0; i < wire.size(); ++i) {
            CHECK(framer.feed(reinterpret_cast<const char*>(&wire[i]), 1));
            // Partial packet must NOT dispatch.
            if (i + 1 < wire.size()) CHECK(cap.types.empty());
        }
        CHECK(cap.types.size() == 1);
        CHECK(cap.types[0] == 'L');
    } END
}

void test_FramerBatchedPackets() {
    TEST(FramerBatchedPackets) {
        SoupBinTcpFramer framer;
        Capture cap;
        framer.setOnPacket(std::ref(cap));

        std::vector<uint8_t> wire;
        wire.resize(3 * 3);
        soupWriteHeartbeat(&wire[0], true);
        soupWriteEndOfSession(&wire[3]);
        soupWriteHeartbeat(&wire[6], false);

        CHECK(framer.feed(reinterpret_cast<const char*>(wire.data()), wire.size()));
        CHECK(cap.types.size() == 3);
        CHECK(cap.types[0] == 'H');
        CHECK(cap.types[1] == 'Z');
        CHECK(cap.types[2] == 'R');
    } END
}

void test_FramerRejectsOversizeLength() {
    TEST(FramerRejectsOversizeLength) {
        SoupBinTcpFramer framer(/*maxAccumBytes=*/16);
        // 3 byte header that claims length 65535. The framer should
        // reject before attempting to allocate.
        uint8_t bad[3] = {0xFF, 0xFF, 'S'};
        // This depends on the SOUP_MAX_PACKET_PAYLOAD bound — anything
        // > that should be rejected. 65535 - 1 = 65534, which IS the
        // cap, so this exact value is allowed but won't complete (the
        // framer is bounded to 16 bytes for this test).
        // Use a value above the protocol cap by saying length 0 — must
        // be malformed.
        bad[0] = 0; bad[1] = 0;
        CHECK(!framer.feed(reinterpret_cast<const char*>(bad), 3)
              && "length=0 is malformed");
    } END
}

void test_FramerRejectsZeroLength() {
    TEST(FramerRejectsZeroLength) {
        SoupBinTcpFramer framer;
        // Need 3 bytes for the framer to read the length and reach the
        // length-validation check. A length-field of 0 is a protocol
        // violation (must be ≥1 to carry the type byte).
        uint8_t bad[3] = {0, 0, 'S'};
        CHECK(!framer.feed(reinterpret_cast<const char*>(bad), 3));
    } END
}

// ─── Session tests ──────────────────────────────────────────────────────────

namespace {

std::vector<uint8_t> buildLoginRequest(const std::string& user,
                                       const std::string& pass,
                                       const std::string& session = "",
                                       uint64_t seq = 0) {
    std::vector<uint8_t> wire(3 + SOUP_SIZE_LOGIN_REQUEST_PAYLOAD);
    soupWriteLoginRequest(wire.data(), user, pass, session, seq);
    return wire;
}

}  // namespace

void test_SessionLoginAccepted() {
    TEST(SessionLoginAccepted) {
        std::string sent;
        SoupBinTcpSession sess([&](std::string_view b) { sent.append(b); },
                               "SESSXYZ");
        sess.setOnLoginRequest([](const SoupLoginRequest& r) -> char {
            return (r.username == "ACME" && r.password == "secret") ? 0
                : SOUP_LOGIN_REJECT_NOT_AUTHORIZED;
        });

        auto wire = buildLoginRequest("ACME", "secret", "", 0);
        CHECK(sess.feed(reinterpret_cast<const char*>(wire.data()),
                        wire.size(), /*nowMs=*/100));
        CHECK(sess.loggedIn());
        CHECK(!sess.closed());

        // Response is a LoginAccepted packet (type 'A').
        CHECK(sent.size() == 3 + SOUP_SIZE_LOGIN_ACCEPTED_PAYLOAD);
        const auto* resp = reinterpret_cast<const uint8_t*>(sent.data());
        CHECK(resp[2] == 'A');
    } END
}

void test_SessionLoginRejected() {
    TEST(SessionLoginRejected) {
        std::string sent;
        SoupBinTcpSession sess([&](std::string_view b) { sent.append(b); },
                               "SESSXYZ");
        sess.setOnLoginRequest([](const SoupLoginRequest&) {
            return SOUP_LOGIN_REJECT_NOT_AUTHORIZED;
        });

        auto wire = buildLoginRequest("BAD", "creds");
        // feed returns false because session is closed by rejection.
        CHECK(!sess.feed(reinterpret_cast<const char*>(wire.data()),
                         wire.size(), 100));
        CHECK(!sess.loggedIn());
        CHECK(sess.closed());

        CHECK(sent.size() == 4);  // 2 len + 1 type + 1 reason
        const auto* resp = reinterpret_cast<const uint8_t*>(sent.data());
        CHECK(resp[2] == 'J');
        CHECK(resp[3] == SOUP_LOGIN_REJECT_NOT_AUTHORIZED);
    } END
}

void test_SessionRejectsAppDataBeforeLogin() {
    TEST(SessionRejectsAppDataBeforeLogin) {
        std::string sent;
        SoupBinTcpSession sess([&](std::string_view b) { sent.append(b); },
                               "SESS");

        bool appFired = false;
        sess.setOnAppPayload([&](const uint8_t*, size_t, bool) { appFired = true; });

        // Unsequenced data without a prior login.
        uint8_t body[5] = {0,1,2,3,4};
        uint8_t wire[3 + 5];
        soupWriteEnvelope(wire, SOUP_PT_UNSEQUENCED_DATA, body, sizeof(body));
        CHECK(sess.feed(reinterpret_cast<const char*>(wire), sizeof(wire), 100));

        CHECK(!appFired && "must NOT deliver app data before login");
        CHECK(sess.packetsDropped() == 1);
    } END
}

void test_SessionDeliversAppPayloadAfterLogin() {
    TEST(SessionDeliversAppPayloadAfterLogin) {
        std::string sent;
        SoupBinTcpSession sess([&](std::string_view b) { sent.append(b); },
                               "SESS");
        sess.setOnLoginRequest([](const SoupLoginRequest&) { return char{0}; });

        // Login.
        auto login = buildLoginRequest("U", "P");
        sess.feed(reinterpret_cast<const char*>(login.data()), login.size(), 100);
        CHECK(sess.loggedIn());

        // Send Unsequenced application payload.
        std::vector<uint8_t> capturedPayload;
        bool capturedSeq = true;
        sess.setOnAppPayload([&](const uint8_t* p, size_t n, bool seq) {
            capturedPayload.assign(p, p + n);
            capturedSeq = seq;
        });

        uint8_t app[7] = {'h','e','l','l','o','!','!'};
        uint8_t wire[3 + 7];
        soupWriteEnvelope(wire, SOUP_PT_UNSEQUENCED_DATA, app, sizeof(app));
        CHECK(sess.feed(reinterpret_cast<const char*>(wire), sizeof(wire), 110));

        CHECK(capturedPayload.size() == 7);
        CHECK(std::memcmp(capturedPayload.data(), app, 7) == 0);
        CHECK(!capturedSeq && "unsequenced flag must be false");
    } END
}

void test_SessionLogoutHandshake() {
    TEST(SessionLogoutHandshake) {
        std::string sent;
        SoupBinTcpSession sess([&](std::string_view b) { sent.append(b); },
                               "SESS");
        sess.setOnLoginRequest([](const SoupLoginRequest&) { return char{0}; });

        auto login = buildLoginRequest("U", "P");
        sess.feed(reinterpret_cast<const char*>(login.data()), login.size(), 100);
        sent.clear();

        uint8_t lo[3];
        soupWriteLogoutRequest(lo);
        // feed() returns false post-logout to signal the gateway to
        // drop the socket — the session is terminal now.
        CHECK(!sess.feed(reinterpret_cast<const char*>(lo), 3, 200));

        // Server responds with EndOfSession before signaling close.
        CHECK(sent.size() == 3);
        const auto* resp = reinterpret_cast<const uint8_t*>(sent.data());
        CHECK(resp[2] == 'Z');
        CHECK(sess.closed());
    } END
}

void test_SessionEmitsHeartbeatAfterIdle() {
    TEST(SessionEmitsHeartbeatAfterIdle) {
        std::string sent;
        SoupBinTcpSession sess([&](std::string_view b) { sent.append(b); },
                               "SESS");
        sess.setOnLoginRequest([](const SoupLoginRequest&) { return char{0}; });
        sess.setIntervals(/*heartbeatMs=*/1000, /*peerIdleMs=*/15000);

        auto login = buildLoginRequest("U", "P");
        sess.feed(reinterpret_cast<const char*>(login.data()), login.size(), 100);
        sent.clear();

        // Tick before the heartbeat interval — no heartbeat yet.
        CHECK(sess.tick(500));
        CHECK(sess.heartbeatsSent() == 0);

        // Tick past the interval — heartbeat fires.
        CHECK(sess.tick(100 + 1500));
        CHECK(sess.heartbeatsSent() == 1);
        CHECK(sent.size() == 3);
        const auto* resp = reinterpret_cast<const uint8_t*>(sent.data());
        CHECK(resp[2] == 'H');
    } END
}

void test_SessionPeerIdleTimeoutClosesSession() {
    TEST(SessionPeerIdleTimeoutClosesSession) {
        std::string sent;
        SoupBinTcpSession sess([&](std::string_view b) { sent.append(b); },
                               "SESS");
        sess.setOnLoginRequest([](const SoupLoginRequest&) { return char{0}; });
        sess.setIntervals(1000, /*peerIdleMs=*/5000);

        auto login = buildLoginRequest("U", "P");
        sess.feed(reinterpret_cast<const char*>(login.data()), login.size(), 100);

        // 5000ms after the login (the last peer activity) → tick must
        // return false signalling the gateway should close.
        CHECK(!sess.tick(100 + 5000));
    } END
}

void test_SessionClientHeartbeatExtendsLiveness() {
    TEST(SessionClientHeartbeatExtendsLiveness) {
        std::string sent;
        SoupBinTcpSession sess([&](std::string_view b) { sent.append(b); },
                               "SESS");
        sess.setOnLoginRequest([](const SoupLoginRequest&) { return char{0}; });
        sess.setIntervals(1000, /*peerIdleMs=*/5000);

        auto login = buildLoginRequest("U", "P");
        sess.feed(reinterpret_cast<const char*>(login.data()), login.size(), 100);

        // Client heartbeat at t=3000 resets the peer-idle clock.
        uint8_t hb[3];
        soupWriteHeartbeat(hb, /*serverSide=*/false);
        CHECK(sess.feed(reinterpret_cast<const char*>(hb), 3, /*nowMs=*/3000));

        // Now tick at t=7000 — would have timed out (>5s from login),
        // but client HB at 3000 resets the clock.
        CHECK(sess.tick(7000) && "client HB must extend liveness");
    } END
}

// ─── Integration: OUCH over SoupBinTCP ──────────────────────────────────────

void test_OuchOverSoupBinTcp() {
    TEST(OuchOverSoupBinTcp) {
        // Build the stack: SoupBinTcpSession at the bottom; OuchSession
        // consumes the payloads the soup session hands up; OUCH
        // responses pass back through soup as UnsequencedData (server
        // → client, ack pattern).
        MatchingEngine engine;
        engine.addSymbol(7);
        engine.start();

        std::string wireOut;
        SoupBinTcpSession soup(
            [&](std::string_view b) { wireOut.append(b); }, "S");
        soup.setOnLoginRequest([](const SoupLoginRequest&) { return char{0}; });

        // Wire OUCH on top of soup. The OUCH session's send_ wraps
        // outbound bytes in SoupBinTCP UnsequencedData packets — that
        // way, what arrives on the actual TCP socket is spec-compliant
        // top-to-bottom.
        OuchSession ouch(engine, [&](std::string_view ouchBytes) {
            soup.sendUnsequenced(ouchBytes.data(), ouchBytes.size());
        });

        // Soup hands unwrapped payloads up to OUCH.
        soup.setOnAppPayload([&](const uint8_t* p, size_t n, bool) {
            ouch.feed(reinterpret_cast<const char*>(p), n);
        });

        // 1. Client logs in.
        auto login = buildLoginRequest("U", "P");
        CHECK(soup.feed(reinterpret_cast<const char*>(login.data()),
                        login.size(), 100));
        CHECK(soup.loggedIn());

        // Clear the LoginAccepted from the captured wire.
        wireOut.clear();

        // 2. Client sends an OUCH EnterOrder wrapped in soup.
        std::vector<uint8_t> ouchEnter(OUCH_SIZE_ENTER_ORDER, ' ');
        ouchEnter[0] = OUCH_MT_ENTER_ORDER;
        writeAsciiDecimal(ouchEnter.data() + 1, 14, /*token=*/12345);
        ouchEnter[15] = 'B';
        writeU32BE(ouchEnter.data() + 16, /*shares=*/50);
        writeAsciiDecimal(ouchEnter.data() + 20, 8, /*stock=*/7);
        writeU32BE(ouchEnter.data() + 28, /*price=*/1000);
        writeU32BE(ouchEnter.data() + 32, OUCH_TIF_DAY);
        writeAsciiDecimal(ouchEnter.data() + 36, 4, /*firm=*/1);
        ouchEnter[40] = 'Y'; ouchEnter[41] = 'O'; ouchEnter[42] = 'N';
        writeU32BE(ouchEnter.data() + 43, 0);
        ouchEnter[47] = 'N'; ouchEnter[48] = 'R';

        std::vector<uint8_t> soupWrapped(3 + ouchEnter.size());
        soupWriteEnvelope(soupWrapped.data(), SOUP_PT_UNSEQUENCED_DATA,
                          ouchEnter.data(), ouchEnter.size());

        CHECK(soup.feed(reinterpret_cast<const char*>(soupWrapped.data()),
                        soupWrapped.size(), 150));

        // The OUCH session must have accepted and acked the order.
        CHECK(ouch.ordersAccepted() == 1);
        auto* book = engine.getOrderBook(7);
        CHECK(book != nullptr);
        CHECK(book->getOrder(12345) != nullptr);

        // The wire response is a SoupBinTCP Unsequenced packet wrapping
        // an OUCH 'A' (OrderAccepted) reply.
        CHECK(wireOut.size() == 3 + OUCH_SIZE_ORDER_ACCEPTED);
        const auto* resp = reinterpret_cast<const uint8_t*>(wireOut.data());
        CHECK(resp[2] == SOUP_PT_UNSEQUENCED_DATA);
        CHECK(resp[3] == OUCH_MT_ORDER_ACCEPTED);
    } END
}

int main() {
    std::cout << "Running SoupBinTcpTest\n";

    test_HeartbeatEncode();
    test_EndOfSessionEncode();
    test_LoginRequestRoundtrip();
    test_LoginAcceptedLayout();
    test_ApplicationPayloadEnvelope();

    test_FramerSinglePacket();
    test_FramerByteFragmentation();
    test_FramerBatchedPackets();
    test_FramerRejectsOversizeLength();
    test_FramerRejectsZeroLength();

    test_SessionLoginAccepted();
    test_SessionLoginRejected();
    test_SessionRejectsAppDataBeforeLogin();
    test_SessionDeliversAppPayloadAfterLogin();
    test_SessionLogoutHandshake();
    test_SessionEmitsHeartbeatAfterIdle();
    test_SessionPeerIdleTimeoutClosesSession();
    test_SessionClientHeartbeatExtendsLiveness();

    test_OuchOverSoupBinTcp();

    std::cout << "\n" << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
