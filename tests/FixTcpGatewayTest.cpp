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

using namespace OrderMatcher;

namespace {

int connectTo(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
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
        ssize_t n = ::send(fd, data, len, 0);
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

std::string makeNewOrder(OrderId clOrdId, SymbolId sym, Quantity qty, Price price) {
    std::string body;
    body += "35=D"; body += FIX_SOH;
    body += "49=100"; body += FIX_SOH;     // SenderCompID = participantId
    body += "11=" + std::to_string(clOrdId); body += FIX_SOH;
    body += "55=" + std::to_string(sym);     body += FIX_SOH;
    body += "54=1"; body += FIX_SOH;         // Buy
    body += "38=" + std::to_string(qty);     body += FIX_SOH;
    body += "44=" + std::to_string(price);   body += FIX_SOH;
    body += "40=2"; body += FIX_SOH;         // Limit
    body += "59=1"; body += FIX_SOH;         // GTC
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

        auto frame = makeNewOrder(/*clOrdId=*/1001, kSym,
                                  /*qty=*/50, /*price=*/1000);
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

        // Keep price near the prior order's to avoid the engine's
        // volatility circuit breaker — orthogonal to what this test
        // exercises (gateway framing under fragmentation).
        auto frame = makeNewOrder(/*clOrdId=*/1002, kSym,
                                  /*qty=*/25, /*price=*/1010);
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

    // ── Scenario 3: clean shutdown ──────────────────────────────────────────
    gw.stop();
    engine.stop();

    std::puts("FixTcpGatewayTest passed");
    return 0;
}
