#pragma once

#include "FaultInjector.h"
#include "HugePageAllocator.h"

#include <atomic>
#include <vector>
#include <cassert>
#include <cstddef>
#include <new>

namespace OrderMatcher {

// Lock-free Multi-Producer Single-Consumer (MPSC) bounded queue.
//
// Design: Array-based ring buffer with per-slot state flags.
//   - Producers: atomically claim a write slot by CAS on writePos_
//   - Each slot has an atomic flag: 0=empty, 1=written
//   - Producer writes data, then sets flag to 1 (release)
//   - Consumer reads from readPos_, checks flag (acquire), advances when ready
//   - Power-of-2 sizing for bitmask indexing
//
// This eliminates the need for a producer mutex while remaining lock-free.

    // Hardcode cache line size to 64 bytes to avoid GCC -Winterference-size warnings
    constexpr size_t MPSC_CACHE_LINE = 64;

template<typename T>
class MpscQueue {
public:
    explicit MpscQueue(size_t capacity) : capacity_(capacity), mask_(capacity - 1) {
        assert((capacity & (capacity - 1)) == 0 && "Capacity must be power of 2");
        // Back the ring's slot storage with 2 MB huge pages (falls back to the
        // historical aligned new[] off Linux / when unreserved). Each Slot is
        // cache-line sized, so a large ring spans many pages on the hot enqueue
        // path — huge pages keep it TLB-resident. Slots are placement-
        // constructed into the raw block and destroyed in the dtor.
        slotsAlloc_ = hugeAlloc(sizeof(Slot) * capacity, alignof(Slot));
        slots_ = static_cast<Slot*>(slotsAlloc_.ptr);
        for (size_t i = 0; i < capacity; ++i) {
            new (&slots_[i]) Slot();
            slots_[i].sequence.store(i, std::memory_order_relaxed);
        }
        writePos_.store(0, std::memory_order_relaxed);
        readPos_.store(0, std::memory_order_relaxed);
    }

    ~MpscQueue() {
        for (size_t i = 0; i < capacity_; ++i) {
            slots_[i].~Slot();
        }
        hugeFree(slotsAlloc_);
    }

    MpscQueue(const MpscQueue&) = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;

    // Push (multiple producers). Returns false if queue is full.
    bool push(const T& item) {
        // Fault injection: simulate a spurious "queue full" return even when
        // capacity is available. Exercises the engine's retry loop and the
        // QueueBackpressure rejection path; never causes lost or duplicated
        // items because the caller observes a clean false return.
        if (FaultInjector::instance().shouldFail("queue.push.spurious_fail")) {
            return false;
        }
        size_t pos = writePos_.load(std::memory_order_relaxed);

        for (;;) {
            Slot& slot = slots_[pos & mask_];
            size_t seq = slot.sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

            if (diff == 0) {
                // Slot is available for writing at this position
                if (writePos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    slot.data = item;
                    slot.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
                // CAS failed, pos was updated by compare_exchange_weak, retry
            } else if (diff < 0) {
                // Queue is full
                return false;
            } else {
                // Another producer advanced writePos_, reload
                pos = writePos_.load(std::memory_order_relaxed);
            }
        }
    }

    // Pop (single consumer only). Returns false if queue is empty.
    bool pop(T& item) {
        // Fault injection: simulate a spurious "empty" return even when an
        // item is ready. The consumer loop must not livelock or skip the
        // item — on the next pop() call, the same item is still there.
        if (FaultInjector::instance().shouldFail("queue.pop.spurious_empty")) {
            return false;
        }
        // Sole writer: read our own position once, relaxed, and reuse it.
        const size_t rp = readPos_.load(std::memory_order_relaxed);
        Slot& slot = slots_[rp & mask_];
        size_t seq = slot.sequence.load(std::memory_order_acquire);
        intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(rp + 1);

        if (diff < 0) {
            return false; // Queue empty
        }

        item = slot.data;
        slot.sequence.store(rp + mask_ + 1, std::memory_order_release);
        readPos_.store(rp + 1, std::memory_order_release);
        return true;
    }

    bool empty() const {
        size_t rp = readPos_.load(std::memory_order_acquire);
        Slot& slot = slots_[rp & mask_];
        size_t seq = slot.sequence.load(std::memory_order_acquire);
        return static_cast<intptr_t>(seq) - static_cast<intptr_t>(rp + 1) < 0;
    }

    size_t capacity() const { return capacity_; }

    // Approximate queue depth (producer writePos - consumer readPos).
    // This is a snapshot — by the time you read it, it may have changed.
    // Useful for backpressure decisions, not for correctness.
    size_t approxSize() const {
        size_t wp = writePos_.load(std::memory_order_relaxed);
        // Acquire, not a plain read: producers call this via enqueueSafe while
        // the consumer is advancing readPos_ in pop(). A stale value here is
        // fine (the result is documented approximate) — a data race is not.
        size_t rp = readPos_.load(std::memory_order_acquire);
        return wp >= rp ? (wp - rp) : 0;
    }

private:
    struct Slot {
        T data;
        alignas(MPSC_CACHE_LINE) std::atomic<size_t> sequence;
    };
    // alignas(64) on `sequence` forces Slot's alignment to a full cache line,
    // so sizeof(Slot) is padded up to a 64-byte multiple. Each slot therefore
    // starts on its own cache line, preventing false sharing between adjacent
    // slots. This assert guards that invariant if the layout ever changes.
    static_assert(sizeof(Slot) % 64 == 0, "Slot must be cache-line aligned to prevent false sharing");

    size_t capacity_;
    size_t mask_;
    Slot* slots_;
    HugeAllocation slotsAlloc_{};  // huge-page backing for slots_ (release info)

    // Separate cache lines for write and read positions
    alignas(MPSC_CACHE_LINE) std::atomic<size_t> writePos_;
    char pad1_[MPSC_CACHE_LINE - sizeof(std::atomic<size_t>)];

    // Written only by the consumer (pop), but READ by producers: enqueueSafe
    // calls approxSize() on every submit once a backpressure threshold is
    // configured. A plain size_t therefore made `readPos_++` in pop() race with
    // those reads — a C++ data race, i.e. UB, not merely a stale value; TSan
    // reports it. It is atomic so the concurrent access is defined. Kept on its
    // own cache line to avoid false sharing with writePos_.
    //
    // Ordering: the consumer is the sole writer, so its own reads are relaxed.
    // The store is release and the cross-thread loads are acquire, which costs
    // one stlr per pop on ARM (free on x86) and gives observers a coherent view
    // rather than only a non-racy one. The ring's actual correctness protocol
    // runs through slot.sequence, not through this field.
    alignas(MPSC_CACHE_LINE) std::atomic<size_t> readPos_;
    char pad2_[MPSC_CACHE_LINE - sizeof(std::atomic<size_t>)];
};

} // namespace OrderMatcher
