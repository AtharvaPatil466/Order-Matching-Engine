#pragma once

// FixTcpGateway — slim, portable TCP gateway that speaks FIX 4.2.
//
// Single-threaded event loop on kqueue (macOS) or epoll (Linux). One
// FixSession per connection. The session does all FIX work — framing,
// validation, dispatch into MatchingEngine, ExecutionReport serialization.
// This file is just the OS-specific plumbing that moves bytes between the
// socket and the session.
//
// Reads: drain the socket on each wake-up; pipe everything into
// session.feed(); close the connection if feed() reports an unrecoverable
// framing error.
//
// Writes: the session's send_ callback writes synchronously via blocking
// send() on the same fd. With small ExecutionReport responses (<200 bytes)
// and a level-triggered local-loopback socket, this is fine for tests and
// modest production loads. A high-throughput deployment would queue
// outbound bytes per-fd and arm a write event — left as a future
// extension.
//
// Replaces the previous broken IoUringGateway. The portable framing /
// dispatch primitive (FixSession) is the same one a future io_uring
// rewrite plugs into.

#include "FixSession.h"
#include "MatchingEngine.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/event.h>
#define FIXGW_KQUEUE 1
#elif defined(__linux__)
#include <sys/epoll.h>
#define FIXGW_EPOLL 1
#endif

namespace OrderMatcher {

class FixTcpGateway {
public:
    explicit FixTcpGateway(MatchingEngine& engine, size_t maxFrameSize = 64 * 1024)
        : engine_(engine), maxFrameSize_(maxFrameSize) {}

    ~FixTcpGateway() { stop(); }

    FixTcpGateway(const FixTcpGateway&) = delete;
    FixTcpGateway& operator=(const FixTcpGateway&) = delete;

    // Start listening on `port` (0 lets the OS pick). Returns false on
    // setup failure.
    bool start(uint16_t port, int backlog = 64) {
        if (running_.load()) return false;

        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ < 0) return false;

        int opt = 1;
        ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
            ::listen(listenFd_, backlog) < 0) {
            ::close(listenFd_); listenFd_ = -1;
            return false;
        }

        sockaddr_in bound{};
        socklen_t blen = sizeof(bound);
        ::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&bound), &blen);
        port_ = ntohs(bound.sin_port);
        setNonBlocking(listenFd_);

#ifdef FIXGW_KQUEUE
        evFd_ = ::kqueue();
#elif defined(FIXGW_EPOLL)
        evFd_ = ::epoll_create1(0);
#endif
        if (evFd_ < 0) {
            ::close(listenFd_); listenFd_ = -1;
            return false;
        }
        registerRead(listenFd_);

        // Self-pipe for fast shutdown wake-up.
        if (::pipe(wake_) == 0) {
            setNonBlocking(wake_[0]);
            registerRead(wake_[0]);
        }

        running_.store(true);
        loopThread_ = std::thread(&FixTcpGateway::eventLoop, this);
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        // Wake the loop.
        if (wake_[1] >= 0) { char c = 1; (void)::write(wake_[1], &c, 1); }
        if (loopThread_.joinable()) loopThread_.join();

        // After the loop exits, no other thread touches connections_.
        for (auto& [fd, conn] : connections_) ::close(fd);
        connections_.clear();
        if (listenFd_ >= 0) { ::close(listenFd_); listenFd_ = -1; }
        if (evFd_     >= 0) { ::close(evFd_);     evFd_     = -1; }
        for (int i = 0; i < 2; ++i) {
            if (wake_[i] >= 0) { ::close(wake_[i]); wake_[i] = -1; }
        }
    }

    uint16_t port() const { return port_; }
    size_t connectionCount() const {
        // Loop-only access; safe to read once stopped or from inside the loop.
        return connections_.size();
    }

private:
    struct Connection {
        std::unique_ptr<FixSession> session;
        int                         fd;
    };

    void setNonBlocking(int fd) {
        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    void registerRead(int fd) {
#ifdef FIXGW_KQUEUE
        struct kevent ev;
        EV_SET(&ev, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        ::kevent(evFd_, &ev, 1, nullptr, 0, nullptr);
#elif defined(FIXGW_EPOLL)
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = fd;
        ::epoll_ctl(evFd_, EPOLL_CTL_ADD, fd, &ev);
#endif
    }

    void unregister(int fd) {
#ifdef FIXGW_KQUEUE
        struct kevent ev;
        EV_SET(&ev, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        ::kevent(evFd_, &ev, 1, nullptr, 0, nullptr);
#elif defined(FIXGW_EPOLL)
        ::epoll_ctl(evFd_, EPOLL_CTL_DEL, fd, nullptr);
#endif
    }

    void closeConnection(int fd) {
        unregister(fd);
        ::close(fd);
        connections_.erase(fd);
    }

    void onAccept() {
        for (;;) {
            sockaddr_in caddr{};
            socklen_t   clen = sizeof(caddr);
            int cfd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&caddr), &clen);
            if (cfd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                return;
            }
            int flag = 1;
            ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
#ifdef SO_NOSIGPIPE
            ::setsockopt(cfd, SOL_SOCKET, SO_NOSIGPIPE, &flag, sizeof(flag));
#endif
            setNonBlocking(cfd);

            auto sess = std::make_unique<FixSession>(
                engine_,
                [cfd](std::string_view bytes) {
                    // Synchronous write. Blocking is acceptable for the
                    // small ExecutionReport sizes typical here; a high-
                    // throughput rewrite would queue + EVFILT_WRITE.
                    auto* p = bytes.data();
                    size_t left = bytes.size();
                    while (left) {
                        ssize_t n = ::send(cfd, p, left, 0);
                        if (n <= 0) return;  // give up; reader will reap
                        p += n; left -= size_t(n);
                    }
                },
                maxFrameSize_);

            connections_.emplace(cfd, Connection{std::move(sess), cfd});
            registerRead(cfd);
        }
    }

    void onReadable(int fd) {
        auto it = connections_.find(fd);
        if (it == connections_.end()) return;
        char buf[4096];
        for (;;) {
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n > 0) {
                if (!it->second.session->feed(buf, size_t(n))) {
                    closeConnection(fd);
                    return;
                }
                continue;
            }
            if (n == 0) { closeConnection(fd); return; }
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            closeConnection(fd);
            return;
        }
    }

    void eventLoop() {
#ifdef FIXGW_KQUEUE
        struct kevent events[64];
        while (running_.load()) {
            timespec ts{0, 100 * 1000 * 1000};  // 100ms tick for shutdown polling
            int n = ::kevent(evFd_, nullptr, 0, events, 64, &ts);
            for (int i = 0; i < n; ++i) {
                int fd = int(events[i].ident);
                if (fd == listenFd_) onAccept();
                else if (fd == wake_[0]) {
                    char drain[16];
                    while (::read(wake_[0], drain, sizeof(drain)) > 0) {}
                } else onReadable(fd);
            }
        }
#elif defined(FIXGW_EPOLL)
        epoll_event events[64];
        while (running_.load()) {
            int n = ::epoll_wait(evFd_, events, 64, /*timeout_ms=*/100);
            for (int i = 0; i < n; ++i) {
                int fd = events[i].data.fd;
                if (fd == listenFd_) onAccept();
                else if (fd == wake_[0]) {
                    char drain[16];
                    while (::read(wake_[0], drain, sizeof(drain)) > 0) {}
                } else onReadable(fd);
            }
        }
#endif
    }

    MatchingEngine&                              engine_;
    size_t                                       maxFrameSize_;
    int                                          listenFd_{-1};
    int                                          evFd_{-1};
    int                                          wake_[2]{-1, -1};
    uint16_t                                     port_{0};
    std::atomic<bool>                            running_{false};
    std::thread                                  loopThread_;
    std::unordered_map<int, Connection>          connections_;  // loop-only access
};

}  // namespace OrderMatcher
