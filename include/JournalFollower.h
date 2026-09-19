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
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

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

    // Number of entries applied from the journal file AS IT STANDS NOW —
    // equivalently, the follower's position in the current file. A checkpoint
    // replaces the file with a snapshot numbered from 1, so this counter goes
    // DOWN across one: it is a position, not a monotonic lifetime total, and a
    // coordinator must not treat it as a high-water mark that only grows. To
    // decide promotion safety, compare it against the leader's CURRENT journal
    // sequence (Journal::getSequence), which is renumbered by the same
    // checkpoint and therefore stays comparable.
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
        //
        // Torn trailing record: there is NO advisory lock (no flock/lockf)
        // between the leader's writer and this reader — we tolerate, rather
        // than exclude, a concurrent in-flight append. That is safe because
        // readAll()/readEntriesFromPath() reads whole fixed-size records via
        // fread(sizeof(JournalEntry)) and rejects any record that is not
        // intact:
        //   * A short final record (fewer than sizeof(JournalEntry) bytes on
        //     disk yet) makes fread return 0, so the loop stops WITHOUT
        //     counting it — the partial tail is dropped.
        //   * A full-length but torn/garbled final record fails the per-entry
        //     CRC32 (or the contiguous-sequence check) and is likewise dropped
        //     rather than applied.
        // So entries.size() never includes a partial trailing record; on a
        // later poll, once the writer has committed it in full, it appears and
        // is applied then. appliedCount_ only ever advances over fully-intact,
        // CRC-valid, contiguously-sequenced entries.
        auto entries = Journal(path_).readAll(/*validateCRC=*/true,
                                               /*validateSequence=*/true);

        size_t applied =
            static_cast<size_t>(appliedCount_.load(std::memory_order_relaxed));
        switch (classify(entries, applied)) {
        case Continuity::Inconclusive:
            // This read tells us nothing trustworthy. Change NOTHING and try
            // again next poll — see classify() for the cases that land here.
            return;
        case Continuity::Replaced:
            // See rebuild() for why the whole book goes, not just the delta.
            rebuild();
            applied = 0;
            break;
        case Continuity::Appended:
            break;
        }
        for (size_t i = applied; i < entries.size(); ++i) {
            applyEntry(entries[i]);
        }
        if (!entries.empty()) {
            firstOnDisk_ = entries.front();
            lastApplied_ = entries.back();
        }
        appliedCount_.store(entries.size(), std::memory_order_release);
    }

    enum class Continuity {
        Appended,      // same file, grown (or unchanged) — apply the tail
        Replaced,      // a different file is at this path — rebuild from it
        Inconclusive   // this read proves nothing — do not act on it
    };

    // Is `entries` the same log we have been following, grown by appends — or a
    // different file that replaced it?
    //
    // WHY NOT A COUNT, AND WHY NOT A SEQUENCE NUMBER. A checkpoint builds a
    // snapshot of every resting order in <path>.tmp and rename(2)s it over the
    // journal (Journal::prepareRewrite/commitRewrite). That is a REPLACEMENT,
    // and it breaks both of the obvious progress marks:
    //   * A positional index breaks because the snapshot holds one record per
    //     resting order — almost always FEWER than the history it replaces. The
    //     index then points past the end and the follower stops forever; if the
    //     file later grows past it, the index lands on an unrelated record and
    //     silently skips everything before it.
    //   * A last-applied sequence number breaks because the snapshot is written
    //     into a truncated temp journal, so it is renumbered from 1
    //     (Journal::truncate + commitRewrite's recoverSequenceFromDisk). Every
    //     record in the new file is at or below the old mark and would be
    //     skipped wholesale.
    // So the mark has to identify the FILE, not a position in it.
    //
    // WHY BYTE IDENTITY RATHER THAN st_ino. An inode compare needs the stat and
    // the read to observe the same file — but Journal::readAll opens by path
    // internally, so a rename landing between our stat and its open would be
    // invisible. Byte identity has no such window: the anchors are taken from
    // the very vector we are about to apply from, so what we check IS what we
    // read. It also catches an in-place rewrite (truncate + rewrite keeps the
    // inode, which an st_ino check would wave through).
    //
    // Two anchors:
    //   * entries.front() decides WHICH FILE this is. Any rewrite writes
    //     record 1 from scratch with a fresh now() timestamp, so a replacement
    //     cannot reproduce the old record 1 and an append cannot alter it.
    //   * entries[applied-1] proves the prefix we already applied has not
    //     shifted underneath us.
    // A journal is append-only between rewrites and records are immutable once
    // written, so for a genuine append both anchors match by construction.
    //
    // WHY SHRINKAGE IS NOT THE SIGNAL, AND WHY THERE IS A THIRD ANSWER. Four
    // cases rule out "the record count went down" as a replacement test, and
    // three of them are the reason Inconclusive exists rather than defaulting
    // to one of the other two:
    //   1. A mid-file CRC failure truncates the READ, not the file: readAll
    //      stops at the bad record and returns a short prefix forever. Treating
    //      that as a replacement would throw away a good book and rebuild it
    //      from a corrupt prefix. Record 1 still matches, so we answer
    //      Inconclusive and hold position — the same conservative posture the
    //      journal takes toward a corrupt record.
    //   2. readEntriesFromPath fopen()s the file and then headerBytesOf()
    //      fopen()s it AGAIN; a rename landing between the two can pair the old
    //      inode's bytes with the new file's header offset, misframing every
    //      record into an empty result. An empty read therefore carries no
    //      information about the file we were following, so it is never
    //      grounds to rebuild — the next poll reads a settled file.
    //   3. A checkpoint of a book with N >= applied replaces the file WITHOUT
    //      shrinking it. Count says "grew"; record 1 says "different file".
    //      The anchor is what decides, so this is caught.
    //   4. Journal::truncate() rewrites in place and KEEPS the inode, which is
    //      why this is a content check and not an st_ino check.
    Continuity classify(const std::vector<JournalEntry>& entries,
                        size_t applied) const {
        if (applied == 0) return Continuity::Appended;  // nothing to contradict
        if (entries.empty()) return Continuity::Inconclusive;        // case 2
        if (!sameRecord(entries.front(), firstOnDisk_)) {
            return Continuity::Replaced;                             // case 3/4
        }
        // Same file. It cannot legitimately have lost records or rewritten one
        // we already applied, so anything of that shape is corruption, not
        // progress: hold position rather than act on it.
        if (entries.size() < applied) return Continuity::Inconclusive;  // case 1
        if (!sameRecord(entries[applied - 1], lastApplied_)) {
            return Continuity::Inconclusive;
        }
        return Continuity::Appended;
    }

    // JournalEntry is #pragma pack(1) POD — no padding, so memcmp is an exact
    // record compare.
    static bool sameRecord(const JournalEntry& a, const JournalEntry& b) {
        return std::memcmp(&a, &b, sizeof(JournalEntry)) == 0;
    }

    // Drop every resting order so the replacement file can be applied from
    // record 1 onto an empty book.
    //
    // WHY NOT JUST RE-APPLY THE SNAPSHOT ON TOP. Because the follower may be
    // BEHIND the checkpoint: the leader can append records and checkpoint
    // before our next poll. The snapshot then describes the book at a point
    // after entries we never saw, and those entries may have cancelled, filled
    // or resized orders we are still holding. Adding the snapshot's orders on
    // top only fixes what is missing, never what is stale, so the follower
    // would diverge permanently. A snapshot is a COMPLETE state description,
    // so the sound move is to discard local state and take it wholesale.
    //
    // ponytail: cancel-per-order rather than a new OrderBook::clear(). O(book)
    // once per checkpoint, off the leader's hot path entirely. cancelOrder()
    // also unwinds participant exposure and returns pool slots, which a naive
    // container clear would leak.
    void rebuild() {
        std::vector<OrderId> ids;
        // Collect first, cancel second: forEachOrderLocked holds bookLock_ and
        // cancelOrder takes it — the mutex is not recursive.
        target_.forEachOrderLocked([&ids](const Order& o) { ids.push_back(o.id); });
        for (OrderId id : ids) {
            target_.cancelOrder(id);
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
            // Same shape as MatchingEngine::replayJournal and
            // applyReplicatedEntry, which apply this record type from disk and
            // from the wire. The 6-argument addOrder used here before dropped
            // timeInForce, expiryTime, stopPrice, stopLimitPrice, displayQty,
            // pegType, pegOffset, trailAmount, minQty and hidden — so a GTD,
            // iceberg, pegged or stop-limit order came back from a checkpoint
            // as a plain GTC limit. The getOrder() guard keeps a re-applied
            // snapshot idempotent instead of a DuplicateOrderId reject.
            if (!target_.getOrder(e.orderId)) {
                target_.addOrder(e.orderId, e.participantId, e.side, e.price,
                                 e.quantity, e.orderType, e.stopPrice,
                                 e.displayQty, e.timeInForce, e.expiryTime,
                                 e.stopLimitPrice, e.pegType, e.pegOffset,
                                 e.trailAmount, e.minQty, e.hidden);
            }
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
    // Stream-identity anchors for the file we are currently following. Touched
    // only by the polling thread (loop(), poll(), promote() — never
    // concurrently, since promote() joins first). See continuesAppliedPrefix().
    JournalEntry        firstOnDisk_{};
    JournalEntry        lastApplied_{};
};

}  // namespace OrderMatcher
