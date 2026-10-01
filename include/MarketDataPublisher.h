#pragma once

#include "OrderBook.h"
#include <string>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <memory>

// POSIX shared memory
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace OrderMatcher {

// Shared memory layout for market data feed.
// Publisher writes, subscribers mmap and read.
// Lock-free via sequence numbers: subscriber spins on sequence to detect new entries.
//
// Each slot is a seqlock. ShmEntry::sequence is the lock word: the writer sets
// it to SLOT_WRITING, writes the payload, then stores the entry's sequence. A
// reader accepts a slot only if it reads the sequence it wants both before and
// after copying the payload, so a writer that laps it mid-copy can't hand it
// half of one update and half of the next. Payload words are copied with
// relaxed atomics: the copy races the writer by design, and a plain memcpy
// would be a data race (and a TSan report) on every lap.
//
// ShmHeader::epoch names the publisher session: its start time in ns, set by
// start(), and 0 once stop() has run. A subscriber remembers the epoch it
// connected to; when it changes, poll() returns Reset rather than waiting
// forever on a ring that a restarted publisher rewound to 0 (or that a
// stopped one unlinked). The field sits in the header's existing padding, so
// the layout and VERSION are unchanged; a v1 publisher leaves it 0.

struct ShmHeader {
    static constexpr uint32_t MAGIC = 0x4D444658; // "MDFX"
    static constexpr uint32_t VERSION = 1;
    static constexpr uint32_t MIN_READABLE_VERSION = 1;

    uint32_t magic;
    uint32_t version;
    uint32_t entrySize;      // sizeof(ShmEntry)
    uint32_t capacity;       // number of entries in ring
    std::atomic<uint64_t> epoch;                  // publisher session; 0 = stopped
    alignas(64) std::atomic<uint64_t> writeSeq;  // next sequence to write
    char padding[64 - sizeof(std::atomic<uint64_t>)];
};
static_assert(offsetof(ShmHeader, epoch) == 16 && offsetof(ShmHeader, writeSeq) == 64 &&
              sizeof(ShmHeader) == 128, "the header is shared with other processes");

struct ShmEntry {
    enum class Type : uint8_t {
        IncrementalUpdate = 1,
        Snapshot = 2
    };

    uint64_t sequence;         // monotonic sequence number
    Type type;
    uint8_t padding1[7];

    // For incremental updates
    MarketDataUpdate update;

    // For snapshots (top 5 levels)
    static constexpr size_t MAX_DEPTH = 5;
    SymbolId symbolId;
    Price lastTradePrice;
    Quantity lastTradeQty;
    uint64_t timestamp;
    uint32_t bidCount;
    uint32_t askCount;
    PriceLevel bidLevels[MAX_DEPTH];
    PriceLevel askLevels[MAX_DEPTH];
};

namespace shm_detail {
// Never a real sequence: the slot is being rewritten.
inline constexpr uint64_t SLOT_WRITING = ~uint64_t{0};
inline constexpr size_t ENTRY_WORDS = sizeof(ShmEntry) / sizeof(uint64_t);
static_assert(sizeof(ShmEntry) % sizeof(uint64_t) == 0 && alignof(ShmEntry) >= alignof(uint64_t),
              "the seqlock copies ShmEntry as whole 64-bit words");
static_assert(offsetof(ShmEntry, sequence) == 0, "the lock word is the first word");

inline std::atomic_ref<uint64_t> word(const void* base, size_t i) {
    return std::atomic_ref<uint64_t>(
        const_cast<uint64_t*>(static_cast<const uint64_t*>(base))[i]);
}
} // namespace shm_detail

class MarketDataPublisher {
public:
    explicit MarketDataPublisher(const std::string& name, size_t capacity = 4096)
        : shmName_("/" + name), capacity_(capacity), shmFd_(-1), shmPtr_(nullptr), shmSize_(0) {}

    ~MarketDataPublisher() {
        stop();
    }

    // Disallow copy
    MarketDataPublisher(const MarketDataPublisher&) = delete;
    MarketDataPublisher& operator=(const MarketDataPublisher&) = delete;

    bool start() {
        shmSize_ = sizeof(ShmHeader) + capacity_ * sizeof(ShmEntry);

        // Create or open shared memory
        shmFd_ = shm_open(shmName_.c_str(), O_CREAT | O_RDWR, 0666);
        if (shmFd_ < 0) return false;

        // A publisher that crashed leaves its segment behind. Grow it only if
        // it is too small: macOS refuses a second ftruncate of a shm object
        // (EINVAL) and reports its size rounded up to a page, so truncating
        // unconditionally meant a restarted publisher could never start there.
        struct stat st{};
        if (fstat(shmFd_, &st) != 0 ||
            (static_cast<size_t>(st.st_size) < shmSize_ &&
             ftruncate(shmFd_, static_cast<off_t>(shmSize_)) != 0)) {
            close(shmFd_);
            shmFd_ = -1;
            return false;
        }

        shmPtr_ = mmap(nullptr, shmSize_, PROT_READ | PROT_WRITE, MAP_SHARED, shmFd_, 0);
        if (shmPtr_ == MAP_FAILED) {
            close(shmFd_);
            shmFd_ = -1;
            shmPtr_ = nullptr;
            return false;
        }

        // Initialize header. On a reused segment, epoch goes to 0 first, so a
        // subscriber of the old session is told before the ring rewinds, and
        // the new epoch goes in last, so one that connects and reads it also
        // sees writeSeq == 0.
        auto* header = getHeader();
        header->epoch.store(0, std::memory_order_release);
        header->magic = ShmHeader::MAGIC;
        header->version = ShmHeader::VERSION;
        header->entrySize = sizeof(ShmEntry);
        header->capacity = static_cast<uint32_t>(capacity_);
        header->writeSeq.store(0, std::memory_order_release);
        header->epoch.store(newEpoch(), std::memory_order_release);

        running_ = true;
        return true;
    }

    void stop() {
        running_ = false;
        if (shmPtr_ && shmPtr_ != MAP_FAILED) {
            getHeader()->epoch.store(0, std::memory_order_release);
            munmap(shmPtr_, shmSize_);
            shmPtr_ = nullptr;
        }
        if (shmFd_ >= 0) {
            close(shmFd_);
            shmFd_ = -1;
        }
        shm_unlink(shmName_.c_str());
    }

    // Publish an incremental market data update
    void publishUpdate(const MarketDataUpdate& update) {
        if (!running_) return;

        ShmEntry entry{};
        entry.type = ShmEntry::Type::IncrementalUpdate;
        entry.update = update;
        commit(entry);
    }

    // Publish a full L2 snapshot
    void publishSnapshot(const MarketDataSnapshot& snap) {
        if (!running_) return;

        ShmEntry entry{};
        entry.type = ShmEntry::Type::Snapshot;
        entry.symbolId = snap.symbolId;
        entry.lastTradePrice = snap.lastTradePrice;
        entry.lastTradeQty = snap.lastTradeQty;
        entry.timestamp = snap.timestamp;

        size_t bidCount = std::min(static_cast<size_t>(snap.bidCount), ShmEntry::MAX_DEPTH);
        size_t askCount = std::min(static_cast<size_t>(snap.askCount), ShmEntry::MAX_DEPTH);
        entry.bidCount = static_cast<uint32_t>(bidCount);
        entry.askCount = static_cast<uint32_t>(askCount);

        for (size_t i = 0; i < bidCount; ++i)
            entry.bidLevels[i] = snap.bids[i];
        for (size_t i = 0; i < askCount; ++i)
            entry.askLevels[i] = snap.asks[i];

        commit(entry);
    }

    // Hook into OrderBook via EventListener interface
    struct MdListener : EventListener {
        MarketDataPublisher* pub;
        explicit MdListener(MarketDataPublisher* p) : pub(p) {}
        void onMarketData(const MarketDataUpdate& u) override { pub->publishUpdate(u); }
    };

    // Create and return a listener that publishes market data to shared memory.
    // Caller must keep the returned listener alive and pass it to OrderBook::setEventListener().
    std::unique_ptr<MdListener> createListener() {
        return std::make_unique<MdListener>(this);
    }

    uint64_t getSequence() const {
        if (!running_) return 0;
        return getHeader()->writeSeq.load(std::memory_order_acquire);
    }

    const std::string& shmName() const { return shmName_; }
    bool isRunning() const { return running_; }

private:
    // Seqlock write of the next entry (see the layout comment).
    void commit(const ShmEntry& entry) {
        auto* header = getHeader();
        const uint64_t seq = header->writeSeq.load(std::memory_order_relaxed);
        ShmEntry* slot = getEntry(seq);

        shm_detail::word(slot, 0).store(shm_detail::SLOT_WRITING, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        const auto* src = reinterpret_cast<const uint64_t*>(&entry);
        for (size_t i = 1; i < shm_detail::ENTRY_WORDS; ++i)
            shm_detail::word(slot, i).store(src[i], std::memory_order_relaxed);
        shm_detail::word(slot, 0).store(seq, std::memory_order_release);

        header->writeSeq.store(seq + 1, std::memory_order_release);
    }

    static uint64_t newEpoch() {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return ns > 0 ? static_cast<uint64_t>(ns) : 1;
    }

    ShmHeader* getHeader() const {
        return static_cast<ShmHeader*>(shmPtr_);
    }

    ShmEntry* getEntry(uint64_t seq) const {
        size_t idx = seq % capacity_;
        auto* base = static_cast<char*>(shmPtr_) + sizeof(ShmHeader);
        return reinterpret_cast<ShmEntry*>(base + idx * sizeof(ShmEntry));
    }

    std::string shmName_;
    size_t capacity_;
    int shmFd_;
    void* shmPtr_;
    size_t shmSize_;
    bool running_{false};
};

// What MarketDataSubscriber::poll found. `out` is written only for Entry.
enum class PollResult : uint8_t {
    Empty,  // nothing new yet
    Entry,  // `out` holds the next entry
    Gap,    // the publisher lapped us and entries are lost; readSequence()
            // jumped to the oldest one left. Resync the book from a snapshot.
    Reset,  // the publisher restarted or stopped (its epoch changed). The
            // subscriber has disconnected: connect() again, then resync.
};

// Subscriber: read-only access to shared memory market data feed.
// Runs in a separate process; maps the same shared memory segment.

class MarketDataSubscriber {
public:
    explicit MarketDataSubscriber(const std::string& name)
        : shmName_("/" + name), shmFd_(-1), shmPtr_(nullptr), shmSize_(0), readSeq_(0) {}

    ~MarketDataSubscriber() {
        disconnect();
    }

    MarketDataSubscriber(const MarketDataSubscriber&) = delete;
    MarketDataSubscriber& operator=(const MarketDataSubscriber&) = delete;

    bool connect() {
        shmFd_ = shm_open(shmName_.c_str(), O_RDONLY, 0);
        if (shmFd_ < 0) return false;

        // Get size
        struct stat st;
        if (fstat(shmFd_, &st) != 0) {
            close(shmFd_);
            shmFd_ = -1;
            return false;
        }
        shmSize_ = static_cast<size_t>(st.st_size);

        shmPtr_ = mmap(nullptr, shmSize_, PROT_READ, MAP_SHARED, shmFd_, 0);
        if (shmPtr_ == MAP_FAILED) {
            close(shmFd_);
            shmFd_ = -1;
            shmPtr_ = nullptr;
            return false;
        }

        // Validate header. The feed is prefix-compatible: a newer publisher
        // may append fields to ShmEntry and advertise a larger entrySize, but
        // existing subscribers only read the prefix they understand.
        auto* header = getHeader();
        if (shmSize_ < sizeof(ShmHeader) ||
            header->magic != ShmHeader::MAGIC ||
            header->version < ShmHeader::MIN_READABLE_VERSION ||
            header->version > ShmHeader::VERSION ||
            header->entrySize < sizeof(ShmEntry) ||
            header->capacity == 0) {
            disconnect();
            return false;
        }

        capacity_ = header->capacity;
        entrySize_ = header->entrySize;
        epoch_ = header->epoch.load(std::memory_order_acquire);
        readSeq_ = 0;
        size_t requiredSize = sizeof(ShmHeader) + capacity_ * entrySize_;
        if (shmSize_ < requiredSize) {
            disconnect();
            return false;
        }
        return true;
    }

    void disconnect() {
        if (shmPtr_ && shmPtr_ != MAP_FAILED) {
            munmap(const_cast<void*>(shmPtr_), shmSize_);
            shmPtr_ = nullptr;
        }
        if (shmFd_ >= 0) {
            close(shmFd_);
            shmFd_ = -1;
        }
    }

    // Poll for the next entry; see PollResult.
    PollResult poll(ShmEntry& out) {
        if (!shmPtr_) return PollResult::Empty;

        auto* header = getHeader();
        if (header->epoch.load(std::memory_order_acquire) != epoch_) return reset();

        uint64_t writeSeq = header->writeSeq.load(std::memory_order_acquire);
        if (readSeq_ >= writeSeq) return PollResult::Empty;

        if (writeSeq - readSeq_ > capacity_) {
            readSeq_ = writeSeq - capacity_; // oldest still in the ring
            return PollResult::Gap;
        }

        // Seqlock read: the slot must hold readSeq_ before and after the copy.
        const ShmEntry* slot = getEntry(readSeq_);
        if (shm_detail::word(slot, 0).load(std::memory_order_acquire) == readSeq_) {
            auto* dst = reinterpret_cast<uint64_t*>(&out);
            for (size_t i = 0; i < shm_detail::ENTRY_WORDS; ++i)
                dst[i] = shm_detail::word(slot, i).load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (shm_detail::word(slot, 0).load(std::memory_order_relaxed) == readSeq_) {
                // A restarted publisher may have rewritten this slot with the
                // same sequence; its new epoch is visible by now if so.
                if (header->epoch.load(std::memory_order_relaxed) != epoch_) return reset();
                out.sequence = readSeq_;
                readSeq_++;
                return PollResult::Entry;
            }
        }

        // The writer lapped us while we read: readSeq_ is gone.
        writeSeq = header->writeSeq.load(std::memory_order_acquire);
        readSeq_ = std::max(readSeq_ + 1, writeSeq > capacity_ ? writeSeq - capacity_ : 0);
        return PollResult::Gap;
    }

    uint64_t readSequence() const { return readSeq_; }
    void setReadSequence(uint64_t seq) { readSeq_ = seq; }

    uint64_t gapCount() const {
        if (!shmPtr_) return 0;
        uint64_t writeSeq = getHeader()->writeSeq.load(std::memory_order_acquire);
        return (writeSeq > readSeq_) ? (writeSeq - readSeq_) : 0;
    }

private:
    PollResult reset() {
        disconnect();
        return PollResult::Reset;
    }

    const ShmHeader* getHeader() const {
        return static_cast<const ShmHeader*>(shmPtr_);
    }

    const ShmEntry* getEntry(uint64_t seq) const {
        size_t idx = seq % capacity_;
        auto* base = static_cast<const char*>(shmPtr_) + sizeof(ShmHeader);
        return reinterpret_cast<const ShmEntry*>(base + idx * entrySize_);
    }

    std::string shmName_;
    int shmFd_;
    const void* shmPtr_;
    size_t shmSize_;
    size_t capacity_{0};
    size_t entrySize_{0};
    uint64_t epoch_{0};
    uint64_t readSeq_;
};

} // namespace OrderMatcher
