#pragma once

#include "Order.h"
#include "FlatHashMap.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#ifdef __linux__
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/mempolicy.h>
#endif

namespace OrderMatcher {

// Price-level-aware arena allocator for Order nodes.
//
// WHY: a plain ObjectPool hands out slots in allocation order, so orders that
// rest at the same price level end up scattered across the pool. Walking the
// intrusive OrderList at a price level then chases pointers across cold cache
// lines (~47 L1-dcache-misses/order measured on x86). This arena groups orders
// by their (incoming) limit price into contiguous SLAB_SIZE-slot slabs, so a
// price level's resting orders are physically adjacent and the matching loop's
// list walk stays warm in cache.
//
// IDENTITY MODEL (unchanged from ObjectPool): a slot's address is fixed from
// allocate() to deallocate(). Orders are never relocated, so every Order* held
// elsewhere (orderLookup_, the intrusive list links, the 27 OrderBook call
// sites, replication) stays valid. This is purely a memory-PLACEMENT change —
// it does not touch ordering, so FIFO_Preservation / NoNegativeQuantity /
// GTD_Expiry_Correctness are bit-for-bit identical.
//
// O(1): allocate is a hash lookup (price -> slab) + a slot pop; deallocate is
// index math (slot -> slab) + a slot push, with O(1) slab-stack maintenance via
// swap-remove. No linear scans. No heap allocation on the hot path: storage,
// per-slab metadata, and the slab stacks are all pre-allocated at construction.
//
// FALLBACK / EXHAUSTION: true exhaustion is totalLive_ == capacity_ (identical
// to ObjectPool's "all slots used"). When a price has no free slab AND the
// free-slab stack is empty but slots remain (high price-diversity), allocate
// falls back to any PARTIAL slab — a locality miss, never a rejection — so the
// CapacityExhausted reject path fires only when genuinely out of slots.
// Slots per slab. Default 64 (64 * 192B = 12KB, ~3 pages). Overridable at build
// time with -DOB_SLAB_SIZE=<N> (scripts/aws_benchmark.sh sweeps K=32/64/128 to
// tune the locality/fragmentation trade-off) without editing this file.
#ifndef OB_SLAB_SIZE
#define OB_SLAB_SIZE 64
#endif

class OrderArena {
public:
    static constexpr size_t SLAB_SIZE = OB_SLAB_SIZE;

    explicit OrderArena(size_t capacity)
        : capacity_(capacity),
          slabCount_(static_cast<uint32_t>((capacity + SLAB_SIZE - 1) / SLAB_SIZE)),
          // Sized so every slab can have its own active price without rehashing
          // on the hot path (max distinct active prices <= slabCount_).
          activePriceToSlab_(slabCount_ + 1) {
        allocatePoolStorage();

        for (size_t i = 0; i < capacity_; ++i) {
            new (&pool_[i]) Order{};
        }

        slabs_        = static_cast<Slab*>(::operator new(slabCount_ * sizeof(Slab)));
        freeStack_    = static_cast<uint32_t*>(::operator new(slabCount_ * sizeof(uint32_t)));
        partialStack_ = static_cast<uint32_t*>(::operator new(slabCount_ * sizeof(uint32_t)));

        for (uint32_t s = 0; s < slabCount_; ++s) {
            size_t base = static_cast<size_t>(s) * SLAB_SIZE;
            slabs_[s] = Slab{};
            slabs_[s].base = static_cast<uint32_t>(base);
            slabs_[s].cap  = static_cast<uint32_t>(
                std::min(SLAB_SIZE, capacity_ - base));
            slabs_[s].where = Where::Free;
            slabs_[s].pos   = s;          // position in freeStack_
            freeStack_[s]   = s;
        }
        freeTop_ = slabCount_;

#ifndef NDEBUG
        liveBitmapWords_ = (capacity_ + 63) / 64;
        liveBitmap_ = static_cast<uint64_t*>(::operator new(liveBitmapWords_ * sizeof(uint64_t)));
        std::memset(liveBitmap_, 0, liveBitmapWords_ * sizeof(uint64_t));
#endif
    }

    ~OrderArena() {
        for (size_t i = 0; i < capacity_; ++i) {
            pool_[i].~Order();
        }
        freePoolStorage();
        ::operator delete(slabs_);
        ::operator delete(freeStack_);
        ::operator delete(partialStack_);
#ifndef NDEBUG
        ::operator delete(liveBitmap_);
#endif
    }

    OrderArena(const OrderArena&) = delete;
    OrderArena& operator=(const OrderArena&) = delete;

    // Allocate a slot, preferring the slab affiliated with `priceHint` so that
    // orders resting at the same price land contiguously. The hint only steers
    // placement; any hint yields a valid slot (or nullptr when truly full).
    Order* allocate(Price priceHint) {
        if (totalLive_ == capacity_) [[unlikely]] {
            return nullptr;
        }

        uint32_t s;
        uint32_t* mapped = activePriceToSlab_.find(priceHint);
        if (mapped && slabs_[*mapped].live < slabs_[*mapped].cap) {
            s = *mapped;                       // price's current slab has room
        } else if (freeTop_ > 0) {
            s = freeStack_[freeTop_ - 1];      // claim a fresh empty slab for this price
            slabs_[s].price = priceHint;
            activePriceToSlab_.insert(priceHint, s);
        } else {
            // No empty slab but slots remain: fall back to any partial slab.
            // A pure locality miss — correctness preserved, never a rejection.
            s = partialStack_[partialTop_ - 1];
            slabs_[s].price = priceHint;
            activePriceToSlab_.insert(priceHint, s);
        }

        Order* obj = popSlot(s);
#ifndef NDEBUG
        markLive(obj, true);
#endif
        return obj;
    }

    void deallocate(Order* obj) {
        if (!obj) [[unlikely]] {
            return;
        }
#ifndef NDEBUG
        assert(isFromPool(obj) && "OrderArena::deallocate received an out-of-pool pointer");
        assert(isLive(obj) && "OrderArena::deallocate detected a double free or dangling pointer");
        markLive(obj, false);
#endif
        uint32_t idx = static_cast<uint32_t>(obj - pool_);
        uint32_t s   = idx / static_cast<uint32_t>(SLAB_SIZE);

        // Push the freed slot onto its owning slab's intrusive free list. Reusing
        // Order::next is safe: OrderList::remove() nulls next/prev before any
        // deallocate, so a freed slot is never reachable from a live list.
        obj->next = slabs_[s].freeHead;
        slabs_[s].freeHead = obj;

        uint32_t before = slabs_[s].live--;
        --totalLive_;

        if (slabs_[s].live == 0) {
            transition(s, Where::Free);
            // Drop the price->slab affinity only if it still points here (a later
            // slab may have taken over this price after this one filled up).
            uint32_t* m = activePriceToSlab_.find(slabs_[s].price);
            if (m && *m == s) {
                activePriceToSlab_.erase(slabs_[s].price);
            }
        } else if (before == slabs_[s].cap) {
            transition(s, Where::Partial);  // was Full -> now has room
        }
    }

    size_t capacity()  const { return capacity_; }
    size_t available() const { return capacity_ - totalLive_; }
    bool   exhausted() const { return totalLive_ == capacity_; }

private:
    enum class Where : uint8_t { Free = 0, Partial = 1, Full = 2 };

    struct Slab {
        Order*   freeHead = nullptr;  // returned slots, chained via Order::next
        uint32_t hw    = 0;           // high-water: [base+hw, base+cap) never handed out
        uint32_t live  = 0;           // currently-allocated slots
        uint32_t cap   = 0;           // slots in this slab (SLAB_SIZE, or remainder)
        uint32_t base  = 0;           // index of first slot in pool_
        uint32_t pos   = 0;           // index within freeStack_/partialStack_
        Price    price = 0;           // price currently affiliated (when not Free)
        Where    where = Where::Free;
    };

    // Pop a slot from slab `s` (precondition: live < cap) and maintain stacks.
    Order* popSlot(uint32_t s) {
        Slab& sl = slabs_[s];
        Order* obj;
        if (sl.freeHead) {
            obj = sl.freeHead;
            sl.freeHead = obj->next;
        } else {
            obj = &pool_[sl.base + sl.hw];
            ++sl.hw;
        }
        uint32_t before = sl.live++;
        ++totalLive_;
        if (sl.live == sl.cap) {
            transition(s, Where::Full);
        } else if (before == 0) {
            transition(s, Where::Partial);  // was Free -> now Partial
        }
        return obj;
    }

    // Move slab `s` to a new residence stack (Free / Partial / Full) in O(1)
    // via swap-remove from its current stack. Full slabs live in no stack.
    void transition(uint32_t s, Where to) {
        Where from = slabs_[s].where;
        if (from == to) {
            return;
        }
        if (from == Where::Free) {
            stackRemove(freeStack_, freeTop_, s);
        } else if (from == Where::Partial) {
            stackRemove(partialStack_, partialTop_, s);
        }
        if (to == Where::Free) {
            stackPush(freeStack_, freeTop_, s);
        } else if (to == Where::Partial) {
            stackPush(partialStack_, partialTop_, s);
        }
        slabs_[s].where = to;
    }

    void stackPush(uint32_t* stack, size_t& top, uint32_t s) {
        stack[top] = s;
        slabs_[s].pos = static_cast<uint32_t>(top);
        ++top;
    }

    void stackRemove(uint32_t* stack, size_t& top, uint32_t s) {
        uint32_t p = slabs_[s].pos;
        uint32_t moved = stack[--top];
        stack[p] = moved;
        slabs_[moved].pos = p;
    }

    // ─── Pool storage (ported faithfully from MemoryPool.h to preserve the
    //     hugepage/NUMA placement that matters for x86 TLB/cache behaviour) ───
    void allocatePoolStorage() {
#ifdef __linux__
        size_t bytes = capacity_ * sizeof(Order);
        int mmapFlags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_HUGETLB
        void* huge = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                          mmapFlags | MAP_HUGETLB, -1, 0);
        if (huge != MAP_FAILED) {
            pool_ = static_cast<Order*>(huge);
            mmapBytes_ = bytes;
        }
#endif
        if (!pool_) {
            void* normal = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, mmapFlags, -1, 0);
            if (normal != MAP_FAILED) {
                pool_ = static_cast<Order*>(normal);
                mmapBytes_ = bytes;
            }
        }
        if (pool_) {
#ifdef MADV_HUGEPAGE
            (void)madvise(pool_, mmapBytes_, MADV_HUGEPAGE);
#endif
            return;
        }
#endif
        pool_ = static_cast<Order*>(::operator new(capacity_ * sizeof(Order),
                                                   std::align_val_t{alignof(Order)}));
    }

    void freePoolStorage() {
#ifdef __linux__
        if (mmapBytes_ != 0) {
            munmap(pool_, mmapBytes_);
            return;
        }
#endif
        ::operator delete(pool_, std::align_val_t{alignof(Order)});
    }

#ifndef NDEBUG
    bool isFromPool(const Order* obj) const {
        return obj >= pool_ && obj < (pool_ + capacity_);
    }
    size_t indexOf(const Order* obj) const {
        return static_cast<size_t>(obj - pool_);
    }
    bool isLive(const Order* obj) const {
        size_t idx = indexOf(obj);
        return (liveBitmap_[idx / 64] & (1ULL << (idx % 64))) != 0;
    }
    void markLive(const Order* obj, bool live) {
        size_t idx = indexOf(obj);
        uint64_t mask = 1ULL << (idx % 64);
        uint64_t& word = liveBitmap_[idx / 64];
        if (live) {
            word |= mask;
        } else {
            word &= ~mask;
        }
    }
#endif

    Order*   pool_{nullptr};
    size_t   capacity_{0};
    uint32_t slabCount_{0};

    Slab*     slabs_{nullptr};
    uint32_t* freeStack_{nullptr};     // slab ids with live == 0
    size_t    freeTop_{0};
    uint32_t* partialStack_{nullptr};  // slab ids with 0 < live < cap
    size_t    partialTop_{0};

    size_t    totalLive_{0};
    FlatHashMap<Price, uint32_t> activePriceToSlab_;

#ifdef __linux__
    size_t mmapBytes_{0};
#endif

#ifndef NDEBUG
    uint64_t* liveBitmap_{nullptr};
    size_t    liveBitmapWords_{0};
#endif
};

} // namespace OrderMatcher
