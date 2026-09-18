#pragma once

#include "Types.h"
#include "FaultInjector.h"
#include "Metrics.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include <unistd.h>
#ifdef __APPLE__
#include <fcntl.h>
#endif
#ifdef __linux__
#include <sys/stat.h>
#endif

// Optional io_uring-backed journal write path (Linux only). Enabled when the
// build defines OB_HAVE_LIBURING (set by CMake when liburing is found). When
// absent — macOS, or Linux without liburing — the journal uses the classic
// fwrite + fdatasync path below, with identical durability semantics.
#if defined(__linux__) && defined(OB_HAVE_LIBURING)
#include <liburing.h>
#include <array>
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

// Hardware CRC32-C intrinsics
#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#elif defined(__SSE4_2__)
#include <nmmintrin.h>
#endif

namespace OrderMatcher {

inline uint32_t computeCRC32(const void* data, size_t length) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFF;

#if defined(__ARM_FEATURE_CRC32)
    while (length >= 8) {
        uint64_t val;
        __builtin_memcpy(&val, bytes, 8);
        crc = __crc32cd(crc, val);
        bytes += 8;
        length -= 8;
    }
    while (length-- > 0) {
        crc = __crc32cb(crc, *bytes++);
    }
#elif defined(__SSE4_2__)
    while (length >= 8) {
        uint64_t val;
        __builtin_memcpy(&val, bytes, 8);
        crc = static_cast<uint32_t>(_mm_crc32_u64(crc, val));
        bytes += 8;
        length -= 8;
    }
    while (length-- > 0) {
        crc = _mm_crc32_u8(crc, *bytes++);
    }
#else
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ (0x82F63B78 & (-(crc & 1)));
        }
    }
#endif

    return ~crc;
}

#pragma pack(push, 1)
// On-disk file header, written once at offset 0.
//
// Before this, the file was a bare array of packed structs: no magic, no
// length prefix, no version. Framing was implicit in sizeof(JournalEntry), so
// a layout change sliced an existing file on the wrong boundaries, failed
// every CRC, and read back as empty — indistinguishable from "nothing was
// ever written here".
//
// LEGACY DISCRIMINATION. A pre-header file begins with a JournalEntry, whose
// first byte is entryType — always 1..5. `magic` begins with 'O' (0x4F), which
// no entryType can be, so "starts with the magic" separates a headered file
// from a pre-header one with certainty rather than by probability. Files
// written before this header are still read, as bare records.
#pragma pack(push, 1)
struct JournalFileHeader {
    char     magic[8];       // "OBJRNL" + NUL padding
    uint32_t formatVersion;  // bump when the file or record layout changes
    uint32_t recordSize;     // sizeof(JournalEntry) as the writer saw it
    uint64_t reserved;       // zero
};
#pragma pack(pop)

inline constexpr char     JOURNAL_MAGIC[8]    = {'O','B','J','R','N','L','\0','\0'};
inline constexpr uint32_t JOURNAL_FORMAT_V1   = 1;
static_assert(sizeof(JournalFileHeader) == 24,
              "JournalFileHeader is on disk; changing its size needs a format "
              "version bump and a migration path");

struct JournalEntry {
    enum class Type : uint8_t {
        AddOrder = 1,
        CancelOrder = 2,
        ModifyOrder = 3,
        CancelReplace = 4,
        Snapshot = 5
    };

    Type entryType;
    uint64_t sequenceNumber;
    uint64_t timestamp;
    OrderId orderId;
    ParticipantId participantId;
    SymbolId symbolId;
    Side side;
    Price price;
    Quantity quantity;
    OrderType orderType;
    TimeInForce timeInForce;
    uint64_t expiryTime;
    Price stopPrice;
    Price stopLimitPrice;
    Quantity displayQty;
    PegType pegType;
    Price pegOffset;
    Price trailAmount;
    Quantity minQty;
    bool hidden;
    Price newPrice;
    Quantity newQty;
    uint32_t checksum;
};
#pragma pack(pop)

class Journal {
public:
    enum class SyncPolicy : uint8_t {
        Immediate,
        GroupCommit
    };

    explicit Journal(const std::string& filePath,
                     SyncPolicy syncPolicy = SyncPolicy::GroupCommit,
                     size_t batchSize = 64)
        : filePath_(filePath), syncPolicy_(syncPolicy),
          batchSize_(batchSize == 0 ? 1 : batchSize) {
        // NOTE: the stale-".tmp" cleanup deliberately does NOT live here.
        // Constructing a Journal must not modify anything on disk, because
        // several consumers construct one purely to READ — JournalFollower
        // does it on every poll (1ms by default), and ResearchHarness and the
        // CLI tools do it once. Removing the sibling here meant a follower
        // tailing a live leader DELETED the leader's in-progress checkpoint:
        // the leader went on writing to an unlinked inode, its rename failed
        // with ENOENT, and the checkpoint silently never happened.
        // The cleanup now runs in prepareRewrite(), which is the only place
        // that is about to create a .tmp and therefore the only place that
        // has any business removing a stale one.

        open("ab+");
        sequence_ = recoverSequenceFromDisk();
        checkRecoverable();

#if defined(__linux__) && defined(OB_HAVE_LIBURING)
        // io_uring_queue_init(entries, ring, flags) returns 0 on success or
        // -errno on failure (e.g. kernel < 5.1, or blocked by a seccomp /
        // container policy). On any failure we leave useIoUring_ = false and
        // the journal transparently uses the classic fwrite + fdatasync path —
        // behaviour-identical, just without the ring. (io_uring_queue_init(3))
        useIoUring_ = (io_uring_queue_init(kRingDepth, &ring_, 0) == 0);
        if (useIoUring_) {
            // Pre-size each inflight slot's copy buffer so the hot commit path
            // never allocates (Hazard A). Then start the completion reaper that
            // fires onCommit_ once the write→fdatasync chain is durable.
            for (auto& slot : inflight_) {
                slot.buf.reserve(batchSize_);
            }
            reaper_ = std::thread([this] { reaperLoop(); });
        }
#endif
    }

    ~Journal() {
        flush();  // commits + drains the final batch; reaper fires its onCommit
#if defined(__linux__) && defined(OB_HAVE_LIBURING)
        // After flush() the ring is idle (inflightCount_ == 0). Stop and join
        // the reaper BEFORE closing the fd / tearing the ring down, so it can
        // never touch a closed fd or a freed ring. (io_uring_queue_exit(3))
        stopReaper();
#endif
        close();
#if defined(__linux__) && defined(OB_HAVE_LIBURING)
        if (useIoUring_) {
            io_uring_queue_exit(&ring_);
            useIoUring_ = false;
        }
#endif
    }

    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;

    void logAddOrder(OrderId id, ParticipantId pid, SymbolId sym, Side side, Price price,
                     Quantity qty, OrderType type, TimeInForce tif = TimeInForce::GTC,
                     uint64_t expiry = 0, Price stopPrice = 0, Price stopLimitPrice = 0,
                     Quantity displayQty = 0, PegType pegType = PegType::None,
                     Price pegOffset = 0, Price trailAmount = 0, Quantity minQty = 0,
                     bool hidden = false) {
        JournalEntry entry{};
        entry.entryType = JournalEntry::Type::AddOrder;
        entry.timestamp = now();
        entry.orderId = id;
        entry.participantId = pid;
        entry.symbolId = sym;
        entry.side = side;
        entry.price = price;
        entry.quantity = qty;
        entry.orderType = type;
        entry.timeInForce = tif;
        entry.expiryTime = expiry;
        entry.stopPrice = stopPrice;
        entry.stopLimitPrice = stopLimitPrice;
        entry.displayQty = displayQty;
        entry.pegType = pegType;
        entry.pegOffset = pegOffset;
        entry.trailAmount = trailAmount;
        entry.minQty = minQty;
        entry.hidden = hidden;
        appendEntry(entry);
    }

    // symbolId is REQUIRED, not defaulted.
    //
    // These three records used to carry no symbol at all — the field existed
    // and stayed zero-filled — so replay could only find the order by scanning
    // every book for its id. That is correct exactly while order ids are
    // globally unique, and nothing enforces that: the duplicate check is
    // per-book (OrderBook::addOrder). Two participants on different symbols
    // using overlapping id ranges is ordinary, and the scan then cancels
    // whichever book it reaches first — silently losing a resting order on
    // recovery. Reproduced in JournalCrashTest::testDuplicateIdAcrossSymbols.
    //
    // A default of 0 would hide exactly the omissions that caused this, since
    // 0 is a legal SymbolId. Required means the compiler names every site.
    void logCancelOrder(OrderId id, SymbolId symbolId) {
        JournalEntry entry{};
        entry.entryType = JournalEntry::Type::CancelOrder;
        entry.timestamp = now();
        entry.orderId = id;
        entry.symbolId = symbolId;
        appendEntry(entry);
    }

    void logModifyOrder(OrderId id, SymbolId symbolId, Quantity newQty) {
        JournalEntry entry{};
        entry.entryType = JournalEntry::Type::ModifyOrder;
        entry.timestamp = now();
        entry.orderId = id;
        entry.symbolId = symbolId;
        entry.newQty = newQty;
        appendEntry(entry);
    }

    void logCancelReplace(OrderId id, SymbolId symbolId, Price newPrice,
                          Quantity newQty) {
        JournalEntry entry{};
        entry.entryType = JournalEntry::Type::CancelReplace;
        entry.timestamp = now();
        entry.orderId = id;
        entry.symbolId = symbolId;
        entry.newPrice = newPrice;
        entry.newQty = newQty;
        appendEntry(entry);
    }

    void logSnapshot(OrderId id, ParticipantId pid, SymbolId sym, Side side, Price price,
                     Quantity remainingQty, OrderType type, TimeInForce tif = TimeInForce::GTC,
                     uint64_t expiry = 0, Price stopPrice = 0, Price stopLimitPrice = 0,
                     Quantity displayQty = 0, PegType pegType = PegType::None,
                     Price pegOffset = 0, Price trailAmount = 0, Quantity minQty = 0,
                     bool hidden = false) {
        JournalEntry entry{};
        entry.entryType = JournalEntry::Type::Snapshot;
        entry.timestamp = now();
        entry.orderId = id;
        entry.participantId = pid;
        entry.symbolId = sym;
        entry.side = side;
        entry.price = price;
        entry.quantity = remainingQty;
        entry.orderType = type;
        entry.timeInForce = tif;
        entry.expiryTime = expiry;
        entry.stopPrice = stopPrice;
        entry.stopLimitPrice = stopLimitPrice;
        entry.displayQty = displayQty;
        entry.pegType = pegType;
        entry.pegOffset = pegOffset;
        entry.trailAmount = trailAmount;
        entry.minQty = minQty;
        entry.hidden = hidden;
        appendEntry(entry);
    }

    std::vector<JournalEntry> readAll(bool validateCRC = true,
                                      bool validateSequence = false) {
        flush();
        return readEntriesFromPath(filePath_, validateCRC, validateSequence);
    }

    void flush() {
        if (!file_) {
            return;
        }

        if (!batch_.empty()) {
            // Drain, do not commit once. commitBatch() writes a PREFIX and
            // deliberately keeps an unwritten suffix in batch_ for the next
            // commit when the write comes up short (a real short write, or an
            // injected torn write). One call therefore does not mean "the
            // batch is on disk", and every caller of flush() — readAll(),
            // truncate(), rewriteAtomically(), the destructor — assumes it
            // does. With SyncPolicy::Immediate and batch size 1 the gap could
            // not bite, because a single-entry batch either wrote or did not.
            // With a real batch it silently loses the suffix.
            //
            // Stop on no progress rather than spinning: if a commit writes
            // nothing at all, retrying cannot help, and the caller checks
            // pendingEntries() to find out.
            while (!batch_.empty()) {
                const size_t before = batch_.size();
                commitBatch();
                if (batch_.size() >= before) break;
            }
        } else {
            std::fflush(file_);
        }
        // Barrier: on the async io_uring path commitBatch() only *submits* the
        // durability chain — block here until the reaper has reaped it and
        // fired onCommit. On the synchronous path (macOS, or Linux without
        // liburing) this is a no-op: commits are already durable-and-acked
        // inline. Guarantees read-after-flush consistency for readAll(),
        // truncate(), rewriteAtomically(), and the destructor.
        drainCompletions();
    }

    // Block until every in-flight durability chain has been reaped and its
    // onCommit callback has fired. On the synchronous commit path this is a
    // no-op. Callers that submitted orders asynchronously use this to
    // establish a happens-before edge before observing onCommit side effects.
    void quiesce() { drainCompletions(); }

    void truncate() {
        flush();
        close();
        open("wb+");
        sequence_ = 0;
        persistedEntries_.store(0, std::memory_order_relaxed);
    }

    // Split into prepare/commit so a caller can build the replacement OUTSIDE
    // the lock that guards appends. prepareRewrite() writes only <path>.tmp and
    // never touches this journal, so it is safe to run unlocked; commitRewrite()
    // does the flush/close/rename/open and must be called under that lock.
    //
    // The caller is responsible for the part this class cannot see: if anything
    // was appended between preparing and committing, the swap would discard it,
    // because the snapshot was built from a book state that predates it. See
    // MatchingEngine::checkpointInternal.
    bool rewriteAtomically(const std::function<void(Journal&)>& writer) {
        if (!prepareRewrite(writer)) return false;
        return commitRewrite();
    }

    // Build the replacement into <path>.tmp. Touches nothing owned by this
    // journal; safe to call without the caller's append lock held.
    bool prepareRewrite(const std::function<void(Journal&)>& writer) {
        const std::string tmpPath = filePath_ + ".tmp";
        // A previous rewrite that crashed between close() and rename() can
        // leave a stale sibling. Clear it here — the one place that is about
        // to write a new one — rather than in the constructor, where it made
        // every read-only consumer destructive.
        std::remove(tmpPath.c_str());
        bool snapshotComplete = false;
        {
            // GroupCommit, NOT Immediate-with-batch-1.
            //
            // The snapshot used to fsync once per entry, which is pure cost
            // for no guarantee: atomicity here comes from rename(2), and a
            // crash before that rename discards the temp file wholesale — the
            // original is untouched and a stale ".tmp" is removed on the next
            // open. Per-entry durability inside a file that only becomes real
            // at the rename protects nothing.
            //
            // What IS required is that the temp file's bytes be durable BEFORE
            // the rename, or a crash just after it could expose an entry whose
            // data never reached the platter. The temp.flush() below is that
            // barrier, and it is sufficient on its own.
            //
            // This mattered when checkpointInternal held journalMutex_ across
            // the whole call, blocking every worker's append for its duration.
            // Measured on a 20,000-order book: 53.8s before, 0.02s after. The
            // build now runs outside that lock entirely (prepareRewrite), so
            // what remains under it is the flush/close/rename/open.
            //
            // The batch bounds memory rather than durability: entries are held
            // until it fills, so a very large snapshot still commits in pieces
            // instead of buffering the whole book.
            constexpr size_t kSnapshotBatch = 4096;
            Journal temp(tmpPath, SyncPolicy::GroupCommit, kSnapshotBatch);
            temp.truncate();
            writer(temp);
            temp.flush();
            // The snapshot is about to REPLACE the live journal, so publishing
            // a partial one loses resting orders permanently — worse than not
            // checkpointing at all. If flush() could not drain every entry
            // (a short or torn write), abandon the rewrite and leave the
            // original in place; the caller sees false and the pre-call state,
            // which is exactly the atomicity contract.
            snapshotComplete = (temp.pendingEntries() == 0);
        }
        if (!snapshotComplete) {
            std::remove(tmpPath.c_str());
            return false;
        }
        return true;
    }

    // Swap a prepared <path>.tmp over the live journal. MUST be called with the
    // caller's append lock held: it closes and reopens the file handle.
    bool commitRewrite() {
        const std::string tmpPath = filePath_ + ".tmp";

        // Commit whatever is still batched before the handle closes. Anything
        // appended since prepareRewrite() is about to be discarded by the
        // rename — that is the caller's invariant to enforce, not this one's.
        flush();
        close();

        // Fault injection: simulate rename(2) failure (e.g., target on a
        // different filesystem, ENOSPC, EPERM). The original file is
        // untouched and still readable; rewriteAtomically must return
        // false. The atomicity contract — caller sees either the
        // pre-call state or the post-call state, never a partial — is
        // exactly what this test exercises.
        bool renameOk;
        if (FaultInjector::instance().shouldFail(
                "journal.checkpoint.rename_fail")) {
            std::remove(tmpPath.c_str());  // clean up the would-be tmp
            renameOk = false;
        } else {
            renameOk = (::rename(tmpPath.c_str(), filePath_.c_str()) == 0);
        }
        if (!renameOk) {
            open("ab+");
            sequence_ = recoverSequenceFromDisk();
            return false;
        }

        open("ab+");
        sequence_ = recoverSequenceFromDisk();
        persistedEntries_.store(static_cast<size_t>(sequence_),
                                std::memory_order_relaxed);
        return true;
    }

    bool needsCheckpoint(size_t maxEntries, size_t maxBytes) const {
        return persistedEntries_.load(std::memory_order_relaxed) >= maxEntries ||
               bytesOnDisk() >= maxBytes;
    }

    void setMaxSizeMb(size_t mb) { maxSizeMb_ = mb; }
    size_t maxSizeMb() const { return maxSizeMb_; }

    uint64_t getSequence() const { return sequence_; }

    // True when the journal opened a file it could not read at all and is
    // therefore refusing to append. See checkRecoverable().
    bool recoveryFailed() const { return recoveryFailed_; }

    // Returns the number of header bytes at the front of `path`: the header
    // size when one is present, 0 for a pre-header file or one too short to
    // hold a header. Static so the read paths can use it without an instance.
    static size_t headerBytesOf(const std::string& path) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return 0;
        JournalFileHeader h{};
        const bool got = std::fread(&h, sizeof(h), 1, f) == 1;
        std::fclose(f);
        if (!got) return 0;
        return std::memcmp(h.magic, JOURNAL_MAGIC, sizeof(h.magic)) == 0
                   ? sizeof(JournalFileHeader) : 0;
    }

    const std::string& path() const { return filePath_; }

    // Callback fired once per batch immediately after a successful
    // commit (entries are fsync-durable on disk). Used to feed
    // committed entries into the replication coordinator without
    // intruding on the matching-engine hot path. Receives a pointer
    // to the prefix of committed entries and the count.
    //
    // Lifetime: the entries pointer is only valid for the duration
    // of the callback. Copy the bytes if you need them longer.
    using OnCommitFn = std::function<void(const JournalEntry* entries, size_t count)>;
    void setOnCommit(OnCommitFn fn) { onCommit_ = std::move(fn); }
    OnCommitFn onCommit() const { return onCommit_; }

    // Second post-barrier hook, reserved for the owning MatchingEngine, fired
    // under exactly the same durability guarantee as onCommit_. Separate slot
    // rather than chaining onto onCommit_ because that one belongs to the
    // application (main.cpp installs the replication ack there): whichever set
    // it last would silently disable the other, and one of the two would be a
    // durability guarantee that stopped holding without any sign.
    // `n` is the number of entries that just became durable.
    void setOnDurable(std::function<void(size_t)> fn) { onDurable_ = std::move(fn); }

    // Total entries handed to appendEntry() since construction. Pair with the
    // count reported to onCommit_ to learn which appends are durable.
    uint64_t entriesAppended() const { return entriesAppended_; }

    // Durability configuration, so a caller (or a test) can assert what a
    // journal it did not construct will actually do per append.
    SyncPolicy syncPolicy() const { return syncPolicy_; }
    size_t     batchSize()  const { return batchSize_; }

    // Entries appended but not yet on disk. Non-zero after flush() means the
    // write could not make progress; the caller must not treat the file as
    // complete.
    size_t pendingEntries() const { return batch_.size(); }
    // Tracked, not asked for.
    //
    // This used to fstat(2) on Linux and ftell() elsewhere. needsCheckpoint()
    // calls it on EVERY append — the entry-count disjunct in front of it is
    // false 249,999 times in 250,000 at the default threshold, so it never
    // short-circuits — and MatchingEngine::maybeTriggerAutoCheckpoint() makes
    // that a second journalMutex_ acquisition per journaled order. On Linux
    // that is a syscall per order inside the lock whose result is discarded
    // almost every time.
    //
    // Measured on x86 CI (AMD EPYC, ext4, fdatasync ~438us) with the disk
    // amortised away so the serialised region is visible: 980k appends/s with
    // the fstat, 4.88M/s without. It is invisible at the shipped batch of 64
    // because the fsync dominates there — which is exactly why it survived,
    // and why it is the first thing that binds on faster storage or a larger
    // batch. On macOS the same arm measured no difference at all, because
    // ftell() is userspace; a measurement taken only there would have called
    // this a non-issue.
    //
    // Counting written entries is also strictly more correct than ftell() was:
    // io_uring writes bypass the stdio stream, which is the reason Linux
    // needed the syscall in the first place. writeBatch() reports whole
    // entries landed regardless of mechanism.
    size_t bytesOnDisk() const {
        return bytesOnDisk_.load(std::memory_order_relaxed);
    }

protected:
    // Push a not-yet-numbered, not-yet-CRC'd entry into the batch. Both the
    // sequenceNumber and the checksum are assigned at commit time (see
    // commitBatch) so that:
    //   * On-disk sequences are contiguous even when an fwrite is short —
    //     unwritten entries stay in batch_ and are renumbered on retry,
    //     never leaving a gap in the persistent log.
    //   * A process crash between log time and commit time loses entries
    //     that were never assigned a sequence, so no "phantom" sequence
    //     numbers are leaked into a recovery view of the world.
    void appendEntry(JournalEntry& entry) {
        if (!file_) {
            return;
        }
        // Refuse to extend a file we could not read. See checkRecoverable():
        // appending here is what turns an unreadable journal into a
        // permanently unreadable one.
        if (recoveryFailed_) {
            return;
        }
        batch_.push_back(entry);
        // Append ordinal, distinct from sequenceNumber: sequences are assigned
        // at COMMIT time so a crash leaks no phantom numbers, which makes them
        // useless for identifying an entry that has only been appended. This
        // counts appends, and entries commit strictly FIFO, so comparing it
        // against the running total of committed entries tells a caller
        // exactly which appends are now durable. See DurabilityGate.
        ++entriesAppended_;

        if (syncPolicy_ == SyncPolicy::Immediate || batch_.size() >= batchSize_) {
            commitBatch();
        }
    }

    // Persistence contract:
    //   1. Decide how many entries to attempt this commit (`toWrite`).
    //   2. Assign sequenceNumber and CRC to that prefix only — these are the
    //      entries we are about to commit to disk.
    //   3. fwrite. If the underlying write returns short (real OS short
    //      write or torn-write fault), rewind sequence_ by the unwritten
    //      count and keep the unwritten suffix in batch_ for the next
    //      commit. The suffix's sequenceNumber and CRC are reset; they will
    //      be re-assigned on the next attempt.
    //   4. fflush + syncFile (or skip syncFile if fsync_fail fault armed).
    //
    // This guarantees on-disk sequences are contiguous: a torn write loses
    // entries entirely (they stay in memory or are dropped on crash), but
    // never produces a gap in the persistent sequence stream. Strict
    // recovery (validateSequence=true) therefore returns the entire
    // CRC-valid prefix instead of stopping at a gap created by a partial
    // batch.
    void commitBatch() {
        if (!file_) {
            return;
        }
#if defined(__linux__) && defined(OB_HAVE_LIBURING)
        if (useIoUring_) {
            commitBatchAsync();  // drains requeue_ internally, then submits
            return;
        }
#endif
        if (batch_.empty()) {
            return;
        }
        commitBatchSync();
    }

    // Synchronous commit path: fwrite → fflush → fdatasync, then fire onCommit
    // inline once the sync is durable. Used on macOS, on Linux without liburing
    // (or when the ring failed to init), and as the io_uring SQE-exhaustion
    // fallback. This is the original commitBatch body, behaviour-unchanged.
    void commitBatchSync() {
        if (!file_ || batch_.empty()) {
            return;
        }

        auto& fi = FaultInjector::instance();
        size_t toWrite = batch_.size();

        // Fault: simulate a short write at the OS layer — only the prefix
        // makes it to the buffer cache; the rest is "lost" from this
        // attempt and stays in batch_ for retry.
        bool tornWrite = fi.shouldFail("journal.commit.short_write");
        if (tornWrite) {
            toWrite = batch_.size() / 2;
        }

        if (toWrite == 0) {
            return;  // nothing to do this round
        }

        // Assign sequence numbers + CRCs to the prefix we're about to write.
        //
        // offsetof on the pragma-packed JournalEntry is conditionally
        // supported in C++ (the struct contains scoped enums, so it
        // technically isn't standard-layout under strict GCC). Clang
        // and libstdc++-GCC both accept it; if a future toolchain
        // emits -Winvalid-offsetof, suppress at the build level
        // rather than rewriting the CRC range.
        for (size_t i = 0; i < toWrite; ++i) {
            batch_[i].sequenceNumber = ++sequence_;
            batch_[i].checksum = computeCRC32(&batch_[i],
                                              offsetof(JournalEntry, checksum));
        }

        // Fault: bit-flip one byte in one entry of the prefix after CRC
        // assignment. Exercises CRC validation on recovery.
        if (fi.shouldFail("journal.commit.bit_flip")) {
            uint64_t r = fi.nextU64();
            size_t entryIdx = r % toWrite;
            size_t byteOff  = (r >> 16) % sizeof(JournalEntry);
            uint8_t bit     = uint8_t(1) << ((r >> 32) & 7);
            auto* bytes = reinterpret_cast<uint8_t*>(&batch_[entryIdx]);
            bytes[byteOff] ^= bit;
        }

        // Write the toWrite-entry prefix to the page cache. On Linux with
        // liburing this is an io_uring write; elsewhere it is the classic
        // fwrite + fflush. Returns the number of WHOLE entries that landed —
        // identical contract to the old fwrite return — so the short-write
        // rewind and durability barrier below are mechanism-agnostic.
        size_t actuallyWritten = writeBatch(toWrite);

        // If the OS returned short on the actual fwrite (independent of
        // fault injection), rewind sequence_ for the un-written tail of
        // the prefix so they get fresh numbers on retry.
        if (actuallyWritten < toWrite) {
            sequence_ -= (toWrite - actuallyWritten);
        }
        // Only whole entries land, so this stays exact. Atomic because the
        // io_uring reaper reaches this path off the caller's thread, the same
        // reason persistedEntries_ is atomic.
        bytesOnDisk_.fetch_add(actuallyWritten * sizeof(JournalEntry),
                               std::memory_order_relaxed);

        // Durability barrier. The replication ack (onCommit_, fired below)
        // must happen STRICTLY AFTER a successful durable sync — never before,
        // and never when the sync was skipped or failed. Otherwise the backup
        // would ack entries the primary has not durably written; a crash
        // between flush and fsync then loses them on the primary while the
        // backup keeps them (ack-before-fsync hazard / backup divergence).
        //
        // Fault: skip the durability barrier when armed, modelling an fsync
        // that did not make the data durable. (Data still sits in the buffer
        // cache; on a clean exit it is readable, but it is NOT durable, so we
        // must withhold the ack.)
        bool durable = false;
        if (!fi.shouldFail("journal.commit.fsync_fail")) {
            durable = syncFile();
        }

        persistedEntries_.fetch_add(actuallyWritten, std::memory_order_relaxed);
        // Counter cached at function-static scope — first call locks the
        // registry to allocate; subsequent calls hit the atomic directly.
        static auto& kEntriesCommitted = MetricsRegistry::instance().counter(
            "journal_entries_committed_total",
            "Total journal entries successfully written to disk");
        kEntriesCommitted.increment(actuallyWritten);

        // Fire the commit callback ONLY after a successful durable sync (see
        // the durability barrier above), and BEFORE erasing the prefix so the
        // pointer remains valid for the duration of the callback. The callback
        // runs synchronously on the writer thread — implementers should keep it
        // short (the replication coordinator queues bytes for an async send,
        // which is the intended use).
        //
        // `durable` is false only when the sync was skipped or failed. In that
        // case the bytes are not on stable storage, so they must not be acked
        // to the backup — withholding the ack is what closes the
        // ack-before-fsync hazard.
        if (durable && onCommit_ && actuallyWritten > 0) {
            onCommit_(batch_.data(), actuallyWritten);
        }
        if (durable && onDurable_ && actuallyWritten > 0) {
            onDurable_(actuallyWritten);
        }

        // Pop the persisted prefix. Anything past it stays in batch_ and
        // will be renumbered on the next commit. To keep that retry pure
        // (no stale CRCs/sequences), zero out those fields now.
        batch_.erase(batch_.begin(), batch_.begin() + actuallyWritten);
        for (auto& e : batch_) {
            e.sequenceNumber = 0;
            e.checksum = 0;
        }

        // For a fault-injected torn write we modeled as "this attempt
        // wrote less than asked"; the unwritten suffix is now intact in
        // batch_ for the next commit. Real torn writes (process crash
        // mid-fwrite) lose the in-memory tail; on next process start, the
        // file ends at whatever the OS persisted and recovery picks up
        // from there.
        (void)tornWrite;
    }

#if defined(__linux__) && defined(OB_HAVE_LIBURING)
    // user_data layout: (slotIndex << 1) | isFsync. kStopUserData (all ones)
    // cannot collide — kInflightDepth is 8, so real tokens are <= 15.
    static uint64_t encodeUserData(uint32_t slotIdx, bool isFsync) {
        return (static_cast<uint64_t>(slotIdx) << 1) | (isFsync ? 1ull : 0ull);
    }

    // Async durability commit (Hazards A–D). Assigns sequence + CRC, copies the
    // prefix into a free inflight slot, submits a write→fdatasync linked chain,
    // and returns WITHOUT waiting. The reaper thread reaps both CQEs and fires
    // onCommit only once the chain is durable. Called on the writer thread.
    void commitBatchAsync() {
        // The ring appends at EOF, so the header has to be on disk before the
        // first record goes down this path as well.
        writePendingHeader();
        // ── Hazard C: single outstanding durability chain ──
        // Block until the previous chain is fully reaped. On return the reaper
        // is idle, so the writer alone owns sequence_/batch_/requeue_. Then
        // fold back any real-short-write tail (rewind here keeps sequence_
        // writer-owned — the reaper never writes it).
        {
            std::unique_lock<std::mutex> lk(ringMu_);
            chainDone_.wait(lk, [this] { return inflightCount_ == 0; });
            if (pendingRewind_ > 0) {
                sequence_ -= pendingRewind_;
                pendingRewind_ = 0;
                batch_.insert(batch_.begin(), requeue_.begin(), requeue_.end());
                requeue_.clear();
            }
        }

        if (batch_.empty()) {
            return;
        }

        auto& fi = FaultInjector::instance();
        size_t toWrite = batch_.size();

        // Fault: fault-injected short write — attempt only a prefix; the tail
        // stays in batch_ for the next commit (writer-side, exactly as the sync
        // path). Distinct from a *real* short write (write CQE returns fewer
        // bytes than asked), which the reaper hands back via requeue_.
        bool tornWrite = fi.shouldFail("journal.commit.short_write");
        if (tornWrite) {
            toWrite = batch_.size() / 2;
        }
        if (toWrite == 0) {
            return;
        }

        // Assign sequence numbers + CRCs to the prefix we're about to submit.
        for (size_t i = 0; i < toWrite; ++i) {
            batch_[i].sequenceNumber = ++sequence_;
            batch_[i].checksum = computeCRC32(&batch_[i],
                                              offsetof(JournalEntry, checksum));
        }

        // Fault: bit-flip one byte after CRC assignment (exercises recovery).
        if (fi.shouldFail("journal.commit.bit_flip")) {
            uint64_t r = fi.nextU64();
            size_t entryIdx = r % toWrite;
            size_t byteOff  = (r >> 16) % sizeof(JournalEntry);
            uint8_t bit     = uint8_t(1) << ((r >> 32) & 7);
            auto* bytes = reinterpret_cast<uint8_t*>(&batch_[entryIdx]);
            bytes[byteOff] ^= bit;
        }

        // Fault: fsync_fail — model an fsync that did not make data durable. We
        // still submit the WRITE (data reaches the page cache, readable on a
        // clean exit) but omit the fdatasync link and withhold the ack.
        bool wantAck = !fi.shouldFail("journal.commit.fsync_fail");

        // ── Hazard B: two linked SQEs (write → fdatasync) ──
        struct io_uring_sqe* wsqe = io_uring_get_sqe(&ring_);
        struct io_uring_sqe* fsqe = wantAck ? io_uring_get_sqe(&ring_) : nullptr;
        if (!wsqe || (wantAck && !fsqe)) {
            // SQ ring exhausted — commit this batch synchronously. Safe: no
            // chain is inflight (drained above), so the fwrite append is
            // correctly ordered and does not touch the ring.
            commitBatchSync();
            return;
        }

        uint32_t slotIdx;
        {
            // ── Hazard A: copy the prefix into a pre-allocated inflight slot.
            // Populate the slot under the lock so the reaper (which reads it
            // under the same lock) has a happens-before edge — TSan-clean.
            std::lock_guard<std::mutex> lk(ringMu_);
            slotIdx = nextSlot_;
            nextSlot_ = (nextSlot_ + 1) % kInflightDepth;
            Inflight& slot = inflight_[slotIdx];
            slot.buf.assign(batch_.begin(),
                            batch_.begin() + static_cast<std::ptrdiff_t>(toWrite));
            slot.firstSeq = slot.buf.front().sequenceNumber;
            slot.count = static_cast<uint32_t>(toWrite);
            slot.writeRes = INT32_MIN;
            slot.fsyncRes = INT32_MIN;
            slot.wantAck = wantAck;
            slot.pendingCqes = wantAck ? 2 : 1;
            slot.active = true;

            io_uring_prep_write(
                wsqe, fileno(file_), slot.buf.data(),
                static_cast<unsigned>(toWrite * sizeof(JournalEntry)),
                static_cast<unsigned long long>(-1));  // -1 → append at EOF
            io_uring_sqe_set_data64(wsqe, encodeUserData(slotIdx, /*isFsync=*/false));
            if (wantAck) {
                // IOSQE_IO_LINK: the fdatasync runs iff the write succeeds; a
                // failed write auto-cancels it (fsyncRes == -ECANCELED), so the
                // chain is durable only when BOTH CQEs report success.
                wsqe->flags |= IOSQE_IO_LINK;
                io_uring_prep_fsync(fsqe, fileno(file_), IORING_FSYNC_DATASYNC);
                io_uring_sqe_set_data64(fsqe, encodeUserData(slotIdx, /*isFsync=*/true));
            }
            inflightCount_ = 1;
        }
        io_uring_submit(&ring_);  // return ignored, matching the prior code path

        // Pop the submitted prefix; the slot owns the copy now. A fault-injected
        // short-write tail stays in batch_, renumbered on the next commit.
        batch_.erase(batch_.begin(),
                     batch_.begin() + static_cast<std::ptrdiff_t>(toWrite));
        for (auto& e : batch_) {
            e.sequenceNumber = 0;
            e.checksum = 0;
        }
    }

    // Reaper thread loop: reap CQEs, finalize chains, fire onCommit. Runs until
    // stopReaper() sets reaperStop_ and posts the NOP shutdown token.
    void reaperLoop() {
        for (;;) {
            struct io_uring_cqe* cqe = nullptr;
            // Bound worst-case reaper wakeup latency during quiet periods.
            // A bare io_uring_wait_cqe() blocks unboundedly until the next
            // completion (or the shutdown NOP), so a chain that completed just
            // after we parked — or a reaperStop_ set between iterations — could
            // sit unobserved. Peek first (non-blocking); only if the CQ is empty
            // do we block, and even then with a 1µs timeout so we loop back and
            // re-check reaperStop_ rather than parking indefinitely. Completion
            // processing below is byte-for-byte identical to the prior code.
            int rc = io_uring_peek_cqe(&ring_, &cqe);
            if (rc != 0 || !cqe) {
                struct __kernel_timespec ts{0, 1000};  // 1µs quiet-period cap
                rc = io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
            }
            if (rc < 0 || !cqe) {
                // ETIME (quiet-period timeout), EINTR, or spurious — exit if
                // we're shutting down, else loop back and peek again.
                if (reaperStop_.load(std::memory_order_acquire)) return;
                continue;
            }
            uint64_t ud = io_uring_cqe_get_data64(cqe);
            int res = cqe->res;
            io_uring_cqe_seen(&ring_, cqe);

            if (ud == kStopUserData) {
                if (reaperStop_.load(std::memory_order_acquire)) return;
                continue;
            }
            handleCqe(static_cast<uint32_t>(ud >> 1), (ud & 1ull) != 0, res);
        }
    }

    void handleCqe(uint32_t slotIdx, bool isFsync, int res) {
        {
            std::lock_guard<std::mutex> lk(ringMu_);
            Inflight& slot = inflight_[slotIdx];
            if (isFsync) {
                slot.fsyncRes = res;
            } else {
                slot.writeRes = res;
            }
            if (--slot.pendingCqes > 0) {
                return;  // await the paired CQE
            }
        }
        finalizeChain(slotIdx);
    }

    // Chain complete: compute durability, update counters, fire onCommit iff
    // durable, hand any real-short-write tail back to the writer, free the slot.
    void finalizeChain(uint32_t slotIdx) {
        const JournalEntry* bufPtr = nullptr;
        size_t written = 0;
        bool durable = false;
        {
            std::lock_guard<std::mutex> lk(ringMu_);
            Inflight& slot = inflight_[slotIdx];
            written = (slot.writeRes > 0)
                ? static_cast<size_t>(slot.writeRes) / sizeof(JournalEntry)
                : 0;
            if (written > slot.count) written = slot.count;  // defensive
            durable = slot.wantAck && written == slot.count && slot.fsyncRes == 0;
            bufPtr = slot.buf.data();

            // Real short write: the linked fdatasync still synced what landed,
            // so the `written` prefix is durable; the unwritten tail must be
            // renumbered and rewritten. Stage it for the writer to fold back.
            if (written < slot.count) {
                requeue_.assign(
                    slot.buf.begin() + static_cast<std::ptrdiff_t>(written),
                    slot.buf.end());
                for (auto& e : requeue_) { e.sequenceNumber = 0; e.checksum = 0; }
                pendingRewind_ = slot.count - static_cast<uint32_t>(written);
            }
        }

        // persistedEntries_ + metric reflect bytes that reached the file,
        // regardless of the ack decision (matches the synchronous path).
        persistedEntries_.fetch_add(written, std::memory_order_relaxed);
        static auto& kEntriesCommitted = MetricsRegistry::instance().counter(
            "journal_entries_committed_total",
            "Total journal entries successfully written to disk");
        kEntriesCommitted.increment(written);

        // Fire the ack STRICTLY after durability, on this reaper thread. The
        // slot stays active (inflightCount_ still 1) until after the callback,
        // so the writer cannot reuse the slot's buffer mid-callback. The
        // ReplicationCoordinator locks internally, so this is safe from here.
        if (durable && onCommit_ && written > 0) {
            onCommit_(bufPtr, written);
        }
        if (durable && onDurable_ && written > 0) {
            onDurable_(written);
        }

        {
            std::lock_guard<std::mutex> lk(ringMu_);
            Inflight& slot = inflight_[slotIdx];
            slot.active = false;
            slot.buf.clear();
            inflightCount_ = 0;
        }
        chainDone_.notify_all();
    }

    // Stop and join the reaper. Called from the dtor AFTER flush() has drained
    // all real chains, so the ring is idle and the NOP is the only completion.
    void stopReaper() {
        if (!reaper_.joinable()) {
            return;
        }
        reaperStop_.store(true, std::memory_order_release);
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (sqe) {
            io_uring_prep_nop(sqe);
            io_uring_sqe_set_data64(sqe, kStopUserData);
            io_uring_submit(&ring_);
        }
        reaper_.join();
    }
#endif  // __linux__ && OB_HAVE_LIBURING

    static uint64_t now() {
        // steady_clock — monotonic, unaffected by NTP step adjustments.
        // high_resolution_clock aliases system_clock on libstdc++ and
        // can move backwards on wall-clock corrections, which would
        // produce non-monotonic journal timestamps and break audit
        // replay / latency reconstruction.
        return static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
    }

private:
    void open(const char* mode) {
        file_ = std::fopen(filePath_.c_str(), mode);
        if (file_) {
            std::fseek(file_, 0, SEEK_END);
            establishHeader();
            std::fseek(file_, 0, SEEK_END);
            // Re-anchor the counter from the real file. Every path that
            // replaces the file underneath us — truncate(), commitRewrite() —
            // goes through close()/open(), so this is the single point where
            // the tracked size can drift back into agreement with the disk.
            const long size = std::ftell(file_);
            bytesOnDisk_.store(size > 0 ? static_cast<size_t>(size) : 0,
                               std::memory_order_relaxed);
        } else {
            bytesOnDisk_.store(0, std::memory_order_relaxed);
        }
    }

    void close() {
        if (file_) {
            std::fclose(file_);
            file_ = nullptr;
        }
    }

    // Write the first `toWrite` entries of batch_ to the page cache and return
    // the number of WHOLE entries that landed. Synchronous fwrite + fflush.
    //
    // This is the synchronous commit half, used by commitBatchSync() (macOS,
    // Linux without liburing / ring-init-failed, and the io_uring
    // SQE-exhaustion fallback). The io_uring write is issued directly by
    // commitBatchAsync() as the first link of the durability chain — it does
    // NOT go through here, so this stays a plain fwrite.
    // Put the header down before the first record. Called from both write
    // paths; a no-op after the first time and on pre-header files.
    void writePendingHeader() {
        if (!pendingHeader_ || !file_) return;
        JournalFileHeader h{};
        std::memcpy(h.magic, JOURNAL_MAGIC, sizeof(h.magic));
        h.formatVersion = JOURNAL_FORMAT_V1;
        h.recordSize    = static_cast<uint32_t>(sizeof(JournalEntry));
        h.reserved      = 0;
        std::fseek(file_, 0, SEEK_END);
        if (std::fwrite(&h, sizeof(h), 1, file_) == 1) {
            // Flushed before any record is written, and before io_uring may
            // touch the same fd — stdio buffering and the ring must not
            // interleave.
            std::fflush(file_);
            pendingHeader_ = false;
            bytesOnDisk_.fetch_add(sizeof(JournalFileHeader),
                                   std::memory_order_relaxed);
        }
    }

    size_t writeBatch(size_t toWrite) {
        writePendingHeader();
        size_t w = std::fwrite(batch_.data(), sizeof(JournalEntry), toWrite, file_);
        std::fflush(file_);
        return w;
    }

    // Returns true iff the durable barrier actually made the bytes durable.
    // Callers gate the replication ack (onCommit_) on this: an ack may only
    // be sent once the data is provably on stable storage.
    bool syncFile() const {
        if (!file_) {
            return false;
        }
        // Synchronous durability barrier for commitBatchSync(). On the async
        // io_uring path the fdatasync is the SECOND link of the chain issued by
        // commitBatchAsync(), so it does not go through here.
#ifdef __APPLE__
        return ::fcntl(fileno(file_), F_FULLFSYNC) == 0;
#else
        return ::fdatasync(fileno(file_)) == 0;
#endif
    }

    // ── Drain barrier (all platforms; body only on the async path) ──
    void drainCompletions() {
#if defined(__linux__) && defined(OB_HAVE_LIBURING)
        if (useIoUring_) {
            std::unique_lock<std::mutex> lk(ringMu_);
            chainDone_.wait(lk, [this] { return inflightCount_ == 0; });
        }
#endif
    }

    // A journal that is at least one whole record long but yields ZERO valid
    // records did not merely fail to recover — it was not understood at all.
    //
    // Records carry no magic number, no length prefix and no version field:
    // the file is a bare array of packed structs and framing is implicit in
    // sizeof(JournalEntry). So a layout change slices an existing file on the
    // wrong boundaries, every record fails CRC, readEntriesFromPath returns
    // empty, and the engine starts with an EMPTY BOOK — then appends new
    // records to the same file, because it is opened "ab+". Silent, total,
    // and on the upgrade path: no error, no log line, exit code 0.
    //
    // This does not add framing. It makes the signature of that failure loud
    // and stops the second half of it: the journal refuses to append, so an
    // unreadable file is not also destroyed.
    //
    // Deliberately NOT tripped by a file shorter than one record. A crash
    // during the very first write leaves a partial record and no valid
    // entries, which is a legitimate torn write that recovery already
    // tolerates — the existing truncation tests depend on that.
    // Write the header on a brand-new file; validate it on an existing one.
    //
    // A mismatch is refused the same way an unreadable file is — but it can
    // SAY what is wrong, which is the whole point of having a version and a
    // record size on disk. "format version 2, this build writes 1" is a
    // different conversation from "every record failed CRC".
    void establishHeader() {
        if (!file_) return;
        dataOffset_ = 0;

        const long size = std::ftell(file_);
        if (size <= 0) {
            // Deferred, NOT written here. Opening must not modify the file:
            // JournalFollower, ResearchHarness, JournalReplayCLI and
            // TimeMachine all construct a Journal purely to READ, and writing
            // a header at construction made a read-only consumer mutate the
            // file it was tailing — which broke the follower outright. The
            // header goes down immediately before the first record instead.
            pendingHeader_ = true;
            dataOffset_ = sizeof(JournalFileHeader);
            return;
        }

        JournalFileHeader h{};
        std::fseek(file_, 0, SEEK_SET);
        const bool got = std::fread(&h, sizeof(h), 1, file_) == 1;
        if (!got || std::memcmp(h.magic, JOURNAL_MAGIC, sizeof(h.magic)) != 0) {
            // Pre-header file: a bare array of records, still readable. Its
            // first byte is an entryType (1..5) and the magic starts with 'O',
            // so this is a definite answer, not a guess.
            dataOffset_ = 0;
            return;
        }

        dataOffset_ = sizeof(JournalFileHeader);
        if (h.formatVersion != JOURNAL_FORMAT_V1 ||
            h.recordSize != sizeof(JournalEntry)) {
            recoveryFailed_ = true;
            std::fprintf(stderr,
                "[Journal] REFUSING TO APPEND: %s was written in journal format "
                "v%u with %u-byte records;\n"
                "          this build reads v%u with %zu-byte records. Reading it "
                "would misframe every\n"
                "          record and start the engine with an EMPTY BOOK, and "
                "appending would make the\n"
                "          file permanently unreadable. Replay it with the build "
                "that wrote it, or\n"
                "          start from a checkpoint.\n",
                filePath_.c_str(), h.formatVersion, h.recordSize,
                JOURNAL_FORMAT_V1, sizeof(JournalEntry));
        }
    }

    void checkRecoverable() {
        const size_t readable = persistedEntries_.load(std::memory_order_relaxed);
        if (readable > 0) {
            // Readable is not the same as recoverable, and this is the gap the
            // first version of this guard had.
            //
            // It decided purely on the LAX read — CRC checked, sequence not —
            // so a file whose records are individually intact but whose
            // sequence does not start at 1 looked healthy here. Recovery does
            // not use that read: replayJournal calls readAll(true, TRUE), and
            // strict mode expects the first record to be sequence 1 and stops
            // at the first gap. So a file like that yields records here and
            // ZERO to replay, and the engine starts with an empty book while a
            // full journal sits beside it — the exact failure this guard was
            // written to make loud, arriving through the one door it left open.
            //
            // Not reachable today: everything that writes a journal numbers
            // from 1. It becomes reachable the moment anything removes a
            // prefix — a compactor, a manual splice, a half-applied
            // migration — which is precisely when nobody would be looking.
            if (strictPrefixEntries_ == 0) {
                recoveryFailed_ = true;
                std::fprintf(stderr,
                    "[Journal] REFUSING TO APPEND: %s holds %zu readable "
                    "record(s), but recovery can\n"
                    "          use none of them: the first record is sequence "
                    "%llu, and replay\n"
                    "          requires the log to start at 1 and be "
                    "contiguous. Starting would\n"
                    "          serve an EMPTY BOOK from a full journal. Move "
                    "the file aside, or\n"
                    "          replay it with a build that understands its "
                    "numbering.\n",
                    filePath_.c_str(), readable,
                    (unsigned long long)firstSequenceOnDisk());
            }
            return;
        }
        // Record bytes, excluding any file header — a file holding nothing but
        // a header is a fresh journal, not an unreadable one.
        const size_t total = bytesOnDisk();
        const size_t bytes = total > dataOffset_ ? total - dataOffset_ : 0;
        if (bytes < sizeof(JournalEntry)) {
            return;   // empty, or a torn first record
        }
        recoveryFailed_ = true;
        std::fprintf(stderr,
            "[Journal] REFUSING TO APPEND: %s is %zu bytes (>= %zu, one whole\n"
            "          record) but no valid record could be read from it.\n"
            "          Every record failing at once is the signature of a\n"
            "          format mismatch, not of corruption: this file was most\n"
            "          likely written by a build with a different\n"
            "          JournalEntry layout. Recovering from it would start the\n"
            "          engine with an EMPTY BOOK, and appending to it would\n"
            "          make it permanently unreadable, so appends are now\n"
            "          refused. Move the file aside and start from a\n"
            "          checkpoint, or replay it with the build that wrote it.\n",
            filePath_.c_str(), bytes, sizeof(JournalEntry));
    }

    // First record's sequence number, for diagnostics only.
    uint64_t firstSequenceOnDisk() const {
        auto entries = readEntriesFromPath(filePath_, true, false);
        return entries.empty() ? 0 : entries.front().sequenceNumber;
    }

    uint64_t recoverSequenceFromDisk() {
        auto entries = readEntriesFromPath(filePath_, true, false);
        persistedEntries_.store(entries.size(), std::memory_order_relaxed);

        // How many records a STRICT read would return, computed from the lax
        // read rather than by reading the file a second time — the sequence
        // numbers are already in hand, and this runs in the constructor, which
        // JournalFollower invokes on every poll.
        //
        // Mirrors readEntriesFromPath's strict mode exactly: expect 1, then
        // +1, stop at the first gap.
        size_t strictPrefix = 0;
        uint64_t expected = 1;
        for (const auto& e : entries) {
            if (e.sequenceNumber != expected) break;
            ++strictPrefix;
            ++expected;
        }
        strictPrefixEntries_ = strictPrefix;

        if (entries.empty()) {
            return 0;
        }
        return entries.back().sequenceNumber;
    }

    static std::vector<JournalEntry> readEntriesFromPath(const std::string& path,
                                                         bool validateCRC,
                                                         bool validateSequence) {
        std::vector<JournalEntry> entries;
        FILE* file = std::fopen(path.c_str(), "rb");
        if (!file) {
            return entries;
        }
        // Skip the file header if there is one. A pre-header journal reports 0
        // and is read exactly as before.
        if (const size_t skip = headerBytesOf(path); skip > 0) {
            std::fseek(file, static_cast<long>(skip), SEEK_SET);
        }

        JournalEntry entry{};
        uint64_t expectedSequence = 1;
        while (std::fread(&entry, sizeof(JournalEntry), 1, file) == 1) {
            if (validateCRC) {
                uint32_t expected = computeCRC32(&entry, offsetof(JournalEntry, checksum));
                if (entry.checksum != expected) {
                    break;
                }
            }

            if (validateSequence) {
                if (entry.sequenceNumber != expectedSequence) {
                    break;
                }
                ++expectedSequence;
            }

            entries.push_back(entry);
        }

        std::fclose(file);
        return entries;
    }

    FILE* file_{nullptr};
    std::string filePath_;
    SyncPolicy syncPolicy_{SyncPolicy::GroupCommit};
    size_t batchSize_{64};
    uint64_t sequence_{0};
    // Written by the reaper thread on the async path (durable-write count) and
    // by the writer thread on the sync path / setup — hence atomic. Read by the
    // background checkpoint thread via needsCheckpoint().
    bool recoveryFailed_{false};
    // Records a STRICT read would return: the contiguous-from-1 prefix of what
    // the lax read found. See checkRecoverable().
    size_t strictPrefixEntries_{0};
    // Bytes before the first record: header size, or 0 for a pre-header file.
    size_t dataOffset_{0};
    // A header is owed to this file but not yet written. See establishHeader().
    bool   pendingHeader_{false};

    // Bytes of whole entries on disk. See bytesOnDisk().
    std::atomic<size_t> bytesOnDisk_{0};
    std::atomic<size_t> persistedEntries_{0};
    uint64_t            entriesAppended_{0};
    std::vector<JournalEntry> batch_;
    // CONTRACT: onCommit_ fires on the reaper thread — must be lock-free and
    // must not touch OrderBook state.
    //
    // On the async io_uring path finalizeChain() invokes onCommit_ from the
    // reaper thread, concurrently with the matching thread that owns and mutates
    // the OrderBook. A callback that grabbed a lock the matching thread also
    // holds, or that read/wrote OrderBook state without external synchronization,
    // would either stall the durability reaper or race the matching thread. On
    // the synchronous path it fires inline on the writer thread, but the same
    // discipline is required so a single callback body is safe under both paths.
    // Keep it to lock-free byte-shipping only — e.g. hand committed bytes to the
    // ReplicationCoordinator's transport; never reach back into engine state.
    OnCommitFn onCommit_;
    std::function<void(size_t)> onDurable_;
    size_t maxSizeMb_{0};

#if defined(__linux__) && defined(OB_HAVE_LIBURING)
    // Per-Journal io_uring ring. Depth 8 is ample — the async path keeps a
    // single durability chain outstanding (2 linked SQEs) plus the shutdown
    // NOP. The writer thread is the sole submitter; the reaper thread is the
    // sole completion consumer, so the ring itself needs no locking.
    static constexpr unsigned kRingDepth = 8;
    mutable struct io_uring ring_{};
    bool useIoUring_{false};

    // ── Async ack machinery (Hazards A–D) ──
    // One durability chain (write → fdatasync) in flight at a time. A chain's
    // entries are copied into a free Inflight slot before submission; the slot
    // owns those bytes until BOTH CQEs are reaped, so the SQE buffer never
    // dangles. Only the reaper thread frees a slot.
    static constexpr unsigned kInflightDepth = 8;
    static constexpr uint64_t kStopUserData = ~uint64_t{0};  // NOP shutdown token
    struct Inflight {
        std::vector<JournalEntry> buf;   // copy submitted in this chain
        uint64_t firstSeq{0};            // sequenceNumber of buf.front()
        uint32_t count{0};               // entries submitted
        int32_t  writeRes{INT32_MIN};    // write CQE res (bytes, or -errno)
        int32_t  fsyncRes{INT32_MIN};    // fdatasync CQE res (0, or -errno)
        uint16_t pendingCqes{0};         // CQEs still to reap for this chain
        bool     wantAck{true};          // false when fsync_fail fault armed
        bool     active{false};          // slot occupied
    };
    std::array<Inflight, kInflightDepth> inflight_{};
    uint32_t nextSlot_{0};               // round-robin slot cursor (writer)
    uint32_t inflightCount_{0};          // chains in flight (0 or 1)
    uint32_t pendingRewind_{0};          // real-short-write shortfall to apply
    std::vector<JournalEntry> requeue_;  // real-short-write tail, writer re-commits
    std::mutex ringMu_;                  // guards inflight_/inflightCount_/requeue_
    std::condition_variable chainDone_;  // signalled when inflightCount_ hits 0
    std::thread reaper_;
    std::atomic<bool> reaperStop_{false};
#endif
};

} // namespace OrderMatcher
