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
        // A previous rewriteAtomically() that crashed between close()
        // and rename() can leave a stale "<path>.tmp" sibling. Remove
        // it on startup so it does not accumulate across restarts and
        // so a half-written checkpoint cannot be picked up by any
        // future tool that lists the directory.
        std::remove((filePath_ + ".tmp").c_str());

        open("ab+");
        sequence_ = recoverSequenceFromDisk();

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

    void logCancelOrder(OrderId id) {
        JournalEntry entry{};
        entry.entryType = JournalEntry::Type::CancelOrder;
        entry.timestamp = now();
        entry.orderId = id;
        appendEntry(entry);
    }

    void logModifyOrder(OrderId id, Quantity newQty) {
        JournalEntry entry{};
        entry.entryType = JournalEntry::Type::ModifyOrder;
        entry.timestamp = now();
        entry.orderId = id;
        entry.newQty = newQty;
        appendEntry(entry);
    }

    void logCancelReplace(OrderId id, Price newPrice, Quantity newQty) {
        JournalEntry entry{};
        entry.entryType = JournalEntry::Type::CancelReplace;
        entry.timestamp = now();
        entry.orderId = id;
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
            commitBatch();
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

    bool rewriteAtomically(const std::function<void(Journal&)>& writer) {
        flush();

        const std::string tmpPath = filePath_ + ".tmp";
        {
            Journal temp(tmpPath, SyncPolicy::Immediate, 1);
            temp.truncate();
            writer(temp);
            temp.flush();
        }

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
    size_t bytesOnDisk() const {
        if (!file_) {
            return 0;
        }
#ifdef __linux__
        // io_uring writes bypass the stdio stream, so ftell(file_) would not
        // reflect them. Ask the kernel for the true size — this is also correct
        // for the fwrite fallback path (commitBatch always fflushes), so it is
        // used unconditionally on Linux.
        struct stat st;
        if (::fstat(fileno(file_), &st) == 0) {
            return static_cast<size_t>(st.st_size);
        }
        return 0;
#else
        long current = std::ftell(file_);
        if (current < 0) {
            return 0;
        }
        return static_cast<size_t>(current);
#endif
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
        batch_.push_back(entry);

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
    size_t writeBatch(size_t toWrite) {
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

    uint64_t recoverSequenceFromDisk() {
        auto entries = readEntriesFromPath(filePath_, true, false);
        persistedEntries_.store(entries.size(), std::memory_order_relaxed);
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
    std::atomic<size_t> persistedEntries_{0};
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

class GroupCommitJournal : public Journal {
public:
    explicit GroupCommitJournal(const std::string& filePath, size_t batchSize = 64)
        : Journal(filePath, SyncPolicy::GroupCommit, batchSize) {}

    void logAddOrderBatched(OrderId id, ParticipantId pid, SymbolId sym, Side side, Price price,
                            Quantity qty, OrderType type, TimeInForce tif = TimeInForce::GTC,
                            uint64_t expiry = 0, Price stopPrice = 0, Price stopLimitPrice = 0,
                            Quantity displayQty = 0, PegType pegType = PegType::None,
                            Price pegOffset = 0, Price trailAmount = 0, Quantity minQty = 0,
                            bool hidden = false) {
        logAddOrder(id, pid, sym, side, price, qty, type, tif, expiry, stopPrice,
                    stopLimitPrice, displayQty, pegType, pegOffset, trailAmount, minQty,
                    hidden);
    }

    void logCancelOrderBatched(OrderId id) { logCancelOrder(id); }
    void logModifyOrderBatched(OrderId id, Quantity newQty) { logModifyOrder(id, newQty); }
    void logCancelReplaceBatched(OrderId id, Price newPrice, Quantity newQty) {
        logCancelReplace(id, newPrice, newQty);
    }
};

} // namespace OrderMatcher
