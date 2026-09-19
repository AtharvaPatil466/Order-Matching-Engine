#pragma once

// JournalFollower — a minimal state-machine-replication primitive.
//
// Tails a journal file written by a leader engine; reads any new entries
// since the last poll; routes each by symbol and applies them in order to the
// follower OrderBook(s) it hosts (in replay mode). When the leader has stopped,
// each follower book converges to byte-identical state with the leader's.
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
//
// ONE JOURNAL, MANY SYMBOLS. A leader running N symbols writes all of them
// interleaved into ONE journal; every record carries the symbolId it belongs
// to. So a follower must be told which symbol(s) it hosts and must ROUTE each
// record, never just apply it. A follower that ignored symbolId collapsed all
// N books into one: ids from different symbols shared a book (nothing enforces
// that ids are unique across symbols — the duplicate check is per-book), a
// cancel for one symbol hit another symbol's order of the same id, and orders
// that never met on the leader crossed and produced fills the leader never had.
// Entries for a symbol this follower does not host are SKIPPED — they are not
// errors, and they still advance the cursor (see appliedCount()).

#include "Journal.h"
#include "OrderBook.h"

#include <algorithm>
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
    // Follow ONE symbol into ONE book. `symbol` is explicit and undefaulted:
    // 0 is a legal SymbolId, so a default would silently make every follower a
    // symbol-0 follower — which is how the collapse-into-one-book bug read from
    // the outside. It also sits BEFORE the book so that the pre-symbol call
    // `JournalFollower(path, book, 1)` fails to compile rather than quietly
    // binding 1 as the symbol.
    //
    // pollIntervalMs: how often to check the journal for new entries.
    // 1ms is fine on local FS; larger if tailing a network mount.
    JournalFollower(const std::string& journalPath,
                    SymbolId symbol,
                    OrderBook& target,
                    uint32_t pollIntervalMs = 1)
        : path_(journalPath),
          resolver_([symbol, book = &target](SymbolId s) -> OrderBook* {
              return s == symbol ? book : nullptr;
          }),
          pollIntervalMs_(pollIntervalMs) {
        // The one book is known up front, so it enters replay mode now rather
        // than on first matching record — the single-book follower is in replay
        // mode from construction, as it always was.
        target.setReplayMode(true);
        books_.push_back(&target);
    }

    // Follow several symbols. `resolve` returns the book hosting `symbol`, or
    // nullptr for a symbol this follower does not host.
    //
    // ponytail: a std::function, not a registry class. The caller already has
    // its books in whatever container it prefers; a lambda over that is the
    // whole of the mapping, and one more owning type would have to be kept in
    // sync with it.
    //
    // A resolved book enters replay mode when the follower first sees a record
    // for it, not at construction — the follower cannot enumerate the resolver's
    // domain. Symmetrically, promote() exits replay mode on exactly the books it
    // resolved. A hosted symbol that never appears in the journal is never
    // touched at all, which is what "the follower applied nothing to it" means.
    JournalFollower(const std::string& journalPath,
                    std::function<OrderBook*(SymbolId)> resolve,
                    uint32_t pollIntervalMs = 1)
        : path_(journalPath), resolver_(std::move(resolve)),
          pollIntervalMs_(pollIntervalMs) {}

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

    // The follower's POSITION in the journal file AS IT STANDS NOW: how many
    // records it has consumed, which is not the same as how many it applied.
    // An entry for a symbol this follower does not host is skipped and still
    // counted, because this is an index into the file, not a tally of
    // mutations. Letting filtering hold it back would desynchronise it from the
    // file and the follower would re-read (and re-apply) records it had already
    // passed.
    //
    // A checkpoint replaces the file with a snapshot numbered from 1, so this
    // counter goes DOWN across one: it is a position, not a monotonic lifetime
    // total, and a coordinator must not treat it as a high-water mark that only
    // grows. To decide promotion safety, compare it against the leader's
    // CURRENT journal sequence (Journal::getSequence), which is renumbered by
    // the same checkpoint and therefore stays comparable.
    uint64_t appliedCount() const {
        return appliedCount_.load(std::memory_order_acquire);
    }

    // Whole journal records pulled off the disk since construction. Polling an
    // unchanging file costs a small constant here (the two identity anchors),
    // not one read per record in the file — that is the difference between a
    // tail read and re-reading the whole journal every millisecond. Exposed so
    // a test can assert the bound deterministically instead of timing it.
    uint64_t recordsReadFromDisk() const {
        return recordsRead_.load(std::memory_order_acquire);
    }

    // Single-shot catch-up. Reads any new entries from disk and applies
    // them, then returns. Useful in deterministic tests to avoid sleeps.
    void poll() { tickOnce(); }

    // Promote the follower's book(s) into a writable leader.
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
    //   * Touch books this follower never resolved. See the resolver ctor.
    void promote() {
        stop();
        tickOnce();  // drain any tail the writer left after our last poll
        for (OrderBook* book : books_) {
            book->setReplayMode(false);
        }
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

    // ONE fopen per poll, reading only what is new.
    //
    // This used to be Journal(path_).readAll(), which re-read and re-CRCed the
    // ENTIRE file every 1ms tick — and worse than it looks: constructing a
    // Journal to read also opens the file "ab+" and runs
    // recoverSequenceFromDisk(), so each poll walked the journal TWICE. With
    // checkpoints firing at 250k entries that is tens of MB of re-read per
    // millisecond in steady state. readTailFromPath seeks to the cursor
    // instead, so a poll on an unchanging file reads two records regardless of
    // the file's size (recordsReadFromDisk() is the receipt).
    //
    // The identity anchor and the tail come from the SAME open, which is the
    // whole reason this is a Journal method and not a stat plus a read here: a
    // rename(2) landing between two separate opens would pair one file's
    // record 1 with another file's contents, and classify() would answer about
    // a file we did not read. See Journal::TailRead.
    //
    // Torn trailing record: there is NO advisory lock (no flock/lockf)
    // between the leader's writer and this reader — we tolerate, rather
    // than exclude, a concurrent in-flight append. That is safe because
    // readTailFromPath() reads whole fixed-size records via
    // fread(sizeof(JournalEntry)) and rejects any record that is not
    // intact:
    //   * A short final record (fewer than sizeof(JournalEntry) bytes on
    //     disk yet) makes fread return 0, so the loop stops WITHOUT
    //     counting it — the partial tail is dropped.
    //   * A full-length but torn/garbled final record fails the per-entry
    //     CRC32 (or the contiguous-sequence check) and is likewise dropped
    //     rather than applied.
    // So the tail never includes a partial trailing record; on a later poll,
    // once the writer has committed it in full, it appears and is applied then.
    // appliedCount_ only ever advances over fully-intact, CRC-valid,
    // contiguously-sequenced records.
    void tickOnce() {
        size_t applied =
            static_cast<size_t>(appliedCount_.load(std::memory_order_relaxed));
        Journal::TailRead read;
        readFrom(applied, read);

        switch (classify(read, applied)) {
        case Continuity::Inconclusive:
            // This read tells us nothing trustworthy. Change NOTHING and try
            // again next poll — see classify() for the cases that land here.
            return;
        case Continuity::Replaced:
            // See rebuild() for why the whole book goes, not just the delta.
            // The book is now empty, so the replacement has to be taken from
            // its record 1 — a second read, once per checkpoint, self-contained
            // in the same way: its own record 1 is the anchor we adopt below.
            rebuild();
            applied = 0;
            readFrom(0, read);
            break;
        case Continuity::Appended:
            break;
        }
        for (const JournalEntry& entry : read.tail) {
            applyEntry(entry);
        }
        if (read.hasFirst) firstOnDisk_ = read.first;
        // Anchored on the last record CONSUMED, applied or skipped — it marks
        // the position, and the position counts skipped records too.
        if (!read.tail.empty()) lastConsumed_ = read.tail.back();
        appliedCount_.store(applied + read.tail.size(),
                            std::memory_order_release);
        // When the re-read above came back empty the anchors are stale, but
        // applied is 0 and classify() consults neither at 0.
    }

    void readFrom(size_t startIndex, Journal::TailRead& out) {
        Journal::readTailFromPath(path_, startIndex, /*validateCRC=*/true,
                                  /*validateSequence=*/true, out);
        recordsRead_.fetch_add(out.recordsRead, std::memory_order_relaxed);
    }

    enum class Continuity {
        Appended,      // same file, grown (or unchanged) — apply the tail
        Replaced,      // a different file is at this path — rebuild from it
        Inconclusive   // this read proves nothing — do not act on it
    };

    // Is `read` the same log we have been following, grown by appends — or a
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
    // the read to observe the same file — but the read opens by path
    // internally, so a rename landing between our stat and its open would be
    // invisible. Byte identity has no such window, PROVIDED the anchors and the
    // tail come off the same open: that is exactly what Journal::TailRead is
    // for, and why the incremental read is one fopen rather than "fetch record 1,
    // then fetch the tail" — two opens would rebuild the window st_ino was
    // rejected for. What we check IS what we read. It also catches an in-place
    // rewrite (truncate + rewrite keeps the inode, which an st_ino check would
    // wave through).
    //
    // Two anchors, both from that one read:
    //   * read.first — record 1 — decides WHICH FILE this is. Any rewrite writes
    //     record 1 from scratch with a fresh now() timestamp, so a replacement
    //     cannot reproduce the old record 1 and an append cannot alter it.
    //   * read.beforeTail — the record at applied-1 — proves the prefix we
    //     already consumed has not shifted underneath us, and that the file
    //     still HAS that many records.
    // A journal is append-only between rewrites and records are immutable once
    // written, so for a genuine append both anchors match by construction.
    //
    // WHY SHRINKAGE IS NOT THE SIGNAL, AND WHY THERE IS A THIRD ANSWER. Four
    // cases rule out "the record count went down" as a replacement test, and
    // three of them are the reason Inconclusive exists rather than defaulting
    // to one of the other two:
    //   1. A CRC failure truncates the READ, not the file: the read stops at
    //      the bad record and returns a short view forever. Treating that as a
    //      replacement would throw away a good book and rebuild it from a
    //      corrupt prefix. Record 1 still matches, so we answer Inconclusive
    //      and hold position — the same conservative posture the journal takes
    //      toward a corrupt record. Corruption at or after the cursor simply
    //      ends the tail, so there is nothing new to apply and the position
    //      holds by itself; corruption AT applied-1 lands here, as hasBeforeTail
    //      false. (Corruption strictly inside the consumed prefix is no longer
    //      re-read at all — see Journal::readTailFromPath on what that gives
    //      up. It was never recoverable: those records are already applied.)
    //   2. An empty read carries no information about the file we were
    //      following, so it is never grounds to rebuild — the next poll reads a
    //      settled file. It used to be produced spuriously, by
    //      readEntriesFromPath fopen()ing the file and headerBytesOf() fopen()ing
    //      it AGAIN, so a rename between the two paired the old inode's bytes
    //      with the new file's header offset and misframed every record; both
    //      read paths now probe the header on the handle they read from, which
    //      closes that. An empty read still happens for real — a header-only
    //      file, a truncate() mid-rewrite, a path that does not exist yet — and
    //      still proves nothing.
    //   3. A checkpoint of a book with N >= applied replaces the file WITHOUT
    //      shrinking it. Count says "grew"; record 1 says "different file".
    //      The anchor is what decides, so this is caught.
    //   4. Journal::truncate() rewrites in place and KEEPS the inode, which is
    //      why this is a content check and not an st_ino check.
    Continuity classify(const Journal::TailRead& read, size_t applied) const {
        if (applied == 0) return Continuity::Appended;  // nothing to contradict
        if (!read.hasFirst) return Continuity::Inconclusive;         // case 2
        if (!sameRecord(read.first, firstOnDisk_)) {
            return Continuity::Replaced;                             // case 3/4
        }
        // Same file. It cannot legitimately have lost records or rewritten one
        // we already consumed, so anything of that shape is corruption, not
        // progress: hold position rather than act on it.
        if (!read.hasBeforeTail) return Continuity::Inconclusive;    // case 1
        if (!sameRecord(read.beforeTail, lastConsumed_)) {
            return Continuity::Inconclusive;
        }
        return Continuity::Appended;
    }

    // JournalEntry is #pragma pack(1) POD — no padding, so memcmp is an exact
    // record compare.
    static bool sameRecord(const JournalEntry& a, const JournalEntry& b) {
        return std::memcmp(&a, &b, sizeof(JournalEntry)) == 0;
    }

    // Drop every resting order from the books THIS FOLLOWER HOSTS, so the
    // replacement file can be applied from record 1 onto empty books.
    //
    // Only books_ — the books the resolver has actually handed us — are
    // emptied. A book belonging to another follower (or to nothing at all) is
    // not ours to clear, and a hosted symbol we have never seen a record for
    // holds nothing we put there.
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
        for (OrderBook* book : books_) {
            std::vector<OrderId> ids;
            // Collect first, cancel second: forEachOrderLocked holds bookLock_
            // and cancelOrder takes it — the mutex is not recursive.
            book->forEachOrderLocked(
                [&ids](const Order& o) { ids.push_back(o.id); });
            for (OrderId id : ids) {
                book->cancelOrder(id);
            }
        }
    }

    // The book hosting `symbol`, or nullptr when this follower does not host it.
    //
    // Every book the resolver names is remembered, because rebuild() and
    // promote() act on the set of books we actually touch and there is no other
    // way to enumerate a std::function's domain. Remembering it is also where a
    // resolver-constructed book enters replay mode.
    //
    // ponytail: a linear scan over books_. That is one entry for a single-symbol
    // follower and a handful for a multi-symbol one; a map earns its keep past a
    // few dozen hosted symbols, on a path that already does file I/O per poll.
    OrderBook* bookFor(SymbolId symbol) {
        OrderBook* book = resolver_ ? resolver_(symbol) : nullptr;
        if (!book) return nullptr;
        if (std::find(books_.begin(), books_.end(), book) == books_.end()) {
            books_.push_back(book);
            book->setReplayMode(true);
        }
        return book;
    }

    void applyEntry(const JournalEntry& e) {
        // Route, do not just apply. An entry for a symbol we do not host is
        // skipped — not an error, and NOT a reason to hold the cursor back:
        // tickOnce() has already counted it as consumed.
        OrderBook* book = bookFor(e.symbolId);
        if (!book) return;
        switch (e.entryType) {
        case JournalEntry::Type::AddOrder:
            book->addOrder(e.orderId, e.participantId, e.side, e.price,
                             e.quantity, e.orderType, e.stopPrice,
                             e.displayQty, e.timeInForce, e.expiryTime,
                             e.stopLimitPrice, e.pegType, e.pegOffset,
                             e.trailAmount, e.minQty, e.hidden);
            break;
        case JournalEntry::Type::CancelOrder:
            book->cancelOrder(e.orderId);
            break;
        case JournalEntry::Type::ModifyOrder:
            book->modifyOrder(e.orderId, e.newQty);
            break;
        case JournalEntry::Type::CancelReplace:
            book->cancelReplace(e.orderId, e.newPrice, e.newQty);
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
            if (!book->getOrder(e.orderId)) {
                book->addOrder(e.orderId, e.participantId, e.side, e.price,
                                 e.quantity, e.orderType, e.stopPrice,
                                 e.displayQty, e.timeInForce, e.expiryTime,
                                 e.stopLimitPrice, e.pegType, e.pegOffset,
                                 e.trailAmount, e.minQty, e.hidden);
            }
            break;
        }
    }

    std::string         path_;
    // symbolId -> the book hosting it, or nullptr. The single-symbol ctor is
    // just a resolver that answers one symbol.
    std::function<OrderBook*(SymbolId)> resolver_;
    uint32_t            pollIntervalMs_;
    std::atomic<bool>   running_{false};
    bool                promoted_{false};
    std::thread         thread_;
    std::atomic<uint64_t> appliedCount_{0};
    std::atomic<uint64_t> recordsRead_{0};
    // Every distinct book the resolver has handed us — the books this follower
    // hosts and is therefore allowed to clear (rebuild) and promote. Grown by
    // bookFor() on the polling thread only.
    std::vector<OrderBook*> books_;
    // Stream-identity anchors for the file we are currently following. Touched
    // only by the polling thread (loop(), poll(), promote() — never
    // concurrently, since promote() joins first). See classify().
    JournalEntry        firstOnDisk_{};
    JournalEntry        lastConsumed_{};
};

}  // namespace OrderMatcher
