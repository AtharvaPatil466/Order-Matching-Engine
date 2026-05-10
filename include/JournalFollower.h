#pragma once

// JournalFollower — a minimal state-machine-replication primitive.
//
// Tails a journal file written by a leader engine; reads any new entries
// since the last poll; applies them in order to a follower OrderBook (in
// replay mode). When the leader has stopped, the follower book converges
// to byte-identical state with the leader.
//
// This is the simplest possible HA building block — log shipping with no
// consensus. It buys you:
//   * A warm standby that can be promoted by halting the leader and
//     stopping the follower's tail (the follower book is already current).
//   * An always-on read replica for market data / analytics that
//     doesn't compete with the leader's hot path.
//
// What this does NOT provide:
//   * Failover automation. A coordinator must detect leader death and
//     promote.
//   * Multi-leader / split-brain handling. Single writer assumed.
//   * Network transport. The "log shipping" here is via shared
//     filesystem; for cross-host replication the journal file must be
//     mirrored externally (NFS, block-level replication, or a
//     dedicated transport). That's the missing layer for real HA.
//   * Quorum / fencing. A misconfigured pair could double-write.
//
// Use cases this is sufficient for:
//   * Single-host hot-standby (process pair on the same machine).
//   * Single-host crash-recovery rehearsal (run the follower against
//     yesterday's journal to validate replay determinism).

#include "Journal.h"
#include "OrderBook.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>

namespace OrderMatcher {

class JournalFollower {
public:
    // pollIntervalMs: how often to check the journal for new entries.
    // 1ms is fine on local FS; larger if tailing a network mount.
    JournalFollower(const std::string& journalPath,
                    OrderBook& target,
                    uint32_t pollIntervalMs = 1)
        : path_(journalPath), target_(target), pollIntervalMs_(pollIntervalMs) {
        target_.setReplayMode(true);
    }

    ~JournalFollower() { stop(); }

    JournalFollower(const JournalFollower&) = delete;
    JournalFollower& operator=(const JournalFollower&) = delete;

    void start() {
        if (running_.exchange(true)) return;
        thread_ = std::thread([this] { loop(); });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
    }

    // Number of entries the follower has applied so far. Useful for tests
    // and for a coordinator deciding when "promotion" is safe (i.e., the
    // follower has caught up to a known leader sequence).
    uint64_t appliedCount() const {
        return appliedCount_.load(std::memory_order_acquire);
    }

    // Single-shot catch-up. Reads any new entries from disk and applies
    // them, then returns. Useful in deterministic tests to avoid sleeps.
    void poll() { tickOnce(); }

    // Promote the follower's book into a writable leader.
    //
    // Sequence:
    //   1. stop() the polling thread (joins, drains the final tick).
    //   2. Drain once more — picks up any entries the leader wrote
    //      between the last poll and the moment we're called.
    //   3. Exit replay mode so the underlying engine will accept new
    //      orders normally.
    //
    // After promote() returns, the caller can drive new orders through
    // the now-leader OrderBook. If they want continued journaling, they
    // should attach a Journal at the same path (the file is append-only
    // and the new leader's entries will continue from the existing
    // sequence).
    //
    // What this does NOT do:
    //   * Fence the previous leader. A coordinator must guarantee the
    //     prior leader is dead/disconnected before calling promote();
    //     otherwise both nodes will write to the same journal and
    //     corrupt the log.
    //   * Verify "caught up to a known leader sequence". Caller should
    //     check appliedCount() against an expected high-water mark
    //     before promoting if they need that guarantee.
    //   * Re-attach a writable Journal — the caller does that.
    void promote() {
        stop();
        tickOnce();  // drain any tail the writer left after our last poll
        target_.setReplayMode(false);
        promoted_ = true;
    }

    bool promoted() const { return promoted_; }

private:
    void loop() {
        while (running_.load(std::memory_order_acquire)) {
            tickOnce();
            std::this_thread::sleep_for(
                std::chrono::milliseconds(pollIntervalMs_));
        }
        // Final drain on stop — the leader may have written entries we
        // haven't picked up yet.
        tickOnce();
    }

    void tickOnce() {
        // Read all CRC + sequence-validated entries from the journal.
        // The journal is append-only and entries are immutable once
        // written, so re-reading the prefix every poll is safe.
        // For larger logs we'd remember the file offset and read only
        // the new bytes; left as a future optimisation since the
        // current readAll is fast enough for unit tests.
        auto entries = Journal(path_).readAll(/*validateCRC=*/true,
                                               /*validateSequence=*/true);
        uint64_t alreadyApplied = appliedCount_.load(std::memory_order_relaxed);
        for (size_t i = alreadyApplied; i < entries.size(); ++i) {
            applyEntry(entries[i]);
        }
        if (entries.size() > alreadyApplied) {
            appliedCount_.store(entries.size(), std::memory_order_release);
        }
    }

    void applyEntry(const JournalEntry& e) {
        switch (e.entryType) {
        case JournalEntry::Type::AddOrder:
            target_.addOrder(e.orderId, e.participantId, e.side, e.price,
                             e.quantity, e.orderType, e.stopPrice,
                             e.displayQty, e.timeInForce, e.expiryTime,
                             e.stopLimitPrice, e.pegType, e.pegOffset,
                             e.trailAmount, e.minQty, e.hidden);
            break;
        case JournalEntry::Type::CancelOrder:
            target_.cancelOrder(e.orderId);
            break;
        case JournalEntry::Type::ModifyOrder:
            target_.modifyOrder(e.orderId, e.newQty);
            break;
        case JournalEntry::Type::CancelReplace:
            target_.cancelReplace(e.orderId, e.newPrice, e.newQty);
            break;
        case JournalEntry::Type::Snapshot:
            target_.addOrder(e.orderId, e.participantId, e.side, e.price,
                             e.quantity, e.orderType);
            break;
        }
    }

    std::string         path_;
    OrderBook&          target_;
    uint32_t            pollIntervalMs_;
    std::atomic<bool>   running_{false};
    bool                promoted_{false};
    std::thread         thread_;
    std::atomic<uint64_t> appliedCount_{0};
};

}  // namespace OrderMatcher
