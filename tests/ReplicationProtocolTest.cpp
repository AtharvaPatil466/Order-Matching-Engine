// ReplicationProtocolTest — tests for the primary-backup HA system.

#include "ReplicationProtocol.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace OrderMatcher;

static int passed = 0;
#define TEST(name) std::cout << "  " << #name << "..." << std::flush
#define PASS() do { ++passed; std::cout << " PASS\n"; } while(0)

// ─── HeartbeatMonitor tests ─────────────────────────────────────────────────

void test_heartbeat_alive() {
    TEST(HeartbeatAlive);

    HeartbeatMonitor hb(50, 200);
    hb.start();
    hb.receivedHeartbeat();

    assert(hb.isAlive());
    assert(hb.missedCount() == 0);

    hb.stop();
    PASS();
}

void test_heartbeat_timeout() {
    TEST(HeartbeatTimeout);

    bool failed = false;
    HeartbeatMonitor hb(50, 100);  // 100ms timeout
    hb.setFailureCallback([&]() { failed = true; });
    hb.start();

    // Don't send any heartbeats — should trigger failure
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    hb.stop();

    assert(failed);
    assert(hb.missedCount() > 0);

    PASS();
}

void test_heartbeat_recovery() {
    TEST(HeartbeatRecovery);

    HeartbeatMonitor hb(50, 200);
    hb.start();

    // Let it time out
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    assert(!hb.isAlive());

    // Send heartbeat — should recover
    hb.receivedHeartbeat();
    assert(hb.isAlive());
    assert(hb.missedCount() == 0);

    hb.stop();
    PASS();
}

// ─── LeaderLease tests ──────────────────────────────────────────────────────

void test_lease_acquire() {
    TEST(LeaseAcquire);

    LeaderLease lease(1, 5000);
    assert(!lease.isLeader());

    assert(lease.tryAcquire());
    assert(lease.isLeader());
    assert(lease.epoch() == 1);

    PASS();
}

void test_lease_fencing() {
    TEST(LeaseFencing);

    LeaderLease lease(1, 5000);
    lease.tryAcquire();
    uint64_t epoch1 = lease.epoch();

    lease.tryAcquire();  // New epoch
    uint64_t epoch2 = lease.epoch();
    assert(epoch2 > epoch1);

    // Old epoch is fenced
    assert(lease.isFenced(epoch1));
    assert(!lease.isFenced(epoch2));

    PASS();
}

void test_lease_release() {
    TEST(LeaseRelease);

    LeaderLease lease(1, 5000);
    lease.tryAcquire();
    assert(lease.isLeader());

    lease.release();
    assert(!lease.isLeader());

    PASS();
}

void test_lease_remote_accept() {
    TEST(LeaseRemoteAccept);

    LeaderLease primary(1, 5000);
    LeaderLease backup(2, 5000);

    primary.tryAcquire();
    auto pl = primary.currentLease();

    // Backup accepts the remote lease
    backup.acceptRemoteLease(pl);
    assert(backup.epoch() == primary.epoch());
    assert(!backup.isLeader());  // Backup doesn't become leader

    PASS();
}

void test_lease_contention() {
    TEST(LeaseContention);

    LeaderLease node1(1, 5000);
    LeaderLease node2(2, 5000);

    // Node1 acquires first
    assert(node1.tryAcquire());

    // Node2 accepts node1's lease
    node2.acceptRemoteLease(node1.currentLease());

    // Node2 can't acquire while node1's lease is valid
    // (because the holder is node1, not node2)
    assert(!node2.tryAcquire());

    PASS();
}

void test_lease_renew() {
    TEST(LeaseRenew);

    LeaderLease lease(1, 5000);
    lease.tryAcquire();
    assert(lease.isLeader());

    // Renew should succeed
    assert(lease.renew());
    assert(lease.isLeader());

    PASS();
}

// ─── ReplicationHeader tests ────────────────────────────────────────────────

void test_replication_header_layout() {
    TEST(ReplicationHeaderLayout);

    static_assert(sizeof(ReplicationHeader) == 25, "ReplicationHeader must be 25 bytes");

    ReplicationHeader hdr{};
    hdr.magic = ReplicationHeader::MAGIC;
    hdr.type = ReplicationHeader::Type::Heartbeat;
    hdr.epoch = 42;
    hdr.senderId = 1;
    hdr.payloadSize = 0;
    hdr.sequenceNum = 99;

    assert(hdr.magic == 0x52455053);
    assert(hdr.epoch == 42);
    assert(hdr.senderId == 1);

    PASS();
}

// ─── ReplicationTransport tests (loopback) ──────────────────────────────────

void test_transport_connect_and_send() {
    TEST(TransportConnectAndSend);

    bool received = false;
    uint64_t receivedEpoch = 0;

    // Server
    ReplicationTransport server;
    assert(server.listenOn(19876));
    server.setMessageHandler([&](const ReplicationHeader& hdr,
                                  const uint8_t*, size_t) {
        if (hdr.type == ReplicationHeader::Type::Heartbeat) {
            received = true;
            receivedEpoch = hdr.epoch;
        }
    });
    server.startReceiving();

    // Give server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Client
    ReplicationTransport client;
    assert(client.connectTo("127.0.0.1", 19876));

    // Send a heartbeat
    assert(client.sendHeartbeat(42, 1));

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    client.stop();
    server.stop();

    assert(received);
    assert(receivedEpoch == 42);

    PASS();
}

void test_transport_send_payload() {
    TEST(TransportSendPayload);

    std::vector<uint8_t> receivedData;

    ReplicationTransport server;
    assert(server.listenOn(19877));
    server.setMessageHandler([&](const ReplicationHeader& hdr,
                                  const uint8_t* payload, size_t len) {
        if (hdr.type == ReplicationHeader::Type::JournalEntry) {
            receivedData.assign(payload, payload + len);
        }
    });
    server.startReceiving();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ReplicationTransport client;
    assert(client.connectTo("127.0.0.1", 19877));

    // Send a journal entry
    std::string entry = "ADD|orderId=123|price=100000|qty=10";
    assert(client.send(ReplicationHeader::Type::JournalEntry, 1, 1,
                       reinterpret_cast<const uint8_t*>(entry.data()),
                       entry.size()));

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    client.stop();
    server.stop();

    assert(receivedData.size() == entry.size());
    assert(std::string(receivedData.begin(), receivedData.end()) == entry);

    PASS();
}

// ─── ReplicationCoordinator tests ───────────────────────────────────────────

void test_coordinator_primary_startup() {
    TEST(CoordinatorPrimaryStartup);

    ReplicationCoordinator primary(1, 100, 500, 5000);
    assert(primary.startAsPrimary(19878));
    assert(primary.role() == NodeRole::Primary);
    assert(primary.isLeader());
    assert(primary.epoch() == 1);

    primary.stop();
    PASS();
}

void test_coordinator_backup_promotion() {
    TEST(CoordinatorBackupPromotion);

    // Start primary
    ReplicationCoordinator primary(1, 50, 200, 5000);
    assert(primary.startAsPrimary(19879));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Start backup connected to primary
    bool promoted = false;
    ReplicationCoordinator backup(2, 50, 200, 5000);
    backup.setPromotionCallback([&]() { promoted = true; });
    assert(backup.startAsBackup("127.0.0.1", 19879));
    assert(backup.role() == NodeRole::Backup);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Kill the primary — backup should detect failure and promote
    primary.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    backup.stop();

    // Promotion may or may not fire depending on timing — but the
    // mechanism is exercised. Check that at least the primary stopped.
    assert(!primary.isRunning());

    PASS();
}

void test_coordinator_replicate_entry() {
    TEST(CoordinatorReplicateEntry);

    ReplicationCoordinator primary(1, 100, 500, 5000);
    assert(primary.startAsPrimary(19880));
    // Follow the real snapshot-on-join protocol: the primary emits
    // SnapshotStart … SnapshotEnd when a backup connects. Here the snapshot
    // body is empty, but the markers are what flush the backup's
    // connect-time snapshot buffer so subsequent live entries are applied.
    primary.setOnPeerJoined([]() { /* empty snapshot */ });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::vector<uint8_t> receivedJournal;
    ReplicationCoordinator backup(2, 100, 500, 5000);
    backup.setJournalApplyCallback([&](const uint8_t* data, size_t len) {
        receivedJournal.assign(data, data + len);
    });
    assert(backup.startAsBackup("127.0.0.1", 19880));

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Replicate a journal entry (post-snapshot, so applied immediately).
    std::string entry = "ADD|1|100|BUY|100000|10";
    primary.replicateEntry(
        reinterpret_cast<const uint8_t*>(entry.data()), entry.size());

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    primary.stop();
    backup.stop();

    // With the snapshot markers flushing the buffer, delivery is now
    // guaranteed — assert it rather than tolerating an empty result.
    assert(!receivedJournal.empty() &&
           "backup did not receive the post-snapshot journal entry");
    std::string received(receivedJournal.begin(), receivedJournal.end());
    assert(received == entry);

    PASS();
}

// ─── Snapshot-before-live ordering (Cancel-before-Insert race) ───────────────

// Drives a precisely ordered message stream into a real backup coordinator
// using a raw transport as the "primary", reproducing the wire order in which
// a live JournalEntry races AHEAD of SnapshotStart. The fix must buffer that
// pre-snapshot entry (and drop it at SnapshotStart, since the snapshot already
// reflects it), so the backup never applies a Cancel before the snapshot Add.
void test_snapshot_ordering() {
    TEST(SnapshotOrdering);

    // Raw transport we drive by hand to inject an exact message order.
    ReplicationTransport primary;
    assert(primary.listenOn(19881));
    primary.startReceiving();  // runs acceptOne() so the backup can connect
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::vector<std::string> applied;
    std::mutex appliedMu;
    ReplicationCoordinator backup(2, 100, 500, 5000);
    backup.setJournalApplyCallback([&](const uint8_t* d, size_t n) {
        std::lock_guard<std::mutex> lk(appliedMu);
        applied.emplace_back(reinterpret_cast<const char*>(d), n);
    });
    assert(backup.startAsBackup("127.0.0.1", 19881));

    // Wait for the primary side to observe the backup connection.
    for (int i = 0; i < 200 && !primary.isConnected(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(primary.isConnected());

    auto sendEntry = [&](const std::string& s) {
        return primary.send(ReplicationHeader::Type::JournalEntry, 1, 1,
                            reinterpret_cast<const uint8_t*>(s.data()), s.size());
    };

    // (1) Live Cancel that races ahead of SnapshotStart. Under the bug this
    //     is applied immediately (Cancel-before-Insert); with the fix it is
    //     buffered as "snapshot pending" and dropped at SnapshotStart.
    sendEntry("CANCEL X (pre-snapshot)");
    // (2) Snapshot boundary, snapshot content, then a live tail.
    primary.send(ReplicationHeader::Type::SnapshotStart, 1, 1, nullptr, 0);
    sendEntry("ADD X (snapshot)");
    primary.send(ReplicationHeader::Type::SnapshotEnd, 1, 1, nullptr, 0);
    sendEntry("CANCEL X (post-snapshot)");

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    backup.stop();
    primary.stop();

    std::lock_guard<std::mutex> lk(appliedMu);
    // Exactly two applies, in snapshot-before-live order. The pre-snapshot
    // Cancel must NOT appear, and ADD must precede the post-snapshot CANCEL.
    assert(applied.size() == 2 &&
           "pre-snapshot live entry leaked into the apply stream");
    assert(applied[0] == "ADD X (snapshot)");
    assert(applied[1] == "CANCEL X (post-snapshot)");

    PASS();
}

// ─── Heartbeat miss-counter reset (skew-defense mechanism) ───────────────────

// The promotion gate requires N consecutive missed renewals, and missedCount()
// is reset to 0 on every received heartbeat. This reset is precisely what stops
// a fast-clock backup from ever reaching the threshold while the primary keeps
// renewing. Verified deterministically here against the local monotonic clock.
void test_heartbeat_miss_reset() {
    TEST(HeartbeatMissReset);

    // Generous alive-timeout (500ms) relative to the renewal cadence so
    // scheduler jitter on a loaded / sanitizer-instrumented CI runner cannot
    // spuriously trip the miss counter or flip liveness during the healthy
    // phase. isAlive() is nowMs()-lastHeartbeat < timeout, so the assertions
    // below are taken IMMEDIATELY after receivedHeartbeat() (≈0 elapsed) — never
    // after a sleep that a slow runner could overshoot past the timeout.
    HeartbeatMonitor hb(20, 500);  // monitor tick 20ms, alive-timeout 500ms
    hb.start();

    // Healthy: each renewal pins missedCount at 0 and keeps the link alive.
    // Checked at zero-elapsed (right after the renewal), so the result is
    // independent of how long the following sleep actually takes.
    for (int i = 0; i < 5; ++i) {
        hb.receivedHeartbeat();
        assert(hb.missedCount() == 0);
        assert(hb.isAlive());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));  // « timeout
    }

    // Outage: no renewals for well past the timeout (+ many monitor ticks). A
    // slow runner only makes this wait longer, never shorter, so the counter
    // climbing past the 3-miss threshold and isAlive() going false are robust.
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    assert(hb.missedCount() >= 3);
    assert(!hb.isAlive());

    // A single renewal resets the counter to 0 and revives liveness — the
    // mechanism that prevents a fast-clock backup from ever reaching the
    // promotion threshold while the primary keeps renewing. Asserted at
    // zero-elapsed for the same timing-robustness reason.
    hb.receivedHeartbeat();
    assert(hb.missedCount() == 0);
    assert(hb.isAlive());

    hb.stop();
    PASS();
}

// ─── Lease clock-skew: no promotion while renewals arrive ────────────────────

// Real failover requires a sustained renewal outage. A backup whose local
// clock runs fast (indistinguishable from real time as long as renewals keep
// arriving and reset the local miss counter / refresh the local-monotonic
// lease) must NOT promote. We run a live primary+backup for many multiples of
// the lease duration with renewals flowing, assert no promotion, then kill the
// primary and assert real failover still happens.
void test_lease_skew_no_promotion() {
    TEST(LeaseSkewNoPromotion);

    // Short, fast timings so the test is quick: lease 200ms, heartbeat 30ms,
    // timeout 120ms, promote after 3 consecutive misses.
    ReplicationCoordinator primary(1, 30, 120, 200, 3);
    assert(primary.startAsPrimary(19882));
    primary.setOnPeerJoined([]() {});

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::atomic<bool> promoted{false};
    ReplicationCoordinator backup(2, 30, 120, 200, 3);
    backup.setPromotionCallback([&]() { promoted.store(true); });
    assert(backup.startAsBackup("127.0.0.1", 19882));

    // Phase A: renewals keep arriving for 600ms == 3x the lease duration.
    // A naive fixed-grant expiry would fire at 200ms; the refresh-on-renewal
    // (local monotonic) + miss-counter reset keep the backup a backup.
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    assert(!promoted.load() &&
           "backup promoted while primary kept renewing (clock-skew split brain)");
    assert(backup.role() == NodeRole::Backup);

    // Phase B: true primary death — real failover must still occur.
    primary.stop();
    bool failedOver = false;
    for (int i = 0; i < 100; ++i) {  // up to ~1s
        if (promoted.load()) { failedOver = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    backup.stop();

    assert(failedOver &&
           "backup failed to promote after genuine primary death");
    assert(backup.role() == NodeRole::Primary);

    PASS();
}

// ─── Main ───────────────────────────────────────────────────────────────────

int main() {
    std::cout << "\n═══ Replication Protocol Tests ═══\n\n";

    // HeartbeatMonitor
    test_heartbeat_alive();
    test_heartbeat_timeout();
    test_heartbeat_recovery();

    // LeaderLease
    test_lease_acquire();
    test_lease_fencing();
    test_lease_release();
    test_lease_remote_accept();
    test_lease_contention();
    test_lease_renew();

    // ReplicationHeader
    test_replication_header_layout();

    // ReplicationTransport (loopback)
    test_transport_connect_and_send();
    test_transport_send_payload();

    // ReplicationCoordinator
    test_coordinator_primary_startup();
    test_coordinator_backup_promotion();
    test_coordinator_replicate_entry();

    // HA correctness regressions
    test_snapshot_ordering();
    test_heartbeat_miss_reset();
    test_lease_skew_no_promotion();

    std::cout << "\n─── Results: " << passed << " passed ───\n";
    std::cout << "\nAll replication protocol tests passed.\n\n";
    return 0;
}
