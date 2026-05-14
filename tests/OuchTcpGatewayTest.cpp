// OuchTcpGatewayTest — exercises the full OUCH-over-SoupBinTCP stack
// over real loopback TCP sockets. The in-process tests
// (OuchSessionTest, SoupBinTcpTest) already cover the protocol
// semantics; this test pins:
//
//   1. The gateway actually binds, listens, and accepts.
//   2. A client TCP send is correctly demultiplexed: bytes → SoupBin
//      session → OUCH session → MatchingEngine.
//   3. The reverse path is correctly remultiplexed: OUCH ack → SoupBin
//      Unsequenced packet → TCP send.
//   4. Login rejection cleanly terminates the connection.
//   5. Clean shutdown joins all worker threads without hangs.

#include "MatchingEngine.h"
#include "OuchProtocol.h"
#include "OuchTcpGateway.h"
#include "SoupBinTCP.h"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
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

// ─── TCP client helpers ─────────────────────────────────────────────────────

namespace {

// Connect to 127.0.0.1:port. Returns the connected fd or -1 on
// failure. TCP_NODELAY enabled so test sends aren't buffered.
int tcpConnect(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool sendAll(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        ssize_t n = ::send(fd, p, len, 0);
        if (n <= 0) return false;
        p += n; len -= static_cast<size_t>(n);
    }
    return true;
}

// Read exactly `len` bytes or fail. Uses a deadline so a hung
// gateway can't lock up the test runner.
bool recvAll(int fd, void* out, size_t len, int deadlineMs = 2000) {
    char* p = static_cast<char*>(out);
    auto start = std::chrono::steady_clock::now();
    while (len > 0) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed > deadlineMs) return false;

        timeval tv{0, 100 * 1000};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ssize_t n = ::recv(fd, p, len, 0);
        if (n > 0) { p += n; len -= static_cast<size_t>(n); continue; }
        if (n == 0) return false;  // peer closed
        // EAGAIN/EWOULDBLOCK from timeout — try again until deadline.
    }
    return true;
}

// Read one SoupBinTCP packet: 2 length bytes, then length-many.
// Returns empty vector on failure.
std::vector<uint8_t> recvSoupPacket(int fd) {
    uint8_t lenBuf[2];
    if (!recvAll(fd, lenBuf, 2)) return {};
    uint16_t len = readU16BE(lenBuf);
    if (len == 0) return {};
    std::vector<uint8_t> body(len);
    if (!recvAll(fd, body.data(), len)) return {};
    std::vector<uint8_t> packet(2 + len);
    std::memcpy(packet.data(), lenBuf, 2);
    std::memcpy(packet.data() + 2, body.data(), len);
    return packet;
}

std::vector<uint8_t> buildLogin(const std::string& u, const std::string& p) {
    std::vector<uint8_t> wire(3 + SOUP_SIZE_LOGIN_REQUEST_PAYLOAD);
    soupWriteLoginRequest(wire.data(), u, p, "", 0);
    return wire;
}

std::vector<uint8_t> buildOuchEnterOrder(uint64_t token, SymbolId stock,
                                         uint32_t shares, uint32_t price) {
    // Wrap an OUCH EnterOrder in a SoupBinTCP Unsequenced packet.
    std::vector<uint8_t> ouch(OUCH_SIZE_ENTER_ORDER, ' ');
    ouch[0] = OUCH_MT_ENTER_ORDER;
    writeAsciiDecimal(ouch.data() + 1, 14, token);
    ouch[15] = 'B';
    writeU32BE(ouch.data() + 16, shares);
    writeAsciiDecimal(ouch.data() + 20, 8, static_cast<uint64_t>(stock));
    writeU32BE(ouch.data() + 28, price);
    writeU32BE(ouch.data() + 32, OUCH_TIF_DAY);
    writeAsciiDecimal(ouch.data() + 36, 4, 1);
    ouch[40] = 'Y'; ouch[41] = 'O'; ouch[42] = 'N';
    writeU32BE(ouch.data() + 43, 0);
    ouch[47] = 'N'; ouch[48] = 'R';

    std::vector<uint8_t> wire(3 + OUCH_SIZE_ENTER_ORDER);
    soupWriteEnvelope(wire.data(), SOUP_PT_UNSEQUENCED_DATA,
                      ouch.data(), ouch.size());
    return wire;
}

}  // namespace

// ─── Tests ──────────────────────────────────────────────────────────────────

void test_GatewayBindsAndAccepts() {
    TEST(GatewayBindsAndAccepts) {
        MatchingEngine engine;
        engine.addSymbol(7);
        engine.start();

        OuchTcpGateway gw(engine, "SESS01");
        CHECK(gw.start(/*port=*/0));
        CHECK(gw.boundPort() != 0);

        // Connect and disconnect immediately. The gateway should
        // count the connection regardless of whether we log in.
        int fd = tcpConnect(gw.boundPort());
        CHECK(fd >= 0);

        // Give the gateway a moment to register the accept.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(gw.connectionsAccepted() >= 1);

        ::close(fd);
        gw.stop();
        CHECK(!gw.isRunning());
    } END
}

void test_GatewayLoginAcceptThenOrderRoundtrip() {
    TEST(GatewayLoginAcceptThenOrderRoundtrip) {
        MatchingEngine engine;
        engine.addSymbol(7);
        engine.start();

        OuchTcpGateway gw(engine, "SESSXYZ");
        // Default validator (accept all) — explicit for clarity.
        gw.setLoginValidator([](const SoupLoginRequest&) { return char{0}; });
        CHECK(gw.start(/*port=*/0));

        int fd = tcpConnect(gw.boundPort());
        CHECK(fd >= 0);

        // 1. Client sends LoginRequest.
        auto login = buildLogin("USER", "PASS");
        CHECK(sendAll(fd, login.data(), login.size()));

        // 2. Server replies with LoginAccepted.
        auto loginAck = recvSoupPacket(fd);
        CHECK(loginAck.size() == 3 + SOUP_SIZE_LOGIN_ACCEPTED_PAYLOAD);
        CHECK(loginAck[2] == SOUP_PT_LOGIN_ACCEPTED);

        // 3. Client sends an OUCH EnterOrder.
        auto order = buildOuchEnterOrder(/*token=*/99001, /*stock=*/7,
                                         /*shares=*/100, /*price=*/1000);
        CHECK(sendAll(fd, order.data(), order.size()));

        // 4. Server replies with OUCH OrderAccepted wrapped in
        //    SoupBinTCP Unsequenced.
        auto ackPkt = recvSoupPacket(fd);
        CHECK(ackPkt.size() == 3 + OUCH_SIZE_ORDER_ACCEPTED);
        CHECK(ackPkt[2] == SOUP_PT_UNSEQUENCED_DATA);
        CHECK(ackPkt[3] == OUCH_MT_ORDER_ACCEPTED);

        // 5. The order landed in the engine.
        auto* book = engine.getOrderBook(7);
        CHECK(book != nullptr);
        CHECK(book->getOrder(99001) != nullptr);

        // 6. Client logs out cleanly.
        uint8_t lo[3];
        soupWriteLogoutRequest(lo);
        sendAll(fd, lo, 3);
        auto eosPkt = recvSoupPacket(fd);
        CHECK(eosPkt.size() == 3);
        CHECK(eosPkt[2] == SOUP_PT_END_OF_SESSION);

        ::close(fd);
        gw.stop();

        CHECK(gw.ordersAcceptedTotal() == 1);
        CHECK(gw.ordersRejectedTotal() == 0);
    } END
}

void test_GatewayLoginRejectClosesConnection() {
    TEST(GatewayLoginRejectClosesConnection) {
        MatchingEngine engine;
        engine.start();

        OuchTcpGateway gw(engine, "SESS");
        gw.setLoginValidator([](const SoupLoginRequest&) {
            return SOUP_LOGIN_REJECT_NOT_AUTHORIZED;
        });
        CHECK(gw.start(0));

        int fd = tcpConnect(gw.boundPort());
        CHECK(fd >= 0);

        auto login = buildLogin("BAD", "USER");
        sendAll(fd, login.data(), login.size());

        // Should receive a LoginRejected and then the connection
        // closes (server side disconnects after reject).
        auto rejPkt = recvSoupPacket(fd);
        CHECK(rejPkt.size() == 3 + SOUP_SIZE_LOGIN_REJECTED_PAYLOAD);
        CHECK(rejPkt[2] == SOUP_PT_LOGIN_REJECTED);
        CHECK(rejPkt[3] == SOUP_LOGIN_REJECT_NOT_AUTHORIZED);

        ::close(fd);
        gw.stop();
    } END
}

void test_GatewayCleanShutdownWithActiveConnection() {
    TEST(GatewayCleanShutdownWithActiveConnection) {
        // stop() must not hang even with an active, idle client
        // connection. The gateway should shut down within a few
        // seconds; we'll cap at 5s as the deadline and consider
        // anything past that a regression.
        MatchingEngine engine;
        engine.start();

        OuchTcpGateway gw(engine, "S");
        CHECK(gw.start(0));

        int fd = tcpConnect(gw.boundPort());
        CHECK(fd >= 0);

        // Send a login to put the connection in the logged-in state.
        auto login = buildLogin("U", "P");
        sendAll(fd, login.data(), login.size());
        recvSoupPacket(fd);  // login accepted

        auto t0 = std::chrono::steady_clock::now();
        gw.stop();
        auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        CHECK(dt < 5000 && "stop() must not hang on active connections");
        ::close(fd);
    } END
}

int main() {
    std::cout << "Running OuchTcpGatewayTest\n";

    test_GatewayBindsAndAccepts();
    test_GatewayLoginAcceptThenOrderRoundtrip();
    test_GatewayLoginRejectClosesConnection();
    test_GatewayCleanShutdownWithActiveConnection();

    std::cout << "\n" << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
