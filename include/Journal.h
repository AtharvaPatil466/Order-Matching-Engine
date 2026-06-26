#pragma once

#include "Types.h"
#include "FaultInjector.h"
#include "Metrics.h"
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
    }

    ~Journal() {
        flush();
        close();
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
    }

    void truncate() {
        flush();
        close();
        open("wb+");
        sequence_ = 0;
        persistedEntries_ = 0;
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
        persistedEntries_ = static_cast<size_t>(sequence_);
        return true;
    }

    bool needsCheckpoint(size_t maxEntries, size_t maxBytes) const {
        return persistedEntries_ >= maxEntries || bytesOnDisk() >= maxBytes;
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
        long current = std::ftell(file_);
        if (current < 0) {
            return 0;
        }
        return static_cast<size_t>(current);
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

        size_t actuallyWritten =
            std::fwrite(batch_.data(), sizeof(JournalEntry), toWrite, file_);
        std::fflush(file_);

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

        persistedEntries_ += actuallyWritten;
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

    // Returns true iff the durable barrier actually made the bytes durable.
    // Callers gate the replication ack (onCommit_) on this: an ack may only
    // be sent once the data is provably on stable storage.
    bool syncFile() const {
        if (!file_) {
            return false;
        }
#ifdef __APPLE__
        return ::fcntl(fileno(file_), F_FULLFSYNC) == 0;
#else
        return ::fdatasync(fileno(file_)) == 0;
#endif
    }

    uint64_t recoverSequenceFromDisk() {
        auto entries = readEntriesFromPath(filePath_, true, false);
        persistedEntries_ = entries.size();
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
    size_t persistedEntries_{0};
    std::vector<JournalEntry> batch_;
    OnCommitFn onCommit_;
    size_t maxSizeMb_{0};
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
