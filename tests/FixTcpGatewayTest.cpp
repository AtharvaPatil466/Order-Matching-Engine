// Integration test for FixTcpGateway: spawn the gateway, TCP-connect a
// client, push real FIX bytes through the wire, parse the ExecutionReport
// that comes back, and verify the order made it into the engine's book.
//
// Two scenarios:
//   * Whole-frame send: the framer / session reassemble normally.
//   * One-byte-at-a-time send: the framer must reassemble across many
//     fragmented recv()s (the gateway loop drains in 4KB chunks but the
//     wire delivers them piecemeal because we only send one byte then
//     block briefly on the client side).

#include "FIXParser.h"
#include "FixTcpGateway.h"
#include "MatchingEngine.h"

#include <arpa/inet.h>
#include <cassert>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

using namespace OrderMatcher;

namespace {

int connectTo(uint16_t port, int rcvBuf = 0) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    // Shrink the receive window before connect() so the gateway's send buffer
    // fills after a few hundred KB rather than megabytes.
    if (rcvBuf > 0) ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvBuf, sizeof(rcvBuf));
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool sendAll(int fd, const char* data, size_t len) {
    while (len) {
        ssize_t n = ::send(fd, data, len, MSG_NOSIGNAL);
        if (n <= 0) return false;
        data += n; len -= size_t(n);
    }
    return true;
}

// Drain the socket until we have a complete FIX frame (anchor on trailer
// "<SOH>10=NNN<SOH>"). Times out after `deadline` to avoid hanging the test.
std::string recvFixFrame(int fd, std::chrono::milliseconds deadline) {
    std::string buf;
    auto end = std::chrono::steady_clock::now() + deadline;
    char tmp[1024];
    while (std::chrono::steady_clock::now() < end) {
        // Non-blocking-ish: small recv with a tiny sleep between empties.
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), MSG_DONTWAIT);
        if (n > 0) {
            buf.append(tmp, size_t(n));
            // Frame complete when we see the trailer pattern at end.
            if (buf.size() >= 8 &&
                buf[buf.size() - 1] == FIX_SOH &&
                buf[buf.size() - 8] == FIX_SOH &&
                buf[buf.size() - 7] == '1' &&
                buf[buf.size() - 6] == '0' &&
                buf[buf.size() - 5] == '=') {
                return buf;
            }
            continue;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return buf;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return buf;
}

// Wrap an already-built body (35=...<SOH>...) with BeginString, BodyLength
// and CheckSum.
std::string frameBody(const std::string& body) {
    std::string head = "8=FIX.4.2";
    head += FIX_SOH;
    head += "9=" + std::to_string(body.size());
    head += FIX_SOH;
    std::string pre = head + body;
    uint32_t sum = 0;
    for (char c : pre) sum += static_cast<uint8_t>(c);
    char cs[4];
    std::snprintf(cs, sizeof(cs), "%03d", sum % 256);
    return pre + "10=" + cs + FIX_SOH;
}

// The session now requires a Logon(35=A) with MsgSeqNum(34) before any
// application message, and a MsgSeqNum on every message.
std::string makeLogon(uint64_t seqNum) {
    std::string body;
    body += "35=A"; body += FIX_SOH;
    body += "34=" + std::to_string(seqNum); body += FIX_SOH;
    body += "108=30"; body += FIX_SOH;       // HeartBtInt
    return frameBody(body);
}

std::string makeNewOrder(OrderId clOrdId, SymbolId sym, Quantity qty,
                         Price price, uint64_t seqNum) {
    std::string body;
    body += "35=D"; body += FIX_SOH;
    body += "34=" + std::to_string(seqNum); body += FIX_SOH;  // MsgSeqNum
    body += "49=100"; body += FIX_SOH;     // SenderCompID = participantId
    body += "11=" + std::to_string(clOrdId); body += FIX_SOH;
    body += "55=" + std::to_string(sym);     body += FIX_SOH;
    body += "54=1"; body += FIX_SOH;         // Buy
    body += "38=" + std::to_string(qty);     body += FIX_SOH;
    body += "44=" + std::to_string(price);   body += FIX_SOH;
    body += "40=2"; body += FIX_SOH;         // Limit
    body += "59=1"; body += FIX_SOH;         // GTC
    return frameBody(body);
}

void expectAck(const std::string& raw, OrderId expectedOrderId) {
    FixMessage resp;
    assert(resp.parse(raw.data(), raw.size()) && "ack failed to parse");
    assert(resp.validateChecksum()             && "ack checksum invalid");
    assert(resp.getString(FixTag::MsgType) == "8" && "ack not ExecutionReport");
    assert(resp.getUint64(FixTag::OrderID) == expectedOrderId);
    if (resp.getChar(FixTag::ExecType) != '0') {
        std::fprintf(stderr,
            "ack ExecType=%c text=%.*s\n",
            resp.getChar(FixTag::ExecType),
            int(resp.getString(FixTag::Text).size()),
            resp.getString(FixTag::Text).data());
        std::abort();
    }
}

}  // namespace

int main() {
    constexpr SymbolId kSym = 9;

    MatchingEngine engine;
    engine.addSymbol(kSym);
    engine.start();  // sync mode is fine — gateway tests dispatch, not throughput

    FixTcpGateway gw(engine);
    assert(gw.start(0));
    uint16_t port = gw.port();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // ── Scenario 1: whole-frame send ────────────────────────────────────────
    {
        int fd = connectTo(port);
        assert(fd >= 0);

        // Logon (seq 1) first — app messages are rejected before Logon.
        auto logon = makeLogon(/*seqNum=*/1);
        assert(sendAll(fd, logon.data(), logon.size()));
        auto logonAck = recvFixFrame(fd, std::chrono::milliseconds(500));
        {
            FixMessage la;
            assert(la.parse(logonAck.data(), logonAck.size()) && "logon ack parse");
            assert(la.getString(FixTag::MsgType) == "A" && "expected Logon ack");
        }

        auto frame = makeNewOrder(/*clOrdId=*/1001, kSym,
                                  /*qty=*/50, /*price=*/1000, /*seqNum=*/2);
        assert(sendAll(fd, frame.data(), frame.size()));

        auto resp = recvFixFrame(fd, std::chrono::milliseconds(500));
        expectAck(resp, 1001);

        auto* book = engine.getOrderBook(kSym);
        assert(book && book->getOrder(1001));
        ::close(fd);
        std::puts("[scenario whole-frame] passed");
    }

    // ── Scenario 2: one-byte-at-a-time send ─────────────────────────────────
    {
        int fd = connectTo(port);
        assert(fd >= 0);

        // Logon (seq 1) first — app messages are rejected before Logon.
        auto logon = makeLogon(/*seqNum=*/1);
        assert(sendAll(fd, logon.data(), logon.size()));
        auto logonAck = recvFixFrame(fd, std::chrono::milliseconds(500));
        {
            FixMessage la;
            assert(la.parse(logonAck.data(), logonAck.size()) && "logon ack parse");
            assert(la.getString(FixTag::MsgType) == "A" && "expected Logon ack");
        }

        // Keep price near the prior order's to avoid the engine's
        // volatility circuit breaker — orthogonal to what this test
        // exercises (gateway framing under fragmentation).
        auto frame = makeNewOrder(/*clOrdId=*/1002, kSym,
                                  /*qty=*/25, /*price=*/1010, /*seqNum=*/2);
        for (size_t i = 0; i < frame.size(); ++i) {
            assert(sendAll(fd, frame.data() + i, 1));
            // Tiny pause so the gateway sees genuinely fragmented arrivals
            // rather than the kernel coalescing them into one read.
            if ((i & 7) == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }

        auto resp = recvFixFrame(fd, std::chrono::milliseconds(500));
        expectAck(resp, 1002);

        auto* book = engine.getOrderBook(kSym);
        assert(book->getOrder(1002));
        ::close(fd);
        std::puts("[scenario one-byte] passed");
    }

    // ── Scenario 3: slow reader must be cut, never corrupted (H8) ───────────
    //
    // The accepted fd is non-blocking, so a client that stops reading makes
    // send() return short mid-ExecutionReport. The old callback returned on
    // the first short write and then wrote the NEXT report straight after the
    // truncated one — a byte stream no FIX client can resynchronise on, with
    // nothing in the protocol to detect it. The gateway must instead stop
    // writing and drop the session; FIX recovers that by seqnum resend.
    {
        // Its own gateway, with a deliberately small socket send buffer. How
        // many bytes it takes to back up a socket is a kernel tuning
        // parameter, not a property of the gateway: macOS backs up after a
        // few hundred KB, Linux auto-tunes into the megabytes, and under
        // sanitizers the difference is the whole 60s test budget. Pinning
        // both ends makes the stall reachable in well under a second
        // everywhere. Production keeps the OS default (the knob defaults off).
        FixTcpGateway slowGw(engine, /*maxFrameSize=*/64 * 1024,
                             /*sendBufferBytes=*/4096);
        assert(slowGw.start(0));
        const uint16_t slowPort = slowGw.port();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        int fd = connectTo(slowPort, /*rcvBuf=*/2048);
        assert(fd >= 0);

        auto logon = makeLogon(/*seqNum=*/1);
        assert(sendAll(fd, logon.data(), logon.size()));
        auto logonAck = recvFixFrame(fd, std::chrono::milliseconds(500));
        {
            FixMessage la;
            assert(la.parse(logonAck.data(), logonAck.size()) && "logon ack parse");
            assert(la.getString(FixTag::MsgType) == "A" && "expected Logon ack");
        }

        // Pump orders from a second thread and never read the replies here.
        // A thread because our own send() will block once the gateway stops
        // draining input to wait on its stalled write.
        //
        // No fixed order count and no fixed sleep: how much data it takes to
        // fill the pipe is a property of the host, not of the code under test.
        // Linux auto-tunes the send buffer into the megabytes, so a cap that
        // stalls macOS sails straight through and the scenario proves nothing.
        // Pump until the pipe actually backs up, and wait on a real signal.
        std::atomic<bool> stopPump{false};
        std::atomic<bool> pumpEnded{false};
        std::thread pump([&] {
            for (uint64_t seq = 2; seq < 200'000 && !stopPump.load(); ++seq) {
                auto f = makeNewOrder(static_cast<OrderId>(2000 + seq), kSym,
                                      /*qty=*/1, /*price=*/1010, seq);
                if (!sendAll(fd, f.data(), f.size())) break;
            }
            pumpEnded.store(true);
        });

        // Read nothing while we wait. The pump ends when the gateway hangs up
        // (our send fails) — that is the outcome under test. If it instead
        // blocks because the gateway is stalled mid-write, the deadline below
        // releases us and the drain unblocks it.
        const auto stallDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!pumpEnded.load() &&
               std::chrono::steady_clock::now() < stallDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        // Now drain to EOF and inspect the shape of what actually arrived.
        std::string stream;
        bool hungUp = false;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        char tmp[4096];
        while (std::chrono::steady_clock::now() < deadline) {
            ssize_t n = ::recv(fd, tmp, sizeof(tmp), MSG_DONTWAIT);
            if (n > 0) { stream.append(tmp, size_t(n)); continue; }
            if (n == 0) { hungUp = true; break; }        // FIN
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            // Closing a socket that still has unread inbound data queued sends
            // RST, not FIN — so a reset is the same signal here.
            if (errno == ECONNRESET) { hungUp = true; }
            break;
        }
        // Unblock the pump before joining. If the gateway did NOT hang up (the
        // bug this scenario exists to catch), our sender is still parked in a
        // blocking send() on a socket nobody is draining, and join() would
        // hang until ctest's timeout kills the whole binary. shutdown() makes
        // that case fail on the assertion below instead, which says why.
        stopPump.store(true);
        ::shutdown(fd, SHUT_RDWR);
        pump.join();
        slowGw.stop();

        // The gateway must have hung up. Without that this scenario proves
        // nothing, so fail loudly rather than pass vacuously.
        assert(hungUp && "gateway did not close the stalled session");

        // Shape check: complete messages, then AT MOST one truncated tail.
        // Any message header appearing after an incomplete message is the
        // corruption this test exists to catch.
        const std::string kHead = std::string("8=FIX");
        size_t pos = 0, complete = 0;
        while (pos < stream.size()) {
            assert(stream.compare(pos, kHead.size(), kHead) == 0 &&
                   "stream did not resume at a message boundary");
            // Trailer is "<SOH>10=NNN<SOH>".
            std::string trailer = std::string(1, FIX_SOH) + "10=";
            size_t t = stream.find(trailer, pos);
            if (t == std::string::npos || t + 8 > stream.size()) {
                // Incomplete tail — allowed, but it must be the LAST thing
                // in the stream (no further message may follow it).
                assert(stream.find(kHead, pos + 1) == std::string::npos &&
                       "a new FIX message was written after a truncated one");
                break;
            }
            pos = t + 8;
            ++complete;
        }
        assert(complete > 0 && "no complete ExecutionReports arrived at all");
        std::printf("[scenario slow-reader] passed (%zu complete msgs, cut cleanly)\n",
                    complete);
        ::close(fd);
    }

    // ── Scenario 4: clean shutdown ──────────────────────────────────────────
    gw.stop();
    engine.stop();

    std::puts("FixTcpGatewayTest passed");
    return 0;
}
