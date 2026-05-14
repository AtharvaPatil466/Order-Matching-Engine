#pragma once

// MoldPacketJournal — bounded in-memory log of recent MoldUDP64
// application messages, keyed by sequence number.
//
// Real Nasdaq retransmission services back this with a persistent
// log so a subscriber can recover from gaps spanning hours. For a
// single-process implementation a fixed-capacity ring is sufficient
// and demonstrates the same wire contract: the retransmission
// service is correct iff every record() that the publisher emits
// stays available in the journal until either (a) the subscriber
// catches up, or (b) the journal evicts it (in which case the
// subscriber is too far behind and must take a full snapshot).
//
// Concurrency: a single-writer, multi-reader pattern is supported.
// The writer (the publisher) calls record() under no internal lock
// — it's the caller's responsibility not to call record() and
// replayRange() concurrently from different threads. For most
// deployments, the publisher writes from the engine-side thread
// and the retransmission service reads from one or more TCP
// connection threads; that needs external synchronization. In this
// implementation we use a single mutex to make the journal
// trivially safe across threads at the cost of some lock contention
// on the publish hot path.

#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

namespace OrderMatcher {

class MoldPacketJournal {
public:
    explicit MoldPacketJournal(size_t maxEntries = 4096)
        : maxEntries_(maxEntries) {}

    // Record one application message at the given sequence number.
    // Sequence numbers should be monotonically increasing — duplicate
    // or out-of-order writes are silently appended (the service
    // would surface them as duplicates on replay).
    void record(uint64_t seq, const void* payload, uint16_t len) {
        std::lock_guard<std::mutex> lock(mu_);
        Entry e;
        e.seq = seq;
        e.bytes.assign(reinterpret_cast<const uint8_t*>(payload),
                       reinterpret_cast<const uint8_t*>(payload) + len);
        entries_.push_back(std::move(e));
        // Trim to bound. Eviction is from the oldest end; the lowest
        // available sequence steps up after eviction.
        while (entries_.size() > maxEntries_) entries_.pop_front();
    }

    // Replay messages in [startSeq, startSeq + count). For each
    // sequence that's still in the journal, the callback is invoked
    // with (seq, payload, len). Returns the count of messages
    // actually delivered (will be < count if some have been evicted
    // or are not yet recorded).
    //
    // Pass count == 0 to replay from startSeq to the end of the
    // journal.
    size_t replayRange(uint64_t startSeq, uint16_t count,
                       std::function<void(uint64_t, const uint8_t*, size_t)> cb) const {
        std::lock_guard<std::mutex> lock(mu_);
        if (entries_.empty()) return 0;

        // Bound the request to what we can actually answer.
        uint64_t endSeq;
        if (count == 0) {
            endSeq = entries_.back().seq + 1;
        } else {
            endSeq = startSeq + count;
        }

        size_t delivered = 0;
        for (const auto& e : entries_) {
            if (e.seq < startSeq) continue;
            if (e.seq >= endSeq) break;
            cb(e.seq, e.bytes.data(), e.bytes.size());
            ++delivered;
        }
        return delivered;
    }

    bool contains(uint64_t seq) const {
        std::lock_guard<std::mutex> lock(mu_);
        if (entries_.empty()) return false;
        return seq >= entries_.front().seq && seq <= entries_.back().seq;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mu_);
        return entries_.size();
    }

    uint64_t lowestSeq() const {
        std::lock_guard<std::mutex> lock(mu_);
        return entries_.empty() ? 0 : entries_.front().seq;
    }

    uint64_t highestSeq() const {
        std::lock_guard<std::mutex> lock(mu_);
        return entries_.empty() ? 0 : entries_.back().seq;
    }

private:
    struct Entry {
        uint64_t              seq{0};
        std::vector<uint8_t>  bytes;
    };

    mutable std::mutex     mu_;
    std::deque<Entry>      entries_;
    size_t                 maxEntries_;
};

}  // namespace OrderMatcher
