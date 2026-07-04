//
// WireLatencyReceiver — Instance B of the OUCH wire-to-wire latency harness.
//
// A minimal OUCH ack server: for each arriving EnterOrder it captures the RX
// (arrival) timestamp, immediately sends an OrderAccepted echoing the order
// token, and captures the TX (ack) timestamp. Both are logged per order so
// post-processing can separate receiver-side processing (tx_ack - rx_arrival)
// from network transit. Pure echo — NO matching engine — so the numbers isolate
// the wire + stack.
//
// Two receive paths, SAME CSV output (seq,rx_arrival_ns,tx_ack_ns,
// processing_ns,hardware) so the kernel and DPDK sessions are directly
// comparable:
//   * default            — kernel sockets; Linux uses SO_TIMESTAMPING hardware
//                           timestamps, else CLOCK_MONOTONIC software fallback.
//   * --conf <path>       — (OB_HAVE_DPDK builds only) F-Stack / DPDK userspace
//                           TCP on a DPDK-bound ENI. F-Stack's ff_recv BSD API
//                           does NOT expose NIC hardware timestamps or kernel
//                           SO_TIMESTAMPING, so this path uses CLOCK_MONOTONIC
//                           software timestamps (the 'hardware' column reads 0).
//                           For a strict apples-to-apples comparison, force the
//                           kernel side to software timestamps too.
//
// Build the DPDK path with -DENABLE_DPDK=ON -DBUILD_WIRE_LATENCY_TOOLS=ON.
//
// Usage: WireLatencyReceiver [--port P] [--count N] [--conf <f-stack.conf>]
//        (N=0 → until disconnect)

#include "OuchProtocol.h"
#include "WireTimestamp.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace OrderMatcher;

namespace {

constexpr int kTxTimestampTimeoutMs = 100;  // errqueue wait for the ack TX timestamp

struct Config {
    uint16_t    port = 12345;
    uint64_t    count = 0;   // 0 = run until the peer disconnects
    std::string conf;        // non-empty selects the F-Stack/DPDK path
    bool        softwareOnly = false;  // force CLOCK_MONOTONIC on the kernel path
};

Config parseArgs(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--port") c.port = static_cast<uint16_t>(std::atoi(next()));
        else if (a == "--count") c.count = std::strtoull(next(), nullptr, 10);
        else if (a == "--conf") c.conf = next();
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

// Bind + listen (kernel sockets); return the listening fd or -1.
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

// ─── Kernel-socket receive path (default) ───────────────────────────────────
int runKernelReceiver(const Config& c) {
    int listenFd = listenOn(c.port);
    if (listenFd < 0) return 1;
    std::printf("[wire-latency] receiver (kernel) listening on port %u\n",
                static_cast<unsigned>(c.port));

    int clientFd = ::accept(listenFd, nullptr, nullptr);
    if (clientFd < 0) {
        std::perror("accept");
        ::close(listenFd);
        return 1;
    }
    int flag = 1;
    ::setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));  // Nagle off

    // --software-timestamps: skip SO_TIMESTAMPING entirely so recvFrameWithTs
    // falls back to CLOCK_MONOTONIC and the TX errqueue is not polled — same
    // basis as the DPDK path, for an apples-to-apples comparison.
    if (!c.softwareOnly) {
        bool hwEnabled = wirelat::enableTimestamping(clientFd);
        wirelat::printTimestampBanner(hwEnabled);
    } else {
        std::printf("[wire-latency] software timestamps forced "
                    "(--software-timestamps): CLOCK_MONOTONIC; 'hardware' column = 0\n");
    }
    std::printf("[wire-latency] client connected\n");
    std::printf("seq,rx_arrival_ns,tx_ack_ns,processing_ns,hardware\n");

    uint8_t orderBuf[OUCH_SIZE_ENTER_ORDER];
    uint8_t ackBuf[OUCH_SIZE_ORDER_ACCEPTED];
    uint64_t processed = 0;

    for (;;) {
        // Software mode: plain recv + softwareNowNs (recvFrameSoftware) — zero
        // recvmsg/cmsg/SO_TIMESTAMPING/errqueue machinery on the hot path,
        // matching the sender.
        uint64_t rxNs = 0;
        bool rxHw = false;
        bool rxOk = c.softwareOnly
            ? wirelat::recvFrameSoftware(clientFd, orderBuf, OUCH_SIZE_ENTER_ORDER, rxNs)
            : wirelat::recvFrameWithTs(clientFd, orderBuf, OUCH_SIZE_ENTER_ORDER, rxNs, rxHw);
        if (!rxOk) {
            break;  // peer disconnected / error
        }

        OuchEnterOrder o;
        if (!decodeEnterOrder(orderBuf, OUCH_SIZE_ENTER_ORDER, o)) {
            std::fprintf(stderr, "malformed EnterOrder — stopping\n");
            break;
        }

        encodeOrderAccepted(ackBuf, wirelat::softwareNowNs(), o.orderToken, o.side,
                            o.shares, o.stock, o.price, o.timeInForce, o.firm,
                            /*orderReferenceNumber=*/o.orderToken);

        uint64_t softwareTxAck = wirelat::softwareNowNs();  // TX fallback
        if (!sendAll(clientFd, ackBuf, OUCH_SIZE_ORDER_ACCEPTED)) {
            std::fprintf(stderr, "send ack failed at seq=%llu\n",
                         static_cast<unsigned long long>(o.orderToken));
            break;
        }

        uint64_t txAckNs = softwareTxAck;
        bool txHw = false;
        if (!c.softwareOnly) {
            if (!wirelat::collectTxTimestamp(clientFd, txAckNs, txHw, kTxTimestampTimeoutMs)) {
                txAckNs = softwareTxAck;
                txHw = false;
            }
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

}  // namespace

// ─── F-Stack / DPDK receive path (--conf, OB_HAVE_DPDK builds only) ──────────
// Self-contained: mirrors DpdkGateway's F-Stack lifecycle (ff_init/ff_run/
// ff_epoll/ff_accept/ff_recv/ff_send) but stays a PURE ECHO (no engine) so its
// CSV is directly comparable to runKernelReceiver above.
#if defined(OB_HAVE_DPDK)

extern "C" {
#include "ff_api.h"
#include "ff_epoll.h"
}

namespace {

constexpr int kMaxEvents = 64;

struct DpdkEchoReceiver {
    uint16_t port = 0;
    uint64_t count = 0;
    int      listenFd = -1;
    int      epollFd = -1;
    uint64_t processed = 0;
    bool     done = false;

    struct Conn {
        std::vector<char> rx;         // accumulates until a whole 49-byte frame
        std::vector<char> pendingTx;  // ack bytes ff_send could not take yet
    };
    std::unordered_map<int, Conn> conns;

    static int trampoline(void* arg) {
        return static_cast<DpdkEchoReceiver*>(arg)->loop();
    }

    int loop() {
        // BUSY-POLL — ff_epoll_wait timeout is 0 ON PURPOSE (same rationale as
        // DpdkGateway): the lcore spins on F-Stack's RX path and never sleeps,
        // eliminating wakeup latency. 100% CPU on the pinned/isolated lcore is
        // expected and correct. Do NOT change to a blocking timeout.
        if (done) return -1;  // non-zero → ff_run() returns, tool exits cleanly

        struct epoll_event events[kMaxEvents];
        int n = ff_epoll_wait(epollFd, events, kMaxEvents, /*timeout_ms=*/0);
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            if (fd == listenFd) {
                onAccept();
            } else if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                closeConn(fd);
            } else {
                if (events[i].events & EPOLLIN)  onReadable(fd);
                if (events[i].events & EPOLLOUT) onWritable(fd);
            }
            if (done) return -1;
        }
        return 0;
    }

    void onAccept() {
        for (;;) {
            int cfd = ff_accept(listenFd, nullptr, nullptr);
            if (cfd < 0) break;
            int on = 1;
            ff_ioctl(cfd, FIONBIO, &on);
            ff_setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
            conns[cfd];  // default-construct connection state
            struct epoll_event ev;
            std::memset(&ev, 0, sizeof(ev));
            ev.events = EPOLLIN;
            ev.data.fd = cfd;
            ff_epoll_ctl(epollFd, EPOLL_CTL_ADD, cfd, &ev);
        }
    }

    void onReadable(int cfd) {
        auto it = conns.find(cfd);
        if (it == conns.end()) return;
        Conn& conn = it->second;

        char tmp[16 * 1024];
        for (;;) {
            ssize_t n = ff_recv(cfd, tmp, sizeof(tmp), 0);
            if (n > 0) {
                // Software RX-arrival ts for the bytes in this recv. On the
                // strict OUCH ping-pong workload a 49-byte order arrives in one
                // segment, so this equals first-segment arrival.
                uint64_t rxNs = wirelat::softwareNowNs();
                conn.rx.insert(conn.rx.end(), tmp, tmp + n);
                while (conn.rx.size() >= OUCH_SIZE_ENTER_ORDER) {
                    OuchEnterOrder o;
                    if (!decodeEnterOrder(
                            reinterpret_cast<const uint8_t*>(conn.rx.data()),
                            OUCH_SIZE_ENTER_ORDER, o)) {
                        std::fprintf(stderr, "[wire-latency] malformed EnterOrder — closing\n");
                        closeConn(cfd);
                        return;
                    }
                    uint8_t ackBuf[OUCH_SIZE_ORDER_ACCEPTED];
                    encodeOrderAccepted(ackBuf, wirelat::softwareNowNs(), o.orderToken,
                                        o.side, o.shares, o.stock, o.price,
                                        o.timeInForce, o.firm,
                                        /*orderReferenceNumber=*/o.orderToken);
                    uint64_t txNs = wirelat::softwareNowNs();  // software send-time
                    sendOrQueue(cfd, ackBuf, OUCH_SIZE_ORDER_ACCEPTED);

                    const uint64_t processing = (txNs >= rxNs) ? (txNs - rxNs) : 0;
                    // hardware column is 0 — F-Stack ff_recv exposes no HW timestamp.
                    std::printf("%llu,%llu,%llu,%llu,%d\n",
                                static_cast<unsigned long long>(o.orderToken),
                                static_cast<unsigned long long>(rxNs),
                                static_cast<unsigned long long>(txNs),
                                static_cast<unsigned long long>(processing), 0);

                    conn.rx.erase(conn.rx.begin(),
                                  conn.rx.begin() + OUCH_SIZE_ENTER_ORDER);
                    ++processed;
                    if (count && processed >= count) { done = true; return; }
                }
            } else if (n == 0) {
                closeConn(cfd);
                return;
            } else {
                break;  // EAGAIN — RX drained
            }
        }
    }

    void sendOrQueue(int cfd, const uint8_t* data, size_t len) {
        auto it = conns.find(cfd);
        if (it == conns.end()) return;
        Conn& conn = it->second;
        if (!conn.pendingTx.empty()) {
            conn.pendingTx.insert(conn.pendingTx.end(), data, data + len);
            return;
        }
        ssize_t sent = ff_send(cfd, data, len, 0);
        size_t off = (sent > 0) ? static_cast<size_t>(sent) : 0;
        if (off < len) {
            conn.pendingTx.insert(conn.pendingTx.end(), data + off, data + len);
            struct epoll_event ev;
            std::memset(&ev, 0, sizeof(ev));
            ev.events = EPOLLIN | EPOLLOUT;
            ev.data.fd = cfd;
            ff_epoll_ctl(epollFd, EPOLL_CTL_MOD, cfd, &ev);
        }
    }

    void onWritable(int cfd) {
        auto it = conns.find(cfd);
        if (it == conns.end()) return;
        Conn& conn = it->second;
        while (!conn.pendingTx.empty()) {
            ssize_t sent = ff_send(cfd, conn.pendingTx.data(), conn.pendingTx.size(), 0);
            if (sent <= 0) return;
            conn.pendingTx.erase(conn.pendingTx.begin(), conn.pendingTx.begin() + sent);
        }
        struct epoll_event ev;
        std::memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.fd = cfd;
        ff_epoll_ctl(epollFd, EPOLL_CTL_MOD, cfd, &ev);
    }

    void closeConn(int cfd) {
        ff_epoll_ctl(epollFd, EPOLL_CTL_DEL, cfd, nullptr);
        ff_close(cfd);
        conns.erase(cfd);
        if (count == 0) done = true;  // until-disconnect: the sender finished
    }
};

int runDpdkReceiver(int argc, char** argv, const Config& c) {
    // F-Stack reads its EAL / port / lcore config from the --conf file via
    // ff_init(argc, argv); it consumes --conf from argv and ignores the rest.
    if (ff_init(argc, argv) < 0) {
        std::fprintf(stderr, "[wire-latency] ff_init failed (check --conf %s, hugepages, vfio bind)\n",
                     c.conf.c_str());
        return 1;
    }

    DpdkEchoReceiver recv;
    recv.port = c.port;
    recv.count = c.count;

    recv.listenFd = ff_socket(AF_INET, SOCK_STREAM, 0);
    if (recv.listenFd < 0) { std::fprintf(stderr, "[wire-latency] ff_socket failed\n"); return 1; }
    int on = 1;
    ff_setsockopt(recv.listenFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    ff_ioctl(recv.listenFd, FIONBIO, &on);

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(c.port);
    if (ff_bind(recv.listenFd, reinterpret_cast<struct linux_sockaddr*>(&addr),
                sizeof(addr)) < 0 ||
        ff_listen(recv.listenFd, 128) < 0) {
        std::fprintf(stderr, "[wire-latency] ff_bind/ff_listen failed on port %u\n",
                     static_cast<unsigned>(c.port));
        return 1;
    }

    recv.epollFd = ff_epoll_create(0);
    struct epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = recv.listenFd;
    ff_epoll_ctl(recv.epollFd, EPOLL_CTL_ADD, recv.listenFd, &ev);

    std::printf("[wire-latency] receiver (DPDK/F-Stack) listening on port %u\n",
                static_cast<unsigned>(c.port));
    std::printf("[wire-latency] software CLOCK_MONOTONIC timestamps "
                "(F-Stack ff_recv exposes no NIC hardware timestamp; 'hardware' column = 0)\n");
    std::printf("seq,rx_arrival_ns,tx_ack_ns,processing_ns,hardware\n");

    // ff_run takes over this thread and returns only when loop() returns
    // non-zero (done — count reached or peer disconnected).
    ff_run(&DpdkEchoReceiver::trampoline, &recv);

    std::printf("[wire-latency] receiver done: %llu orders processed\n",
                static_cast<unsigned long long>(recv.processed));
    return 0;
}

}  // namespace

#endif  // OB_HAVE_DPDK

int main(int argc, char** argv) {
    Config c = parseArgs(argc, argv);

#if defined(OB_HAVE_DPDK)
    if (!c.conf.empty()) {
        return runDpdkReceiver(argc, argv, c);  // F-Stack / DPDK path
    }
#else
    if (!c.conf.empty()) {
        std::fprintf(stderr,
            "[wire-latency] --conf given but this binary was built WITHOUT DPDK "
            "(OB_HAVE_DPDK undefined) — running the kernel-socket receiver instead. "
            "Rebuild with -DENABLE_DPDK=ON for the F-Stack path.\n");
    }
#endif

    return runKernelReceiver(c);
}
