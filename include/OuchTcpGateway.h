#pragma once

// OuchTcpGateway — production-style TCP server for the OUCH-over-
// SoupBinTCP stack.
//
// Composition:
//   TCP socket
//     ↓ bytes
//   SoupBinTcpSession  (login/heartbeat/logout state machine)
//     ↓ app payloads
//   OuchSession        (codec + order dispatch into MatchingEngine)
//     ↓ replies
//   SoupBinTcpSession  (wraps replies as Unsequenced packets)
//     ↓ bytes
//   TCP socket
//
// Design: one acceptor thread + one I/O thread per accepted
// connection. Each I/O thread does poll(fd, 100ms) → recv()/feed()
// → tick() in a loop, exits cleanly on session close or stop().
// Per-connection threads simplify shutdown semantics versus a
// shared event loop and keep correctness easy to reason about; the
// existing TcpGateway demonstrates the io-multiplexed design when
// you want the higher fd-density throughput.
//
// Not designed for thousands of concurrent connections — real
// venues have ~hundreds of broker sessions at peak, and one thread
// per connection is fine in that range. For more, drop in the
// kqueue/epoll loop from TcpGateway and replace the per-conn
// thread with a per-fd state machine.

#include "MatchingEngine.h"
#include "OuchSession.h"
#include "SoupBinTcpSession.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace OrderMatcher {

class OuchTcpGateway {
public:
    // Login validation hook. Returns 0 to accept; SOUP_LOGIN_REJECT_*
    // to reject. Default policy: accept all.
    using LoginValidator = std::function<char(const SoupLoginRequest&)>;

    OuchTcpGateway(MatchingEngine& engine, std::string serverSession)
        : engine_(engine), serverSession_(std::move(serverSession)) {}

    ~OuchTcpGateway() { stop(); }

    OuchTcpGateway(const OuchTcpGateway&) = delete;
    OuchTcpGateway& operator=(const OuchTcpGateway&) = delete;

    void setLoginValidator(LoginValidator v) {
        loginValidator_ = std::move(v);
    }

    // Bind and start the acceptor. Pass port=0 to let the OS choose;
    // the actual bound port is then readable via boundPort(). Returns
    // false on bind/listen failure.
    bool start(uint16_t port = 0, int backlog = 16) {
        if (running_.load()) return false;

        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ < 0) return false;

        int opt = 1;
        ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(listenFd_); listenFd_ = -1; return false;
        }
        if (::listen(listenFd_, backlog) < 0) {
            ::close(listenFd_); listenFd_ = -1; return false;
        }
        sockaddr_in bound{};
        socklen_t boundLen = sizeof(bound);
        ::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&bound), &boundLen);
        boundPort_ = ntohs(bound.sin_port);

        running_.store(true);
        acceptorThread_ = std::thread(&OuchTcpGateway::acceptorLoop, this);
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;

        // Close the listener — wakes up the acceptor blocked in
        // accept(); it'll exit on the next iteration.
        if (listenFd_ >= 0) {
            ::shutdown(listenFd_, SHUT_RDWR);
            ::close(listenFd_);
            listenFd_ = -1;
        }
        if (acceptorThread_.joinable()) acceptorThread_.join();

        // Tell each connection worker to exit, then join them.
        {
            std::lock_guard<std::mutex> lock(connsMutex_);
            for (int fd : openFds_) {
                ::shutdown(fd, SHUT_RDWR);  // unblocks the worker's poll
            }
        }
        for (auto& t : workerThreads_) {
            if (t.joinable()) t.join();
        }
        workerThreads_.clear();
        openFds_.clear();
    }

    uint16_t boundPort() const { return boundPort_; }
    bool isRunning()      const { return running_.load(); }
    uint64_t connectionsAccepted() const {
        return connectionsAccepted_.load();
    }
    uint64_t ordersAcceptedTotal() const {
        return ordersAcceptedTotal_.load();
    }
    uint64_t ordersRejectedTotal() const {
        return ordersRejectedTotal_.load();
    }

private:
    void acceptorLoop() {
        int lfd = listenFd_;
        while (running_.load()) {
            sockaddr_in peer{};
            socklen_t peerLen = sizeof(peer);
            int fd = ::accept(lfd, reinterpret_cast<sockaddr*>(&peer),
                              &peerLen);
            if (fd < 0) {
                if (!running_.load()) break;  // shutdown
                continue;
            }
            int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            registerConnection(fd);
            ++connectionsAccepted_;
            workerThreads_.emplace_back(&OuchTcpGateway::connectionLoop,
                                        this, fd);
        }
    }

    void registerConnection(int fd) {
        std::lock_guard<std::mutex> lock(connsMutex_);
        openFds_.insert(fd);
    }

    void unregisterConnection(int fd) {
        std::lock_guard<std::mutex> lock(connsMutex_);
        openFds_.erase(fd);
    }

    static uint64_t nowMs() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    void connectionLoop(int fd) {
        // Each connection owns its own session pair. The OuchSession
        // routes responses back through the SoupBinTcpSession's
        // Unsequenced channel so the wire packets stay spec-compliant
        // top to bottom.
        auto soup = std::make_unique<SoupBinTcpSession>(
            [fd](std::string_view bytes) {
                ::send(fd, bytes.data(), bytes.size(), 0);
            },
            serverSession_);
        if (loginValidator_) {
            soup->setOnLoginRequest(loginValidator_);
        } else {
            // Default: accept every login.
            soup->setOnLoginRequest([](const SoupLoginRequest&) {
                return char{0};
            });
        }

        // OuchSession holds a raw reference to soup; we need it alive
        // for the duration. Construct ouch on the stack using
        // captures that own the soup pointer via shared_ptr.
        std::shared_ptr<SoupBinTcpSession> soupShared(std::move(soup));
        OuchSession ouch(engine_,
            [soupShared](std::string_view ouchBytes) {
                soupShared->sendUnsequenced(ouchBytes.data(), ouchBytes.size());
            });

        soupShared->setOnAppPayload(
            [&ouch](const uint8_t* p, size_t n, bool /*sequenced*/) {
                ouch.feed(reinterpret_cast<const char*>(p), n);
            });

        constexpr int kPollTimeoutMs = 100;
        constexpr size_t kRecvBufBytes = 4096;
        std::vector<char> buf(kRecvBufBytes);

        while (running_.load() && !soupShared->closed()) {
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLIN;
            int n = ::poll(&pfd, 1, kPollTimeoutMs);
            uint64_t now = nowMs();

            if (n < 0) break;  // poll error — bail
            if (n > 0) {
                if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) break;
                if (pfd.revents & POLLIN) {
                    ssize_t r = ::recv(fd, buf.data(), buf.size(), 0);
                    if (r <= 0) break;  // peer closed or recv error
                    if (!soupShared->feed(buf.data(),
                                          static_cast<size_t>(r), now)) {
                        // soup signaled close — session is terminal or
                        // protocol violation. Drain any remaining
                        // output and exit the loop.
                        break;
                    }
                }
            }

            // Periodic tick for heartbeats / peer-idle disconnect.
            if (!soupShared->tick(now)) break;
        }

        // Surface aggregate counters to the gateway BEFORE tearing
        // down so the test side can read them.
        ordersAcceptedTotal_.fetch_add(ouch.ordersAccepted());
        ordersRejectedTotal_.fetch_add(ouch.ordersRejected());

        ::close(fd);
        unregisterConnection(fd);
    }

    MatchingEngine&            engine_;
    std::string                serverSession_;
    LoginValidator             loginValidator_;

    std::atomic<bool>          running_{false};
    int                        listenFd_{-1};
    uint16_t                   boundPort_{0};

    std::thread                acceptorThread_;
    std::vector<std::thread>   workerThreads_;
    std::mutex                 connsMutex_;
    std::unordered_set<int>    openFds_;

    std::atomic<uint64_t>      connectionsAccepted_{0};
    std::atomic<uint64_t>      ordersAcceptedTotal_{0};
    std::atomic<uint64_t>      ordersRejectedTotal_{0};
};

}  // namespace OrderMatcher
