// GatewayChaosTest — drive the TCP gateway with fragmented receives and
// spurious EAGAIN faults injected into the recv path. Verifies that the
// length-prefix accumulator in handleClientData reassembles every
// OrderRequest correctly under fragmentation, with no order lost,
// duplicated, or corrupted in transit.
//
// Requires -DENABLE_FAULT_INJECTION=ON. Without it, the faults are no-ops
// and the test still runs (degenerates into a basic happy-path check).

#include "FaultInjector.h"
#include "MatchingEngine.h"
#include "TcpGateway.h"

#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_set>

using namespace OrderMatcher;

namespace {

class Client {
public:
    ~Client() { if (fd_ >= 0) close(fd_); }

    bool connect(uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        int flag = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        return ::connect(fd_,
            reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    }

    bool sendOrder(const OrderRequest& req) {
        uint32_t len = htonl(sizeof(OrderRequest));
        return sendAll(&len, sizeof(len)) && sendAll(&req, sizeof(req));
    }

    bool recvResponse(GatewayResponse& resp) {
        uint32_t len;
        if (!recvAll(&len, sizeof(len))) return false;
        if (ntohl(len) != sizeof(GatewayResponse)) return false;
        return recvAll(&resp, sizeof(resp));
    }

private:
    bool sendAll(const void* buf, size_t len) {
        auto* p = static_cast<const char*>(buf);
        while (len) {
            ssize_t n = ::send(fd_, p, len, 0);
            if (n <= 0) return false;
            p += n; len -= size_t(n);
        }
        return true;
    }
    bool recvAll(void* buf, size_t len) {
        auto* p = static_cast<char*>(buf);
        while (len) {
            ssize_t n = ::recv(fd_, p, len, 0);
            if (n <= 0) return false;
            p += n; len -= size_t(n);
        }
        return true;
    }

    int fd_{-1};
};

}  // namespace

int main() {
#ifndef OB_ENABLE_FAULT_INJECTION
    std::puts("GatewayChaosTest: OB_ENABLE_FAULT_INJECTION not defined — "
              "running degenerate happy-path only. "
              "Reconfigure with -DENABLE_FAULT_INJECTION=ON.");
#endif

    constexpr SymbolId kSymbol = 1;
    constexpr int      kOrders = 500;

    auto& fi = FaultInjector::instance();
    fi.reset();
    fi.seed(0xCA05);
    // Aggressive: 70% chance per recv() of being clamped to 1 byte. This
    // forces the framing accumulator through hundreds of partial-fill
    // iterations per OrderRequest (~104 bytes each). 10% spurious EAGAIN
    // adds extra back-pressure with mid-frame stalls.
    fi.arm("gateway.recv.short_read", 0.7);
    fi.arm("gateway.recv.eagain",     0.1);

    MatchingEngine engine;
    engine.start();
    engine.addSymbol(kSymbol);

    TcpGateway gateway(engine);
    assert(gateway.start(0));
    uint16_t port = gateway.port();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    Client client;
    assert(client.connect(port));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::unordered_set<uint64_t> ackedIds;
    ackedIds.reserve(kOrders);

    [[maybe_unused]] auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOrders; ++i) {
        OrderRequest req{};
        req.type = OrderRequest::Type::NewOrder;
        req.symbolId = kSymbol;
        req.orderId = static_cast<OrderId>(i + 1);
        req.participantId = 7;
        req.side = (i & 1) ? Side::Sell : Side::Buy;
        req.price = 10000 + (i % 100);  // wide enough to avoid crossing
        req.qty = 1;
        req.orderType = OrderType::Limit;

        assert(client.sendOrder(req) && "send failed under fault injection");

        GatewayResponse resp{};
        assert(client.recvResponse(resp) && "recv response failed");
        // Every order must come back with a matching orderId — proves the
        // framing accumulator reassembled the right bytes despite the
        // fragmentation introduced by short_read/eagain.
        assert(resp.orderId == req.orderId &&
               "response orderId mismatch — framing corruption");
        // No duplicate acks — engine must not see the same order twice.
        assert(ackedIds.insert(resp.orderId).second &&
               "duplicate ack — order processed twice");
    }
    [[maybe_unused]] auto t1 = std::chrono::steady_clock::now();

    assert(ackedIds.size() == static_cast<size_t>(kOrders));

#ifdef OB_ENABLE_FAULT_INJECTION
    uint64_t shorts = fi.activations("gateway.recv.short_read");
    uint64_t eagains = fi.activations("gateway.recv.eagain");
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::printf("delivered %d orders in %lld ms; "
                "short_read fired %llu times, eagain fired %llu times\n",
                kOrders, static_cast<long long>(ms),
                static_cast<unsigned long long>(shorts),
                static_cast<unsigned long long>(eagains));
    // Each ~104-byte order should produce many short-read fires; demand a
    // generous lower bound to confirm the fault wiring is hot.
    assert(shorts > 100 && "short_read never fired — wiring broken");
#endif

    gateway.stop();
    engine.stop();

    std::puts("GatewayChaosTest passed");
    return 0;
}
