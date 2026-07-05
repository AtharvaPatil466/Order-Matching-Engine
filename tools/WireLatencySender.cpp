//
// WireLatencySender — Instance A of the OUCH wire-to-wire latency harness.
//
// Sends synthetic OUCH EnterOrder messages to a WireLatencyReceiver, captures
// the hardware TX timestamp (SO_TIMESTAMPING error queue), receives the
// OrderAccepted ack, captures the hardware RX timestamp, and logs per-order
// round-trip latency plus a one-way estimate (round-trip / 2).
//
// One-way = round-trip / 2 sidesteps clock synchronisation entirely: both TX
// and RX timestamps come from *this* host's clock, so no PTP is needed for a
// first-pass estimate. True one-way (sender-TX vs receiver-RX) needs PTP and is
// a later step — the receiver already logs the correlating RX timestamps.
//
// Linux uses NIC hardware timestamps; other platforms fall back to
// CLOCK_MONOTONIC software timestamps (see WireTimestamp.h). Build with
// -DBUILD_WIRE_LATENCY_TOOLS=ON.
//
// Orders are sent back-to-back: each is sent immediately after the previous
// order's ack is received (the recv is the only barrier). No pacing / sleep /
// rate limiting between orders.
//
// Usage: WireLatencySender [--host H] [--port P] [--count N]
//                          [--shares Q] [--price P] [--stock S] [--firm F]

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

constexpr int kTxTimestampTimeoutMs = 100;  // errqueue wait (post round-trip; normally µs)

struct Config {
    std::string host = "127.0.0.1";
    uint16_t    port = 12345;
    uint64_t    count = 1000;
    uint32_t    shares = 100;
    uint32_t    price = 100000;
    uint64_t    stock = 1;
    uint64_t    firm = 1;
    bool        softwareOnly = false;  // force CLOCK_MONOTONIC (match the DPDK path)
};

Config parseArgs(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--host") c.host = next();
        else if (a == "--port") c.port = static_cast<uint16_t>(std::atoi(next()));
        else if (a == "--count") c.count = std::strtoull(next(), nullptr, 10);
        else if (a == "--shares") c.shares = static_cast<uint32_t>(std::atoi(next()));
        else if (a == "--price") c.price = static_cast<uint32_t>(std::atoi(next()));
        else if (a == "--stock") c.stock = std::strtoull(next(), nullptr, 10);
        else if (a == "--firm") c.firm = std::strtoull(next(), nullptr, 10);
        else if (a == "--software-timestamps") c.softwareOnly = true;
        else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
        }
    }
    return c;
}

// Send all `len` bytes; returns false on error/disconnect.
bool sendAll(int fd, const uint8_t* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, buf + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

int connectTo(const Config& c) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::perror("socket");
        return -1;
    }
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(c.port);
    if (::inet_pton(AF_INET, c.host.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "invalid host: %s\n", c.host.c_str());
        ::close(fd);
        return -1;
    }
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("connect");
        ::close(fd);
        return -1;
    }
    int flag = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));  // Nagle off — latency
    return fd;
}

}  // namespace

int main(int argc, char** argv) {
    Config c = parseArgs(argc, argv);

    int fd = connectTo(c);
    if (fd < 0) return 1;

    // --software-timestamps: skip SO_TIMESTAMPING so timestamps come from
    // CLOCK_MONOTONIC (softwareNowNs) and the TX errqueue is not polled — same
    // basis as the DPDK receiver path, for an apples-to-apples comparison.
    if (!c.softwareOnly) {
        bool hwEnabled = wirelat::enableTimestamping(fd);
        wirelat::printTimestampBanner(hwEnabled);
    } else {
        std::printf("[wire-latency] software timestamps forced "
                    "(--software-timestamps): CLOCK_MONOTONIC; 'hardware' column = 0\n");
    }
    std::printf("[wire-latency] sender -> %s:%u, %llu orders\n",
                c.host.c_str(), static_cast<unsigned>(c.port),
                static_cast<unsigned long long>(c.count));
    std::printf("seq,tx_ns,rx_ns,rtt_ns,oneway_ns,hardware\n");

    uint8_t orderBuf[OUCH_SIZE_ENTER_ORDER];
    uint8_t ackBuf[OUCH_SIZE_ORDER_ACCEPTED];
    uint64_t completed = 0;

    for (uint64_t seq = 1; seq <= c.count; ++seq) {
        OuchEnterOrder o;
        o.orderToken = seq;
        o.side = (seq & 1) ? Side::Buy : Side::Sell;
        o.shares = c.shares;
        o.stock = static_cast<SymbolId>(c.stock);
        o.price = static_cast<Price>(c.price);
        o.timeInForce = OUCH_TIF_DAY;
        o.firm = static_cast<ParticipantId>(c.firm);
        encodeEnterOrder(orderBuf, o);

        // Software send-time — the TX fallback if the errqueue HW timestamp is
        // unavailable (non-Linux, or HW timestamping off).
        uint64_t softwareTx = wirelat::softwareNowNs();

        if (!sendAll(fd, orderBuf, OUCH_SIZE_ENTER_ORDER)) {
            std::fprintf(stderr, "send failed at seq=%llu\n",
                         static_cast<unsigned long long>(seq));
            break;
        }

        // Block for the ack — this is also the round-trip barrier. In software
        // mode use a plain recv (recvFrameSoftware): zero SO_TIMESTAMPING /
        // errqueue / poll machinery, so the hot path is exactly
        // softwareNowNs → send → recv → softwareNowNs → record. In HW mode the
        // errqueue TX timestamp is ready to collect immediately after this.
        uint64_t rxNs = 0;
        bool rxHw = false;
        bool rxOk = c.softwareOnly
            ? wirelat::recvFrameSoftware(fd, ackBuf, OUCH_SIZE_ORDER_ACCEPTED, rxNs)
            : wirelat::recvFrameWithTs(fd, ackBuf, OUCH_SIZE_ORDER_ACCEPTED, rxNs, rxHw);
        if (!rxOk) {
            std::fprintf(stderr, "recv ack failed at seq=%llu\n",
                         static_cast<unsigned long long>(seq));
            break;
        }

        OuchOrderAccepted ack;
        if (!decodeOrderAccepted(ackBuf, OUCH_SIZE_ORDER_ACCEPTED, ack)) {
            std::fprintf(stderr, "malformed ack at seq=%llu\n",
                         static_cast<unsigned long long>(seq));
            break;
        }
        if (ack.orderToken != seq) {
            std::fprintf(stderr, "ack token mismatch: expected %llu got %llu\n",
                         static_cast<unsigned long long>(seq),
                         static_cast<unsigned long long>(ack.orderToken));
        }

        uint64_t txNs = softwareTx;
        bool txHw = false;
        if (!c.softwareOnly) {
            if (!wirelat::collectTxTimestamp(fd, txNs, txHw, kTxTimestampTimeoutMs)) {
                txNs = softwareTx;   // errqueue unavailable — software send-time
                txHw = false;
            }
        }

        const bool hardware = txHw && rxHw;
        const uint64_t rtt = (rxNs >= txNs) ? (rxNs - txNs) : 0;
        const uint64_t oneWay = rtt / 2;

        std::printf("%llu,%llu,%llu,%llu,%llu,%d\n",
                    static_cast<unsigned long long>(seq),
                    static_cast<unsigned long long>(txNs),
                    static_cast<unsigned long long>(rxNs),
                    static_cast<unsigned long long>(rtt),
                    static_cast<unsigned long long>(oneWay),
                    hardware ? 1 : 0);
        ++completed;
    }

    std::printf("[wire-latency] done: %llu/%llu orders completed\n",
                static_cast<unsigned long long>(completed),
                static_cast<unsigned long long>(c.count));
    ::close(fd);
    return 0;
}
