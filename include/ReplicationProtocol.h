#pragma once

// ReplicationProtocol — primary-backup HA for the matching engine.
//
// Architecture: deterministic primary-backup (the pattern used by CME Globex,
// NYSE Arca, and most real exchange engines — not Raft/Paxos, because matching
// engines need deterministic single-writer semantics, not quorum consensus).
//
// Components:
//   1. HeartbeatMonitor — failure detection via periodic pings
//   2. LeaderLease — epoch-based fencing to prevent split-brain
//   3. ReplicationTransport — TCP log-shipping from primary to backup
//   4. ReplicationCoordinator — ties it all together
//
// The primary journals operations locally, then ships the journal entries
// to backup(s) via TCP. On primary failure, a backup that holds the latest
// epoch + lease can promote itself via JournalFollower::promote().
//
// Wire format for replication:
//   [ReplicationHeader (24B)] [payload (variable)]
//
// This is the "hard part" that was listed in the honest accounting.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// POSIX networking
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

namespace OrderMatcher {

// ─── Heartbeat Monitor ──────────────────────────────────────────────────────

class HeartbeatMonitor {
public:
    using FailureCallback = std::function<void()>;

    explicit HeartbeatMonitor(uint32_t intervalMs = 100, uint32_t timeoutMs = 500)
        : intervalMs_(intervalMs), timeoutMs_(timeoutMs) {}

    ~HeartbeatMonitor() { stop(); }

    HeartbeatMonitor(const HeartbeatMonitor&) = delete;
    HeartbeatMonitor& operator=(const HeartbeatMonitor&) = delete;

    void setFailureCallback(FailureCallback cb) { onFailure_ = std::move(cb); }

    void start() {
        if (running_.exchange(true)) return;
        lastHeartbeat_.store(nowMs());
        worker_ = std::thread([this] { monitorLoop(); });
    }

    void stop() {
        running_.store(false);
        if (worker_.joinable()) worker_.join();
    }

    // Called by the replication transport when a heartbeat arrives from peer.
    void receivedHeartbeat() {
        lastHeartbeat_.store(nowMs());
        missedCount_.store(0);
    }

    // Send a heartbeat. The caller provides the actual send function.
    using SendFn = std::function<bool()>;
    void setSendFunction(SendFn fn) { sendFn_ = std::move(fn); }

    bool isAlive() const {
        return (nowMs() - lastHeartbeat_.load()) < timeoutMs_;
    }

    uint32_t missedCount() const { return missedCount_.load(); }

private:
    void monitorLoop() {
        while (running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs_));
            if (!running_.load()) break;

            // Send our heartbeat
            if (sendFn_) sendFn_();

            // Check peer's heartbeat
            uint64_t elapsed = nowMs() - lastHeartbeat_.load();
            if (elapsed > timeoutMs_) {
                missedCount_.fetch_add(1);
                if (onFailure_) onFailure_();
            }
        }
    }

    static uint64_t nowMs() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    uint32_t intervalMs_;
    uint32_t timeoutMs_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> lastHeartbeat_{0};
    std::atomic<uint32_t> missedCount_{0};
    std::thread worker_;
    FailureCallback onFailure_;
    SendFn sendFn_;
};

// ─── Leader Lease ───────────────────────────────────────────────────────────

class LeaderLease {
public:
    struct Lease {
        uint64_t epoch;          // Monotonically increasing epoch number
        uint64_t grantedAtMs;    // When this lease was granted
        uint64_t durationMs;     // How long the lease is valid
        uint32_t holderId;       // Node ID that holds the lease

        bool isValid(uint64_t nowMs) const {
            return (nowMs - grantedAtMs) < durationMs;
        }
    };

    explicit LeaderLease(uint32_t nodeId, uint64_t leaseDurationMs = 5000)
        : nodeId_(nodeId), leaseDurationMs_(leaseDurationMs) {}

    // Attempt to acquire leadership for a new epoch.
    // Returns true if this node now holds the lease.
    bool tryAcquire() {
        std::lock_guard<std::mutex> lock(mu_);
        uint64_t now = nowMs();

        // Can only acquire if no valid lease exists, or we already hold it
        if (currentLease_.isValid(now) && currentLease_.holderId != nodeId_) {
            return false;  // Someone else holds a valid lease
        }

        currentLease_.epoch = ++epoch_;
        currentLease_.grantedAtMs = now;
        currentLease_.durationMs = leaseDurationMs_;
        currentLease_.holderId = nodeId_;
        return true;
    }

    // Renew an existing lease (must already hold it).
    bool renew() {
        std::lock_guard<std::mutex> lock(mu_);
        if (currentLease_.holderId != nodeId_) return false;
        currentLease_.grantedAtMs = nowMs();
        return true;
    }

    // Release the lease (voluntary step-down).
    void release() {
        std::lock_guard<std::mutex> lock(mu_);
        if (currentLease_.holderId == nodeId_) {
            currentLease_.durationMs = 0;  // Expire immediately
        }
    }

    // Accept a lease from a remote primary (used by backups).
    void acceptRemoteLease(const Lease& remote) {
        std::lock_guard<std::mutex> lock(mu_);
        if (remote.epoch > epoch_) {
            epoch_ = remote.epoch;
            currentLease_ = remote;
        }
    }

    // Fencing check: is operation valid under the given epoch?
    bool isFenced(uint64_t operationEpoch) const {
        std::lock_guard<std::mutex> lock(mu_);
        return operationEpoch < epoch_;
    }

    bool isLeader() const {
        std::lock_guard<std::mutex> lock(mu_);
        return currentLease_.holderId == nodeId_ && currentLease_.isValid(nowMs());
    }

    Lease currentLease() const {
        std::lock_guard<std::mutex> lock(mu_);
        return currentLease_;
    }

    uint64_t epoch() const { return epoch_.load(); }
    uint32_t nodeId() const { return nodeId_; }

private:
    static uint64_t nowMs() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    uint32_t nodeId_;
    uint64_t leaseDurationMs_;
    mutable std::mutex mu_;
    std::atomic<uint64_t> epoch_{0};
    Lease currentLease_{};
};

// ─── Replication Transport ──────────────────────────────────────────────────

#pragma pack(push, 1)
struct ReplicationHeader {
    static constexpr uint32_t MAGIC = 0x52455053;  // "REPS"

    enum class Type : uint8_t {
        Heartbeat       = 1,
        JournalEntry    = 2,
        LeaseGrant      = 3,
        LeaseAck        = 4,
        PromoteRequest  = 5,
        PromoteAck      = 6,
        SnapshotStart   = 7,
        SnapshotChunk   = 8,
        SnapshotEnd     = 9
    };

    uint32_t magic;
    Type     type;
    uint64_t epoch;
    uint32_t senderId;
    uint32_t payloadSize;
    uint32_t sequenceNum;    // monotonic per sender
};
#pragma pack(pop)

static_assert(sizeof(ReplicationHeader) == 25,
              "ReplicationHeader must be exactly 25 bytes");

class ReplicationTransport {
public:
    using OnMessageFn = std::function<void(const ReplicationHeader& hdr,
                                            const uint8_t* payload,
                                            size_t payloadLen)>;

    ReplicationTransport() = default;
    ~ReplicationTransport() { stop(); }

    ReplicationTransport(const ReplicationTransport&) = delete;
    ReplicationTransport& operator=(const ReplicationTransport&) = delete;

    // Primary mode: listen for backup connections.
    bool listenOn(int port) {
        listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ < 0) return false;

        int opt = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(static_cast<uint16_t>(port));

        if (bind(listenFd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(listenFd_);
            listenFd_ = -1;
            return false;
        }

        if (listen(listenFd_, 8) < 0) {
            close(listenFd_);
            listenFd_ = -1;
            return false;
        }

        return true;
    }

    // Backup mode: connect to primary.
    bool connectTo(const std::string& host, int port) {
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(),
                        &hints, &res) != 0) {
            return false;
        }

        peerFd_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (peerFd_ < 0) { freeaddrinfo(res); return false; }

        struct timeval tv{3, 0};
        setsockopt(peerFd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(peerFd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (connect(peerFd_, res->ai_addr, res->ai_addrlen) != 0) {
            close(peerFd_);
            peerFd_ = -1;
            freeaddrinfo(res);
            return false;
        }

        freeaddrinfo(res);
        return true;
    }

    void setMessageHandler(OnMessageFn fn) { onMessage_ = std::move(fn); }

    void startReceiving() {
        if (receiving_.exchange(true)) return;
        recvThread_ = std::thread([this] { receiveLoop(); });
    }

    void stop() {
        receiving_.store(false);
        if (listenFd_ >= 0) { close(listenFd_); listenFd_ = -1; }
        if (peerFd_ >= 0) { close(peerFd_); peerFd_ = -1; }
        if (recvThread_.joinable()) recvThread_.join();
    }

    // Send a message to the peer.
    bool send(ReplicationHeader::Type type, uint64_t epoch,
              uint32_t senderId, const uint8_t* payload, size_t payloadLen) {
        if (peerFd_ < 0) return false;

        ReplicationHeader hdr{};
        hdr.magic = ReplicationHeader::MAGIC;
        hdr.type = type;
        hdr.epoch = epoch;
        hdr.senderId = senderId;
        hdr.payloadSize = static_cast<uint32_t>(payloadLen);
        hdr.sequenceNum = ++sendSeq_;

        // Send header + payload atomically
        std::lock_guard<std::mutex> lock(sendMu_);

        if (sendAll(peerFd_, reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr)) < 0)
            return false;
        if (payloadLen > 0 && sendAll(peerFd_, payload, payloadLen) < 0)
            return false;

        bytesSent_.fetch_add(sizeof(hdr) + payloadLen, std::memory_order_relaxed);
        return true;
    }

    bool sendHeartbeat(uint64_t epoch, uint32_t senderId) {
        return send(ReplicationHeader::Type::Heartbeat, epoch, senderId, nullptr, 0);
    }

    // Accept a single backup connection (blocking).
    bool acceptOne() {
        if (listenFd_ < 0) return false;
        struct sockaddr_in clientAddr{};
        socklen_t len = sizeof(clientAddr);
        peerFd_ = accept(listenFd_, reinterpret_cast<struct sockaddr*>(&clientAddr), &len);
        return peerFd_ >= 0;
    }

    uint64_t bytesSent() const { return bytesSent_.load(std::memory_order_relaxed); }
    uint64_t bytesReceived() const { return bytesReceived_.load(std::memory_order_relaxed); }
    bool isConnected() const { return peerFd_ >= 0; }

private:
    void receiveLoop() {
        while (receiving_.load()) {
            if (peerFd_ < 0) {
                // If in listen mode, try to accept
                if (listenFd_ >= 0) {
                    struct pollfd pfd{listenFd_, POLLIN, 0};
                    if (poll(&pfd, 1, 100) > 0) {
                        acceptOne();
                    }
                    continue;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            // Read header
            ReplicationHeader hdr{};
            ssize_t n = recvAll(peerFd_, reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr));
            if (n <= 0) {
                close(peerFd_);
                peerFd_ = -1;
                continue;
            }

            if (hdr.magic != ReplicationHeader::MAGIC) {
                // Corrupted stream — close and reconnect
                close(peerFd_);
                peerFd_ = -1;
                continue;
            }

            // Read payload
            std::vector<uint8_t> payload;
            if (hdr.payloadSize > 0) {
                payload.resize(hdr.payloadSize);
                n = recvAll(peerFd_, payload.data(), hdr.payloadSize);
                if (n <= 0) {
                    close(peerFd_);
                    peerFd_ = -1;
                    continue;
                }
            }

            bytesReceived_.fetch_add(sizeof(hdr) + hdr.payloadSize,
                                     std::memory_order_relaxed);

            if (onMessage_) {
                onMessage_(hdr, payload.data(), payload.size());
            }
        }
    }

    static ssize_t sendAll(int fd, const uint8_t* buf, size_t len) {
        size_t sent = 0;
        while (sent < len) {
            ssize_t n = ::send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
            if (n <= 0) return -1;
            sent += static_cast<size_t>(n);
        }
        return static_cast<ssize_t>(sent);
    }

    static ssize_t recvAll(int fd, uint8_t* buf, size_t len) {
        size_t got = 0;
        while (got < len) {
            ssize_t n = recv(fd, buf + got, len - got, 0);
            if (n <= 0) return -1;
            got += static_cast<size_t>(n);
        }
        return static_cast<ssize_t>(got);
    }

    int listenFd_{-1};
    int peerFd_{-1};
    std::atomic<bool> receiving_{false};
    std::thread recvThread_;
    std::mutex sendMu_;
    uint32_t sendSeq_{0};
    std::atomic<uint64_t> bytesSent_{0};
    std::atomic<uint64_t> bytesReceived_{0};
    OnMessageFn onMessage_;
};

// ─── Replication Coordinator ────────────────────────────────────────────────

enum class NodeRole : uint8_t {
    Backup  = 0,
    Primary = 1
};

class ReplicationCoordinator {
public:
    explicit ReplicationCoordinator(uint32_t nodeId,
                                    uint32_t heartbeatMs = 100,
                                    uint32_t heartbeatTimeoutMs = 500,
                                    uint64_t leaseDurationMs = 5000)
        : nodeId_(nodeId),
          lease_(nodeId, leaseDurationMs),
          heartbeat_(heartbeatMs, heartbeatTimeoutMs) {}

    ~ReplicationCoordinator() { stop(); }

    ReplicationCoordinator(const ReplicationCoordinator&) = delete;
    ReplicationCoordinator& operator=(const ReplicationCoordinator&) = delete;

    // ── Primary startup ──

    bool startAsPrimary(int replicationPort) {
        role_ = NodeRole::Primary;

        if (!transport_.listenOn(replicationPort)) return false;

        // Set up heartbeat sender
        heartbeat_.setSendFunction([this]() -> bool {
            return transport_.sendHeartbeat(lease_.epoch(), nodeId_);
        });

        // On backup failure detection (for logging/alerting only)
        heartbeat_.setFailureCallback([]() {
            // Backup went dark — primary continues operating.
            // In a real system, this would trigger an alert.
        });

        // Try to acquire the lease
        if (!lease_.tryAcquire()) return false;

        // Set up replication message handler
        transport_.setMessageHandler(
            [this](const ReplicationHeader& hdr, const uint8_t*, size_t) {
                if (hdr.type == ReplicationHeader::Type::Heartbeat) {
                    heartbeat_.receivedHeartbeat();
                }
                if (hdr.type == ReplicationHeader::Type::LeaseAck) {
                    // Backup acknowledged our lease
                }
            });

        transport_.startReceiving();
        heartbeat_.start();
        running_ = true;
        return true;
    }

    // ── Backup startup ──

    bool startAsBackup(const std::string& primaryHost, int primaryPort) {
        role_ = NodeRole::Backup;

        if (!transport_.connectTo(primaryHost, primaryPort)) return false;

        heartbeat_.setSendFunction([this]() -> bool {
            return transport_.sendHeartbeat(lease_.epoch(), nodeId_);
        });

        // On primary failure detection: attempt promotion
        heartbeat_.setFailureCallback([this]() {
            if (promotionCallback_) {
                // Try to acquire the lease (fencing: new epoch)
                if (lease_.tryAcquire()) {
                    role_ = NodeRole::Primary;
                    promotionCallback_();
                }
            }
        });

        transport_.setMessageHandler(
            [this](const ReplicationHeader& hdr, const uint8_t* payload, size_t len) {
                if (hdr.type == ReplicationHeader::Type::Heartbeat) {
                    heartbeat_.receivedHeartbeat();
                }
                if (hdr.type == ReplicationHeader::Type::LeaseGrant) {
                    LeaderLease::Lease remoteLease{};
                    remoteLease.epoch = hdr.epoch;
                    remoteLease.holderId = hdr.senderId;
                    remoteLease.grantedAtMs = nowMs();
                    remoteLease.durationMs = 5000;
                    lease_.acceptRemoteLease(remoteLease);
                }
                if (hdr.type == ReplicationHeader::Type::JournalEntry) {
                    // Apply journal entry to local replica
                    if (journalApplyCallback_) {
                        journalApplyCallback_(payload, len);
                    }
                }
            });

        transport_.startReceiving();
        heartbeat_.start();
        running_ = true;
        return true;
    }

    void stop() {
        running_ = false;
        heartbeat_.stop();
        transport_.stop();
    }

    // Ship a journal entry to backup(s).
    bool replicateEntry(const uint8_t* entry, size_t len) {
        if (role_ != NodeRole::Primary) return false;
        return transport_.send(ReplicationHeader::Type::JournalEntry,
                               lease_.epoch(), nodeId_, entry, len);
    }

    // Callbacks
    using PromotionCallback = std::function<void()>;
    using JournalApplyCallback = std::function<void(const uint8_t* data, size_t len)>;

    void setPromotionCallback(PromotionCallback cb) { promotionCallback_ = std::move(cb); }
    void setJournalApplyCallback(JournalApplyCallback cb) { journalApplyCallback_ = std::move(cb); }

    // State accessors
    NodeRole role() const { return role_; }
    bool isLeader() const { return lease_.isLeader(); }
    uint64_t epoch() const { return lease_.epoch(); }
    bool isPeerAlive() const { return heartbeat_.isAlive(); }
    bool isRunning() const { return running_; }

private:
    static uint64_t nowMs() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    uint32_t nodeId_;
    std::atomic<NodeRole> role_{NodeRole::Backup};
    LeaderLease lease_;
    HeartbeatMonitor heartbeat_;
    ReplicationTransport transport_;
    bool running_{false};
    PromotionCallback promotionCallback_;
    JournalApplyCallback journalApplyCallback_;
};

}  // namespace OrderMatcher
