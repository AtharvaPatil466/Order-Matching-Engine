#pragma once

#include "HugePageAllocator.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#ifdef __linux__
#include <sys/mman.h>  // mlock() in lockMemory()
#include <unistd.h>
#endif

namespace OrderMatcher {

struct PoolAllocationOptions {
    bool useHugePages = true;
    bool adviseTransparentHugePages = true;
    int preferredNumaNode = -1;
};

// Pre-allocated object pool with stack-based free list.
// Uses mmap on Linux so the backing region can opt into huge pages and NUMA
// placement, with debug-time live-object tracking to catch bad frees.
template <typename T>
class ObjectPool {
public:
    explicit ObjectPool(size_t size, PoolAllocationOptions options = {})
        : options_(options), capacity_(size), nextFree_(size) {
        allocatePoolStorage();
        freeList_ = static_cast<T**>(::operator new(capacity_ * sizeof(T*)));

#ifndef NDEBUG
        liveBitmapWords_ = (capacity_ + 63) / 64;
        liveBitmap_ = static_cast<uint64_t*>(::operator new(liveBitmapWords_ * sizeof(uint64_t)));
        std::memset(liveBitmap_, 0, liveBitmapWords_ * sizeof(uint64_t));
#endif

        for (size_t i = 0; i < capacity_; ++i) {
            new (&pool_[i]) T{};
            freeList_[i] = &pool_[i];
        }
    }

    ~ObjectPool() {
        for (size_t i = 0; i < capacity_; ++i) {
            pool_[i].~T();
        }

        freePoolStorage();
        ::operator delete(freeList_);

#ifndef NDEBUG
        ::operator delete(liveBitmap_);
#endif
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    T* allocate() {
        if (nextFree_ == 0) [[unlikely]] {
            return nullptr;
        }

        T* obj = freeList_[--nextFree_];
#ifndef NDEBUG
        markLive(obj, true);
#endif
        return obj;
    }

    void deallocate(T* obj) {
        if (!obj || nextFree_ >= capacity_) [[unlikely]] {
            return;
        }

#ifndef NDEBUG
        assert(isFromPool(obj) && "ObjectPool::deallocate received an out-of-pool pointer");
        assert(isLive(obj) && "ObjectPool::deallocate detected a double free or dangling pointer");
        markLive(obj, false);
#endif

        freeList_[nextFree_++] = obj;
    }

    void warmup() {
        volatile char sink = 0;
        auto* bytes = reinterpret_cast<volatile char*>(pool_);
        size_t totalBytes = capacity_ * sizeof(T);
        constexpr size_t kPageSize = 4096;
        for (size_t offset = 0; offset < totalBytes; offset += kPageSize) {
            sink = bytes[offset];
            bytes[offset] = sink;
        }
        if (totalBytes > 0) {
            sink = bytes[totalBytes - 1];
        }
        (void)sink;
    }

    bool lockMemory() {
#ifdef __linux__
        return mlock(pool_, capacity_ * sizeof(T)) == 0;
#else
        return true;
#endif
    }

    void reset() {
        nextFree_ = capacity_;
        for (size_t i = 0; i < capacity_; ++i) {
            freeList_[i] = &pool_[i];
        }

#ifndef NDEBUG
        std::memset(liveBitmap_, 0, liveBitmapWords_ * sizeof(uint64_t));
#endif
    }

    size_t capacity() const { return capacity_; }
    size_t available() const { return nextFree_; }
    bool exhausted() const { return nextFree_ == 0; }
    bool usingHugePages() const { return usedHugePages_; }

private:
    void allocatePoolStorage() {
        // Route through the shared huge-page helper (P2-15). The helper tries
        // explicit 2 MB huge pages first, then — because this pool opts into
        // THP advice / NUMA binding — a plain mmap it can madvise + mbind, then
        // the portable aligned ::operator new. The map/queue callers use the
        // same helper with defaults and fall straight to ::operator new.
        const size_t bytes = capacity_ * sizeof(T);
        HugeAllocOptions opts;
        opts.tryHugePages = options_.useHugePages;
        opts.adviseTransparentHugePages = options_.adviseTransparentHugePages;
        opts.preferredNumaNode = options_.preferredNumaNode;

        poolAlloc_ = hugeAlloc(bytes, alignof(T), opts);
        pool_ = static_cast<T*>(poolAlloc_.ptr);
        usedHugePages_ = poolAlloc_.usedHugePages;

#ifdef __linux__
        // Startup check: if explicit huge pages were requested but the kernel
        // could not honour it, diagnose the /proc/sys/vm/nr_hugepages shortfall.
        // We warn rather than abort — the 4 KB fallback is already in place and
        // correct, just with more dTLB pressure.
        if (options_.useHugePages && !usedHugePages_) {
            (void)checkHugePageReservation(bytes, /*warn=*/true);
        }
#endif
    }

    void freePoolStorage() {
        hugeFree(poolAlloc_);
    }

#ifndef NDEBUG
    bool isFromPool(const T* obj) const {
        return obj >= pool_ && obj < (pool_ + capacity_);
    }

    size_t indexOf(const T* obj) const {
        return static_cast<size_t>(obj - pool_);
    }

    bool isLive(const T* obj) const {
        size_t idx = indexOf(obj);
        return (liveBitmap_[idx / 64] & (1ULL << (idx % 64))) != 0;
    }

    void markLive(const T* obj, bool live) {
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

    PoolAllocationOptions options_{};
    T* pool_{nullptr};
    T** freeList_{nullptr};
    size_t capacity_{0};
    size_t nextFree_{0};
    bool usedHugePages_{false};
    HugeAllocation poolAlloc_{};  // huge-page backing for pool_ (release info)

#ifndef NDEBUG
    uint64_t* liveBitmap_{nullptr};
    size_t liveBitmapWords_{0};
#endif
};

} // namespace OrderMatcher
