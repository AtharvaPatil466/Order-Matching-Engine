#include "DpdkGateway.h"

// Entire translation unit is empty unless OB_HAVE_DPDK is defined, so this file
// compiles cleanly on macOS / any non-DPDK build and contributes no symbols.
#if defined(OB_HAVE_DPDK)

// F-Stack public API. F-Stack owns the ENA port + FreeBSD TCP stack; we use its
// socket/epoll shim. Signatures per ff_api.h / ff_epoll.h — validate against the
// pinned F-Stack version on AWS.
extern "C" {
#include "ff_api.h"
#include "ff_epoll.h"
}

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>

namespace OrderMatcher {

namespace {
constexpr int    kMaxEvents   = 64;
constexpr size_t kRecvBufSize = 16 * 1024;
}  // namespace

DpdkGateway::DpdkGateway(MatchingEngine& engine, DpdkConfig config)
    : engine_(engine), config_(config) {}

DpdkGateway::~DpdkGateway() { stop(); }

bool DpdkGateway::start(int argc, char** argv) {
    if (running_.load(std::memory_order_acquire)) return false;
    stopRequested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    // F-Stack must initialise and run on the thread that owns its lcore, and
    // ff_run() never returns — so init + listen-socket setup + ff_run all happen
    // on this dedicated thread. (F-Stack's lcore/threading model may require the
    // affinity configured in the --conf; validate on AWS.)
    lcoreThread_ = std::thread([this, argc, argv]() {
        if (ff_init(argc, argv) < 0) {
            running_.store(false, std::memory_order_release);
            return;
        }

        listenFd_ = ff_socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ < 0) { running_.store(false, std::memory_order_release); return; }

        int on = 1;
        ff_setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        ff_ioctl(listenFd_, FIONBIO, &on);  // non-blocking

        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(config_.port);
        if (ff_bind(listenFd_, reinterpret_cast<struct linux_sockaddr*>(&addr),
                    sizeof(addr)) < 0 ||
            ff_listen(listenFd_, config_.backlog) < 0) {
            ff_close(listenFd_);
            listenFd_ = -1;
            running_.store(false, std::memory_order_release);
            return;
        }

        epollFd_ = ff_epoll_create(0);
        struct epoll_event ev;
        std::memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.fd = listenFd_;
        ff_epoll_ctl(epollFd_, EPOLL_CTL_ADD, listenFd_, &ev);

        // Hand the lcore to F-Stack — it calls loopTrampoline() every iteration
        // and returns only when loop() returns non-zero (stop requested).
        ff_run(&DpdkGateway::loopTrampoline, this);
        running_.store(false, std::memory_order_release);
    });
    return true;
}

void DpdkGateway::stop() {
    if (!lcoreThread_.joinable()) return;
    stopRequested_.store(true, std::memory_order_release);
    lcoreThread_.join();
    running_.store(false, std::memory_order_release);
}

int DpdkGateway::loopTrampoline(void* arg) {
    return static_cast<DpdkGateway*>(arg)->loop();
}

int DpdkGateway::loop() {
    // BUSY-POLL — ff_epoll_wait timeout is 0 ON PURPOSE. A zero timeout makes
    // this a non-blocking poll, so the lcore spins continuously on F-Stack's RX
    // path and never sleeps: this eliminates the interrupt/wakeup latency that
    // the whole kernel-bypass path exists to remove. 100% CPU on this lcore is
    // EXPECTED AND CORRECT — the lcore is pinned to an isolated core (isolcpus)
    // dedicated to ingestion. Do NOT "optimise" this into a blocking timeout to
    // save CPU: that reintroduces exactly the wakeup latency we are eliminating.
    if (stopRequested_.load(std::memory_order_acquire)) {
        return -1;  // non-zero → ff_run() returns, unwinding the lcore thread
    }

    struct epoll_event events[kMaxEvents];
    int n = ff_epoll_wait(epollFd_, events, kMaxEvents, /*timeout_ms=*/0);
    for (int i = 0; i < n; ++i) {
        int fd = events[i].data.fd;
        if (fd == listenFd_) {
            onAccept();
        } else if (events[i].events & (EPOLLHUP | EPOLLERR)) {
            closeConn(fd);
        } else {
            if (events[i].events & EPOLLIN)  onReadable(fd);
            if (events[i].events & EPOLLOUT) onWritable(fd);
        }
    }
    return 0;
}

void DpdkGateway::onAccept() {
    for (;;) {
        int cfd = ff_accept(listenFd_, nullptr, nullptr);
        if (cfd < 0) break;  // EAGAIN — no more pending connections

        int on = 1;
        ff_ioctl(cfd, FIONBIO, &on);
        ff_setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));  // Nagle off

        // Per-connection OuchSession — parser reused UNCHANGED; its send
        // callback routes response bytes to ff_send on this fd.
        Conn& conn = conns_[cfd];
        conn.session = std::make_unique<OuchSession>(
            engine_, [this, cfd](std::string_view sv) { queueTx(cfd, sv); });

        struct epoll_event ev;
        std::memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.fd = cfd;
        ff_epoll_ctl(epollFd_, EPOLL_CTL_ADD, cfd, &ev);
    }
}

void DpdkGateway::onReadable(int cfd) {
    auto it = conns_.find(cfd);
    if (it == conns_.end()) return;

    char buf[kRecvBufSize];
    for (;;) {
        ssize_t n = ff_recv(cfd, buf, sizeof(buf), 0);
        if (n > 0) {
            // Same bytes-in surface kernel recv() fills. feed() decodes whole
            // OUCH frames, submits to the engine, and emits acks via the send
            // callback (→ queueTx → ff_send). false = unrecoverable framing.
            if (!it->second.session->feed(buf, static_cast<size_t>(n))) {
                closeConn(cfd);
                return;
            }
        } else if (n == 0) {
            closeConn(cfd);  // peer closed
            return;
        } else {
            break;  // EAGAIN — RX drained
        }
    }
}

void DpdkGateway::queueTx(int cfd, std::string_view bytes) {
    auto it = conns_.find(cfd);
    if (it == conns_.end()) return;
    Conn& conn = it->second;

    // Preserve ordering: if a backlog exists, append rather than send ahead.
    if (!conn.pendingTx.empty()) {
        conn.pendingTx.insert(conn.pendingTx.end(), bytes.begin(), bytes.end());
        return;
    }

    ssize_t sent = ff_send(cfd, bytes.data(), bytes.size(), 0);
    size_t off = (sent > 0) ? static_cast<size_t>(sent) : 0;
    if (off < bytes.size()) {
        conn.pendingTx.insert(conn.pendingTx.end(), bytes.begin() + off, bytes.end());
        struct epoll_event ev;
        std::memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN | EPOLLOUT;  // wake when writable to drain the backlog
        ev.data.fd = cfd;
        ff_epoll_ctl(epollFd_, EPOLL_CTL_MOD, cfd, &ev);
    }
}

void DpdkGateway::onWritable(int cfd) {
    auto it = conns_.find(cfd);
    if (it == conns_.end()) return;
    Conn& conn = it->second;

    while (!conn.pendingTx.empty()) {
        ssize_t sent = ff_send(cfd, conn.pendingTx.data(), conn.pendingTx.size(), 0);
        if (sent <= 0) return;  // EAGAIN — stay registered for EPOLLOUT
        conn.pendingTx.erase(conn.pendingTx.begin(), conn.pendingTx.begin() + sent);
    }
    struct epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;  // backlog drained — stop watching for writable
    ev.data.fd = cfd;
    ff_epoll_ctl(epollFd_, EPOLL_CTL_MOD, cfd, &ev);
}

void DpdkGateway::closeConn(int cfd) {
    ff_epoll_ctl(epollFd_, EPOLL_CTL_DEL, cfd, nullptr);
    ff_close(cfd);
    conns_.erase(cfd);
}

}  // namespace OrderMatcher

#endif  // OB_HAVE_DPDK
