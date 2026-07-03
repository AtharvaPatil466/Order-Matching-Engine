//
// WireLatencyReceiver — Instance B of the OUCH wire-to-wire latency harness.
//
// A minimal OUCH ack server: accepts one TCP connection, and for each arriving
// EnterOrder it captures the hardware RX timestamp (order arrival), immediately
// sends an OrderAccepted echoing the order token, and captures the hardware TX
// timestamp (ack sent). Both timestamps are logged per order so post-processing
// can separate receiver-side processing time (tx_ack - rx_arrival) from network
// transit time.
//
// This isolates the wire path; a later iteration can point the sender at the
// real matching engine's OUCH port to fold in engine processing. Runs on the
// same instance that hosts the engine (a separate port).
//
// Linux uses NIC hardware timestamps; other platforms fall back to
// CLOCK_MONOTONIC software timestamps (see WireTimestamp.h). Build with
// -DBUILD_WIRE_LATENCY_TOOLS=ON.
//
// Usage: WireLatencyReceiver [--port P] [--count N]   (N=0 → until disconnect)

#include "OuchProtocol.h"
#include "WireTimestamp.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace OrderMatcher;

namespace {

constexpr int kTxTimestampTimeoutMs = 100;  // errqueue wait for the ack TX timestamp

struct Config {
    uint16_t port = 12345;
    uint64_t count = 0;  // 0 = run until the peer disconnects
};

Config parseArgs(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--port") c.port = static_cast<uint16_t>(std::atoi(next()));
        else if (a == "--count") c.count = std::strtoull(next(), nullptr, 10);
        else std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
    }
    return c;
}

bool sendAll(int fd, const uint8_t* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, buf + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Bind + listen; return the listening fd or -1.
int listenOn(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::perror("socket");
        return -1;
    }
    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 1) < 0) {
        std::perror("listen");
        ::close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

int main(int argc, char** argv) {
    Config c = parseArgs(argc, argv);

    int listenFd = listenOn(c.port);
    if (listenFd < 0) return 1;
    std::printf("[wire-latency] receiver listening on port %u\n",
                static_cast<unsigned>(c.port));

    int clientFd = ::accept(listenFd, nullptr, nullptr);
    if (clientFd < 0) {
        std::perror("accept");
        ::close(listenFd);
        return 1;
    }
    int flag = 1;
    ::setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));  // Nagle off

    bool hwEnabled = wirelat::enableTimestamping(clientFd);
    wirelat::printTimestampBanner(hwEnabled);
    std::printf("[wire-latency] client connected\n");
    std::printf("seq,rx_arrival_ns,tx_ack_ns,processing_ns,hardware\n");

    uint8_t orderBuf[OUCH_SIZE_ENTER_ORDER];
    uint8_t ackBuf[OUCH_SIZE_ORDER_ACCEPTED];
    uint64_t processed = 0;

    for (;;) {
        // RX timestamp = order arrival at the NIC.
        uint64_t rxNs = 0;
        bool rxHw = false;
        if (!wirelat::recvFrameWithTs(clientFd, orderBuf, OUCH_SIZE_ENTER_ORDER, rxNs, rxHw)) {
            break;  // peer disconnected / error
        }

        OuchEnterOrder o;
        if (!decodeEnterOrder(orderBuf, OUCH_SIZE_ENTER_ORDER, o)) {
            std::fprintf(stderr, "malformed EnterOrder — stopping\n");
            break;
        }

        // Echo an OrderAccepted immediately; engine-side id == order token.
        encodeOrderAccepted(ackBuf, wirelat::softwareNowNs(), o.orderToken, o.side,
                            o.shares, o.stock, o.price, o.timeInForce, o.firm,
                            /*orderReferenceNumber=*/o.orderToken);

        uint64_t softwareTxAck = wirelat::softwareNowNs();  // TX fallback
        if (!sendAll(clientFd, ackBuf, OUCH_SIZE_ORDER_ACCEPTED)) {
            std::fprintf(stderr, "send ack failed at seq=%llu\n",
                         static_cast<unsigned long long>(o.orderToken));
            break;
        }

        // TX timestamp = ack leaving the NIC (errqueue), else software send-time.
        uint64_t txAckNs = 0;
        bool txHw = false;
        if (!wirelat::collectTxTimestamp(clientFd, txAckNs, txHw, kTxTimestampTimeoutMs)) {
            txAckNs = softwareTxAck;
            txHw = false;
        }

        const bool hardware = rxHw && txHw;
        const uint64_t processing = (txAckNs >= rxNs) ? (txAckNs - rxNs) : 0;

        std::printf("%llu,%llu,%llu,%llu,%d\n",
                    static_cast<unsigned long long>(o.orderToken),
                    static_cast<unsigned long long>(rxNs),
                    static_cast<unsigned long long>(txAckNs),
                    static_cast<unsigned long long>(processing),
                    hardware ? 1 : 0);

        ++processed;
        if (c.count && processed >= c.count) break;
    }

    std::printf("[wire-latency] receiver done: %llu orders processed\n",
                static_cast<unsigned long long>(processed));
    ::close(clientFd);
    ::close(listenFd);
    return 0;
}
