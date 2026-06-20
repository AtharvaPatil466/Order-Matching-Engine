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

#include "Metrics.h"
#include "EpochStore.h"

#include <atomic>
#include <memory>
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
        if (running_.exchange(true, std::memory_order_acq_rel)) return;
        lastHeartbeat_.store(nowMs(), std::memory_order_release);
        worker_ = std::thread([this] { monitorLoop(); });
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        if (worker_.joinable()) worker_.join();
    }

    // Called by the replication transport when a heartbeat arrives from peer.
    void receivedHeartbeat() {
        lastHeartbeat_.store(nowMs(), std::memory_order_release);
        missedCount_.store(0);
    }

    // Send a heartbeat. The caller provides the actual send function.
    using SendFn = std::function<bool()>;
    void setSendFunction(SendFn fn) { sendFn_ = std::move(fn); }

    bool isAlive() const {
        return (nowMs() - lastHeartbeat_.load(std::memory_order_acquire)) < timeoutMs_;
    }

    uint32_t missedCount() const { return missedCount_.load(); }

private:
    void monitorLoop() {
        while (running_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs_));
            if (!running_.load(std::memory_order_acquire)) break;

            // Send our heartbeat
            if (sendFn_) sendFn_();

            // Check peer's heartbeat
            uint64_t elapsed = nowMs() - lastHeartbeat_.load(std::memory_order_acquire);
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

    // epochPath (optional): when non-empty, the fencing epoch is persisted
    // there and reloaded on construction, so it survives a restart instead of
    // resetting to 0 — which would let a restarted node regress its epoch and
    // risk split brain. Empty = in-memory only (legacy behavior).
    explicit LeaderLease(uint32_t nodeId, uint64_t leaseDurationMs = 5000,
                         std::string epochPath = "")
        : nodeId_(nodeId), leaseDurationMs_(leaseDurationMs) {
        if (!epochPath.empty()) {
            epochStore_ = std::make_unique<EpochStore>(std::move(epochPath));
            epoch_.store(epochStore_->load());
        }
    }

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
        persistEpoch();   // durable before this epoch is advertised/acted upon
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

    // Accept a lease from a remote primary (used by backups). Two
    // accept paths:
    //   * New (higher) epoch: replace the local lease entirely.
    //   * Same epoch from the same holder: refresh grantedAtMs and
    //     durationMs without bumping epoch_. This is the path that
    //     makes packet-loss fencing work — without it, only the
    //     first LeaseGrant from a primary is accepted and the local
    //     lease silently ages until expired, leaving tryAcquire()
    //     unfenced and allowing split brain. (Original spec failed
    //     to model this because the TLA+ lease is a single global
    //     variable, not per-node.)
    void acceptRemoteLease(const Lease& remote) {
        std::lock_guard<std::mutex> lock(mu_);
        if (remote.epoch > epoch_) {
            epoch_ = remote.epoch;
            currentLease_ = remote;
            persistEpoch();   // a higher epoch learned from a peer is durable too
        } else if (remote.epoch == epoch_ &&
                   remote.holderId == currentLease_.holderId) {
            currentLease_.grantedAtMs = remote.grantedAtMs;
            currentLease_.durationMs = remote.durationMs;
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

    // Persist the current epoch durably (no-op when no epoch store is
    // configured). Called under mu_ after every epoch bump so the durable
    // value is updated before the new epoch is acted upon.
    void persistEpoch() {
        if (epochStore_) epochStore_->store(epoch_.load());
    }

    uint32_t nodeId_;
    uint64_t leaseDurationMs_;
    mutable std::mutex mu_;
    std::atomic<uint64_t> epoch_{0};
    Lease currentLease_{};
    std::unique_ptr<EpochStore> epochStore_;
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

// Payload for ReplicationHeader::Type::LeaseGrant. The primary
// broadcasts its lease duration; the backup uses its own local
// monotonic clock for grantedAtMs (sidesteps wall-clock skew).
#pragma pack(push, 1)
struct LeaseGrantPayload {
    uint64_t durationMs;
};
#pragma pack(pop)
static_assert(sizeof(LeaseGrantPayload) == 8,
              "LeaseGrantPayload must be exactly 8 bytes");

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
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;

        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(static_cast<uint16_t>(port));

        if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(fd);
            return false;
        }

        if (listen(fd, 8) < 0) {
            close(fd);
            return false;
        }

        listenFd_.store(fd, std::memory_order_release);
        return true;
    }

    // Backup mode: connect to primary. Stores host/port so the
    // receive loop can re-establish the connection automatically
    // after a peer crash + recovery, without the coordinator (or
    // an external orchestrator) having to re-invoke connectTo().
    bool connectTo(const std::string& host, int port) {
        if (!host.empty()) {
            connectHost_ = host;
            connectPort_ = port;
        }
        return doConnect();
    }

    void setMessageHandler(OnMessageFn fn) { onMessage_ = std::move(fn); }

    void startReceiving() {
        if (receiving_.exchange(true)) return;
        recvThread_ = std::thread([this] { receiveLoop(); });
    }

    void stop() {
        receiving_.store(false, std::memory_order_release);
        // shutdown() before close() wakes up any blocked recv()/accept()
        // in receiveLoop, which would otherwise hang on the syscall
        // until the kernel processes the close. shutdown(SHUT_RDWR)
        // sends a TCP RST/FIN and recv returns 0 immediately on the
        // other thread; then we close to release the fd number.
        int lfd = listenFd_.exchange(-1, std::memory_order_acq_rel);
        if (lfd >= 0) {
            ::shutdown(lfd, SHUT_RDWR);
            ::close(lfd);
        }
        int pfd = peerFd_.exchange(-1, std::memory_order_acq_rel);
        if (pfd >= 0) {
            ::shutdown(pfd, SHUT_RDWR);
            ::close(pfd);
        }
        if (recvThread_.joinable()) recvThread_.join();
    }

    // Send a message to the peer. Loads peerFd_ once and uses the
    // local — avoids a TOCTOU between the "is fd valid?" check and
    // the syscall use (stop() can null peerFd_ between the two).
    // Residual: if stop() closes the fd between our load and the
    // syscall and the kernel reuses the fd number, we'd write to the
    // wrong file. Acceptable for now — stop() is the teardown path,
    // not a hot path. Full fix would require an fd-refcount or
    // per-send lock that includes the fd lifetime.
    bool send(ReplicationHeader::Type type, uint64_t epoch,
              uint32_t senderId, const uint8_t* payload, size_t payloadLen) {
        int fd = peerFd_.load(std::memory_order_acquire);
        if (fd < 0) return false;

        ReplicationHeader hdr{};
        hdr.magic = ReplicationHeader::MAGIC;
        hdr.type = type;
        hdr.epoch = epoch;
        hdr.senderId = senderId;
        hdr.payloadSize = static_cast<uint32_t>(payloadLen);
        // Atomic increment — multiple threads can reach send()
        // concurrently (heartbeat thread + replicateEntry caller).
        // sendMu_ protects the wire ordering, but the sequence
        // number is read before the lock, so it needs its own
        // synchronization.
        hdr.sequenceNum = sendSeq_.fetch_add(1, std::memory_order_relaxed) + 1;

        // Send header + payload atomically
        std::lock_guard<std::mutex> lock(sendMu_);

        if (sendAll(fd, reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr)) < 0)
            return false;
        if (payloadLen > 0 && sendAll(fd, payload, payloadLen) < 0)
            return false;

        bytesSent_.fetch_add(sizeof(hdr) + payloadLen, std::memory_order_relaxed);
        return true;
    }

    bool sendHeartbeat(uint64_t epoch, uint32_t senderId) {
        return send(ReplicationHeader::Type::Heartbeat, epoch, senderId, nullptr, 0);
    }

    // Accept a single backup connection (blocking). After a
    // successful accept, fires `onPeerConnected_` if set — the
    // primary-side hook used to stream a snapshot to the joining
    // backup before resuming live replication.
    bool acceptOne() {
        int lfd = listenFd_.load(std::memory_order_acquire);
        if (lfd < 0) return false;
        struct sockaddr_in clientAddr{};
        socklen_t len = sizeof(clientAddr);
        int new_fd = accept(lfd, reinterpret_cast<struct sockaddr*>(&clientAddr), &len);
        if (new_fd < 0) return false;
        peerFd_.store(new_fd, std::memory_order_release);
        if (onPeerConnected_) onPeerConnected_();
        return true;
    }

    // Hook fired after a successful accept (primary) or connect
    // (backup). Used by the coordinator's snapshot-on-join path.
    using OnPeerConnectedFn = std::function<void()>;
    void setOnPeerConnected(OnPeerConnectedFn fn) {
        onPeerConnected_ = std::move(fn);
    }

    uint64_t bytesSent() const { return bytesSent_.load(std::memory_order_relaxed); }
    uint64_t bytesReceived() const { return bytesReceived_.load(std::memory_order_relaxed); }
    bool isConnected() const {
        return peerFd_.load(std::memory_order_acquire) >= 0;
    }

private:
    // Internal connect — separated from public connectTo so the
    // receive loop can retry against the saved host/port without
    // mutating them.
    bool doConnect() {
        if (connectHost_.empty()) return false;

        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(connectHost_.c_str(),
                        std::to_string(connectPort_).c_str(),
                        &hints, &res) != 0) {
            return false;
        }

        int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0) { freeaddrinfo(res); return false; }

        struct timeval tv{3, 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
            close(fd);
            freeaddrinfo(res);
            return false;
        }
        peerFd_.store(fd, std::memory_order_release);
        freeaddrinfo(res);
        return true;
    }

    // Helper: close a stale peerFd and reset to -1. Returns the fd
    // that was closed (or -1 if already -1). Centralizes the
    // "peer died, recv loop notices" path so the atomic exchange is
    // never split across two statements.
    void closePeerFd() {
        int old = peerFd_.exchange(-1, std::memory_order_acq_rel);
        if (old >= 0) ::close(old);
    }

    void receiveLoop() {
        while (receiving_.load(std::memory_order_acquire)) {
            int fd = peerFd_.load(std::memory_order_acquire);
            if (fd < 0) {
                int lfd = listenFd_.load(std::memory_order_acquire);
                // If in listen mode (primary), try to accept.
                if (lfd >= 0) {
                    struct pollfd pfd{lfd, POLLIN, 0};
                    if (poll(&pfd, 1, 100) > 0) {
                        acceptOne();
                    }
                    continue;
                }
                // Client mode (backup) with saved connect params:
                // attempt periodic reconnection. Without this, a
                // primary crash + restart leaves the backup stuck
                // with a dead peerFd_ until something external
                // restarts the backup process.
                if (!connectHost_.empty()) {
                    if (doConnect()) {
                        reconnectDelayMs_ = 500;
                        continue;  // peerFd_ valid now — drop into receive
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(reconnectDelayMs_));
                reconnectDelayMs_ = std::min(reconnectDelayMs_ * 2u, 30000u);
                continue;
            }

            // Read header — use the local fd we just loaded.
            ReplicationHeader hdr{};
            ssize_t n = recvAll(fd, reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr));
            if (n <= 0) {
                closePeerFd();
                continue;
            }

            if (hdr.magic != ReplicationHeader::MAGIC) {
                // Corrupted stream — close and reconnect
                closePeerFd();
                continue;
            }

            // Bound the declared payload size BEFORE allocating. A corrupt or
            // hostile header could otherwise request a multi-gigabyte resize
            // (memory-exhaustion DoS). The largest legitimate message is a
            // single JournalEntry (a few hundred bytes); 1 MiB is a generous
            // ceiling that still rejects pathological sizes.
            static constexpr uint32_t kMaxPayloadSize = 1u << 20;  // 1 MiB
            if (hdr.payloadSize > kMaxPayloadSize) {
                closePeerFd();
                continue;
            }

            // Read payload
            std::vector<uint8_t> payload;
            if (hdr.payloadSize > 0) {
                payload.resize(hdr.payloadSize);
                n = recvAll(fd, payload.data(), hdr.payloadSize);
                if (n <= 0) {
                    closePeerFd();
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

    // Atomic so the receive loop's reads and stop()'s writes don't
    // race per TSan. Acquire/release ordering pairs the fd write
    // (in stop, doConnect, acceptOne) with the fd read in
    // receiveLoop/send/isConnected.
    std::atomic<int> listenFd_{-1};
    std::atomic<int> peerFd_{-1};
    std::atomic<bool> receiving_{false};
    std::thread recvThread_;
    std::mutex sendMu_;
    std::atomic<uint32_t> sendSeq_{0};
    std::atomic<uint64_t> bytesSent_{0};
    std::atomic<uint64_t> bytesReceived_{0};
    OnMessageFn onMessage_;
    OnPeerConnectedFn onPeerConnected_;

    // Auto-reconnect support for backup (client) mode. Empty host =
    // primary mode; populated host = backup mode, receive loop will
    // attempt connectTo() with these on every disconnection.
    std::string connectHost_;
    int connectPort_{0};
    // Exponential backoff state for reconnection attempts.
    // Starts at 500ms, doubles on each failure, caps at 30s.
    // Reset to 500ms on successful connection.
    uint32_t reconnectDelayMs_{500};
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

        // Set up heartbeat sender. Three jobs per tick:
        //   1. Renew the primary's own lease.
        //   2. Send a Heartbeat so the backup's "peer alive" timer
        //      gets refreshed.
        //   3. Send a LeaseGrant so the backup's *local* LeaderLease
        //      mirrors the primary's. Without (3), the backup's
        //      tryAcquire() in its failure-callback path would see
        //      no valid lease holder and promote — opening a
        //      split-brain window under packet loss (chaos suite
        //      caught this — the heartbeat-missed event and
        //      lease-expired event are NOT the same thing).
        heartbeat_.setSendFunction([this]() -> bool {
            lease_.renew();
            bool ok = transport_.sendHeartbeat(lease_.epoch(), nodeId_);
            if (ok) {
                LeaseGrantPayload grant{};
                grant.durationMs = lease_.currentLease().durationMs;
                transport_.send(ReplicationHeader::Type::LeaseGrant,
                                lease_.epoch(), nodeId_,
                                reinterpret_cast<const uint8_t*>(&grant),
                                sizeof(grant));
            }
            return ok;
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
                    // Parse the primary's published durationMs; fall
                    // back to the local default if a legacy peer
                    // sends an empty payload.
                    uint64_t durationMs = 5000;
                    if (len >= sizeof(LeaseGrantPayload)) {
                        LeaseGrantPayload grant{};
                        std::memcpy(&grant, payload, sizeof(grant));
                        durationMs = grant.durationMs;
                    }
                    LeaderLease::Lease remoteLease{};
                    remoteLease.epoch = hdr.epoch;
                    remoteLease.holderId = hdr.senderId;
                    remoteLease.grantedAtMs = nowMs();
                    remoteLease.durationMs = durationMs;
                    lease_.acceptRemoteLease(remoteLease);
                }
                if (hdr.type == ReplicationHeader::Type::SnapshotStart) {
                    // Primary is about to stream a full state snapshot.
                    // Buffer any live JournalEntry messages that arrive
                    // concurrently so we do not apply a Cancel before
                    // the corresponding snapshot entry has been applied.
                    inSnapshot_ = true;
                    snapshotBuffer_.clear();
                }
                if (hdr.type == ReplicationHeader::Type::SnapshotEnd) {
                    // Snapshot stream complete. Replay buffered entries
                    // in order, then resume normal live application.
                    inSnapshot_ = false;
                    if (journalApplyCallback_) {
                        for (auto& entry : snapshotBuffer_) {
                            journalApplyCallback_(entry.data(), entry.size());
                        }
                    }
                    snapshotBuffer_.clear();
                }
                if (hdr.type == ReplicationHeader::Type::JournalEntry) {
                    if (inSnapshot_) {
                        // Snapshot in progress — buffer and defer application
                        // until SnapshotEnd to avoid applying a Cancel before
                        // its corresponding order exists in the replica.
                        snapshotBuffer_.emplace_back(payload, payload + len);
                    } else {
                        // No snapshot in flight — apply immediately.
                        if (journalApplyCallback_) {
                            journalApplyCallback_(payload, len);
                        }
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

    // Ship a journal entry to backup(s). Counts every successful
    // ship so operators can scrape /prometheus and see live
    // replication throughput.
    bool replicateEntry(const uint8_t* entry, size_t len) {
        if (role_ != NodeRole::Primary) return false;
        bool ok = transport_.send(ReplicationHeader::Type::JournalEntry,
                                  lease_.epoch(), nodeId_, entry, len);
        if (ok) {
            // Function-static counters: registered on first call,
            // hot path is a direct atomic increment thereafter.
            static auto& kShipped = MetricsRegistry::instance().counter(
                "replication_entries_shipped_total",
                "Journal entries successfully shipped to replication peer");
            static auto& kBytes = MetricsRegistry::instance().counter(
                "replication_bytes_sent_total",
                "Bytes sent on the replication transport (payload only)");
            kShipped.increment(1);
            kBytes.increment(len);
        }
        return ok;
    }

    // Callbacks
    using PromotionCallback = std::function<void()>;
    using JournalApplyCallback = std::function<void(const uint8_t* data, size_t len)>;

    void setPromotionCallback(PromotionCallback cb) { promotionCallback_ = std::move(cb); }
    void setJournalApplyCallback(JournalApplyCallback cb) { journalApplyCallback_ = std::move(cb); }

    // Snapshot-on-join hook. Set on the primary; fires after a
    // backup successfully connects. The callback typically iterates
    // engine state via MatchingEngine::streamSnapshot and ships each
    // resting order via replicateEntry().
    //
    // The transport wraps the callback with SnapshotStart / SnapshotEnd
    // markers so the backup can buffer live JournalEntry messages that
    // arrive concurrently during the snapshot stream, then replay them
    // after all snapshot entries are applied — eliminating the race where
    // a Cancel arrives before the snapshot entry for the same order.
    using OnPeerJoinedCallback = std::function<void()>;
    void setOnPeerJoined(OnPeerJoinedCallback cb) {
        transport_.setOnPeerConnected([this, cb = std::move(cb)]() {
            transport_.send(ReplicationHeader::Type::SnapshotStart,
                            lease_.epoch(), nodeId_, nullptr, 0);
            if (cb) cb();
            transport_.send(ReplicationHeader::Type::SnapshotEnd,
                            lease_.epoch(), nodeId_, nullptr, 0);
        });
    }

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

    // Snapshot-race guard (backup side).
    // Set true when a SnapshotStart is received; cleared on SnapshotEnd.
    // JournalEntry messages that arrive while inSnapshot_ is true are
    // held in snapshotBuffer_ and replayed after all snapshot entries
    // have been applied, preventing Cancel-before-Insert phantoms.
    bool inSnapshot_{false};
    std::vector<std::vector<uint8_t>> snapshotBuffer_;
};

}  // namespace OrderMatcher
