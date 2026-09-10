#pragma once

// ItchRetransmissionService — SoupBinTCP-over-TCP gap-recovery
// service that replays journaled MoldUDP64 messages to subscribers
// that detect a sequence gap on the multicast feed.
//
// Wire protocol (re-request packet, sent by subscriber over
// SoupBinTCP as UnsequencedData):
//   Byte 0   :  Request type tag (1 byte, must be 'R')
//   Bytes 1-8:  Starting sequence number (uint64, big-endian)
//   Bytes 9-10: Count of messages requested (uint16, big-endian)
//                  count == 0 means "from start to end of journal"
// Total payload: 11 bytes.
//
// Real Nasdaq uses a different exact format (see "Sequenced Data
// Re-request" packet in the Nasdaq SoupBinTCP / NASDAQ-OUCH spec);
// this is the same shape with our tag instead of a multi-byte
// header. Documented inline so consumers know what to expect.
//
// Response: the service streams each found message back as a
// SoupBinTCP SequencedData packet, then leaves the connection open
// for follow-up re-requests until the subscriber disconnects.
//
// Composition: an OuchTcpGateway-style server (acceptor + per-
// connection thread). Each connection runs a SoupBinTcpSession;
// the session's app-payload callback parses the re-request and
// streams the response.

#include "MoldPacketJournal.h"
#include "OuchProtocol.h"  // readU16BE / readU64BE
#include "RateLimiter.h"
#include "StructuredLog.h"
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

constexpr char ITCH_RETRANSMIT_REQUEST_TAG = 'R';
constexpr size_t ITCH_RETRANSMIT_REQUEST_BYTES = 11;

// Per-request replay cap. The re-request count field is a uint16 (max
// 65535) and count==0 means "from startSeq to the end of the journal";
// either lets a single subscriber ask the server to stream an unbounded
// run of messages, pinning the per-connection thread and starving other
// requests. The service clamps the work served per re-request to this
// many messages — a subscriber needing more simply issues a follow-up
// request from the next uncovered sequence. Sized to comfortably cover
// realistic multicast gaps while keeping any one request bounded.
constexpr uint16_t ITCH_RETRANSMIT_MAX_REPLAY = 1024;

// Concurrency and budget bounds.
//
// A re-request is ~11 bytes on the wire and can return up to
// ITCH_RETRANSMIT_MAX_REPLAY sequenced messages. That per-request clamp bounds
// ONE request; nothing bounded how many a subscriber could issue, or how many
// subscribers could connect. The service spawns an OS thread per accepted
// connection, so an unbounded accept loop is also an unbounded thread count —
// and every thread object stayed in workerThreads_ until stop(), so even a
// polite client reconnecting in a loop grew the vector without limit.
//
// These are deliberately generous: a genuine subscriber recovering a gap
// issues a short burst of re-requests and stops. Anything sustained past this
// is not gap recovery.
constexpr size_t   ITCH_RETRANSMIT_MAX_CONNECTIONS  = 64;
constexpr uint64_t ITCH_RETRANSMIT_REQS_PER_SEC     = 50;
constexpr uint64_t ITCH_RETRANSMIT_REQ_BURST        = 200;

// Synchronous helper: validate and parse a re-request payload from
// the wire. Returns true on a well-formed request.
struct ItchRetransmitRequest {
    uint64_t startSeq;
    uint16_t count;
};

inline bool parseRetransmitRequest(const uint8_t* p, size_t len,
                                   ItchRetransmitRequest& out) {
    if (len != ITCH_RETRANSMIT_REQUEST_BYTES) return false;
    if (p[0] != static_cast<uint8_t>(ITCH_RETRANSMIT_REQUEST_TAG)) return false;
    out.startSeq = readU64BE(p + 1);
    out.count    = readU16BE(p + 9);
    return true;
}

class ItchRetransmissionService {
public:
    using LoginValidator = SoupBinTcpSession::OnLoginRequest;

    ItchRetransmissionService(MoldPacketJournal& journal,
                              std::string serverSession)
        : journal_(journal), serverSession_(std::move(serverSession)) {}

    ~ItchRetransmissionService() { stop(); }

    ItchRetransmissionService(const ItchRetransmissionService&) = delete;
    ItchRetransmissionService& operator=(const ItchRetransmissionService&) = delete;

    void setLoginValidator(LoginValidator v) { loginValidator_ = std::move(v); }

    bool start(uint16_t port = 0, int backlog = 16) {
        if (running_.load()) return false;

        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        int opt = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd); return false;
        }
        if (::listen(fd, backlog) < 0) {
            ::close(fd); return false;
        }
        sockaddr_in bound{};
        socklen_t boundLen = sizeof(bound);
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &boundLen);
        boundPort_ = ntohs(bound.sin_port);

        listenFd_.store(fd);
        running_.store(true);
        acceptorThread_ = std::thread(
            &ItchRetransmissionService::acceptorLoop, this);
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        // shutdown() wakes a blocked accept(); close() is what wakes it on
        // macOS (shutdown alone doesn't on listening sockets), so keep the
        // close here rather than deferring past the join.
        int lfd = listenFd_.exchange(-1);
        if (lfd >= 0) {
            ::shutdown(lfd, SHUT_RDWR);
            ::close(lfd);
        }
        if (acceptorThread_.joinable()) acceptorThread_.join();

        {
            std::lock_guard<std::mutex> lock(connsMutex_);
            for (int fd : openFds_) ::shutdown(fd, SHUT_RDWR);
        }
        {
            std::lock_guard<std::mutex> lock(threadsMutex_);
            for (auto& [t, done] : workerThreads_) {
                if (t.joinable()) t.join();
            }
            workerThreads_.clear();
        }
        openFds_.clear();
    }

    uint16_t boundPort()              const { return boundPort_; }
    bool     isRunning()              const { return running_.load(); }
    uint64_t requestsServed()         const { return requestsServed_.load(); }
    uint64_t messagesReplayedTotal()  const { return messagesReplayedTotal_.load(); }
    uint64_t requestsThrottled()      const { return requestsThrottled_.load(); }
    uint64_t connectionsRejected()    const { return connectionsRejected_.load(); }
    size_t   activeConnections()      const { return activeConnections_.load(); }

private:
    void acceptorLoop() {
        int lfd = listenFd_.load();
        while (running_.load()) {
            sockaddr_in peer{};
            socklen_t peerLen = sizeof(peer);
            int fd = ::accept(lfd, reinterpret_cast<sockaddr*>(&peer),
                              &peerLen);
            if (fd < 0) {
                if (!running_.load()) break;
                continue;
            }
            int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

            // Retire threads whose connection has ended. Without this the
            // vector grew by one std::thread for every connection ever
            // accepted — a client reconnecting in a loop exhausted memory even
            // while holding only one connection open at a time.
            reapFinishedConnections();

            // Cap concurrency. Each connection owns an OS thread, so an
            // unbounded accept loop is an unbounded thread count: a few
            // thousand sockets is all it takes to exhaust the process. Refuse
            // past the cap by closing immediately — a subscriber that cannot
            // be served should find out now, not by being queued behind a
            // flood.
            if (activeConnections_.load(std::memory_order_relaxed) >=
                ITCH_RETRANSMIT_MAX_CONNECTIONS) {
                ++connectionsRejected_;
                obSink().log(obEvent("itch_retransmit_connection_refused",
                                     LogSeverity::Warn)
                                 .kv("active", (long long)activeConnections_.load()));
                ::close(fd);
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(connsMutex_);
                openFds_.insert(fd);
            }
            activeConnections_.fetch_add(1, std::memory_order_relaxed);
            auto done = std::make_shared<std::atomic<bool>>(false);
            {
                std::lock_guard<std::mutex> lock(threadsMutex_);
                workerThreads_.emplace_back(
                    std::thread([this, fd, done] {
                        connectionLoop(fd);
                        activeConnections_.fetch_sub(1, std::memory_order_relaxed);
                        done->store(true, std::memory_order_release);
                    }),
                    done);
            }
        }
    }

    // Join and drop every connection thread that has finished. Called from the
    // acceptor only, so it never blocks a live connection.
    void reapFinishedConnections() {
        std::lock_guard<std::mutex> lock(threadsMutex_);
        for (auto it = workerThreads_.begin(); it != workerThreads_.end();) {
            if (it->second->load(std::memory_order_acquire)) {
                if (it->first.joinable()) it->first.join();
                it = workerThreads_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void connectionLoop(int fd) {
        auto soup = std::make_shared<SoupBinTcpSession>(
            [fd](std::string_view bytes) {
                ::send(fd, bytes.data(), bytes.size(), 0);
            },
            serverSession_);
        // Always enforce that a re-request names THIS service's session
        // (or leaves it blank = "current"). A login that asks for a
        // different session is a cross-session replay attempt and is
        // refused before any journal access. The caller's custom
        // validator (if any) runs only after the session check passes.
        const std::string expectedSession = serverSession_;
        LoginValidator userValidator = loginValidator_;
        soup->setOnLoginRequest(
            [expectedSession, userValidator](const SoupLoginRequest& req) -> char {
                if (!req.session.empty() && req.session != expectedSession) {
                    return SOUP_LOGIN_REJECT_SESSION_NOT_AVAILABLE;
                }
                if (userValidator) return userValidator(req);
                return char{0};
            });

        auto* journalPtr = &journal_;
        auto requestsCtr = &requestsServed_;
        auto messagesCtr = &messagesReplayedTotal_;
        auto throttledCtr = &requestsThrottled_;
        auto budget = std::make_shared<TokenBucket>(ITCH_RETRANSMIT_REQS_PER_SEC,
                                                    ITCH_RETRANSMIT_REQ_BURST);
        std::weak_ptr<SoupBinTcpSession> soupWeak = soup;

        soup->setOnAppPayload(
            [journalPtr, requestsCtr, messagesCtr, throttledCtr, budget, soupWeak]
            (const uint8_t* p, size_t n, bool /*sequenced*/) {
                ItchRetransmitRequest req;
                if (!parseRetransmitRequest(p, n, req)) return;
                ++*requestsCtr;

                // Per-connection budget. Without it the per-request clamp
                // bounds a single reply but not the stream: a client could
                // issue re-requests back to back and make the venue spend all
                // its egress replaying the journal at one subscriber. Over
                // budget, drop the request silently rather than replying —
                // answering "you are going too fast" to a flood is itself a
                // reply, and doubles the work the flood was trying to cause.
                if (!budget->tryConsume(nowNs())) {
                    ++*throttledCtr;
                    return;
                }

                auto s = soupWeak.lock();
                if (!s) return;
                // Bound the per-request work: clamp count==0 ("to end of
                // journal") and any count above the cap down to
                // ITCH_RETRANSMIT_MAX_REPLAY, so one slow subscriber can't
                // monopolize the connection thread streaming an unbounded
                // run. A client needing more issues a follow-up
                // re-request from the next uncovered sequence.
                uint16_t replayCount = req.count;
                if (replayCount == 0 || replayCount > ITCH_RETRANSMIT_MAX_REPLAY) {
                    replayCount = ITCH_RETRANSMIT_MAX_REPLAY;
                }
                journalPtr->replayRange(req.startSeq, replayCount,
                    [&s, messagesCtr](uint64_t /*seq*/, const uint8_t* data, size_t len) {
                        s->sendSequenced(data, len);
                        ++*messagesCtr;
                    });
                // No terminator: the connection stays open so the
                // subscriber can issue further re-requests, and closes
                // when the peer disconnects (poll loop below exits).
            });

        constexpr int kPollTimeoutMs = 100;
        std::vector<char> buf(4096);
        while (running_.load() && !soup->closed()) {
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLIN;
            int n = ::poll(&pfd, 1, kPollTimeoutMs);
            uint64_t now = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            if (n < 0) break;
            if (n > 0) {
                if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) break;
                if (pfd.revents & POLLIN) {
                    ssize_t r = ::recv(fd, buf.data(), buf.size(), 0);
                    if (r <= 0) break;
                    if (!soup->feed(buf.data(),
                                    static_cast<size_t>(r), now)) break;
                }
            }
            if (!soup->tick(now)) break;
        }

        ::close(fd);
        {
            std::lock_guard<std::mutex> lock(connsMutex_);
            openFds_.erase(fd);
        }
    }

    MoldPacketJournal&       journal_;
    std::string              serverSession_;
    LoginValidator           loginValidator_;

    std::atomic<bool>        running_{false};
    // ponytail: atomic — stop() writes -1 while acceptorLoop() reads it on
    // another thread (TSan data race on the plain int otherwise).
    std::atomic<int>         listenFd_{-1};
    uint16_t                 boundPort_{0};

    std::thread              acceptorThread_;
    // Thread plus a flag it sets on exit, so the acceptor can reap finished
    // connections without blocking on live ones.
    std::vector<std::pair<std::thread, std::shared_ptr<std::atomic<bool>>>>
                             workerThreads_;
    std::mutex               threadsMutex_;
    std::atomic<uint64_t>    requestsThrottled_{0};
    std::atomic<size_t>      activeConnections_{0};
    std::atomic<uint64_t>    connectionsRejected_{0};
    std::mutex               connsMutex_;
    std::unordered_set<int>  openFds_;

    std::atomic<uint64_t>    requestsServed_{0};
    std::atomic<uint64_t>    messagesReplayedTotal_{0};
};

// Helper: build a re-request packet payload (the 11 bytes that go
// inside a SoupBinTCP UnsequencedData wrapper). Caller wraps with
// soupWriteEnvelope().
inline size_t buildRetransmitRequest(uint8_t* out, uint64_t startSeq,
                                     uint16_t count) {
    out[0] = static_cast<uint8_t>(ITCH_RETRANSMIT_REQUEST_TAG);
    writeU64BE(out + 1, startSeq);
    writeU16BE(out + 9, count);
    return ITCH_RETRANSMIT_REQUEST_BYTES;
}

}  // namespace OrderMatcher
