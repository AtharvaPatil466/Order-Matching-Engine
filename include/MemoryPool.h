#pragma once

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
#ifdef __linux__
        size_t bytes = capacity_ * sizeof(T);
        int mmapFlags = MAP_PRIVATE | MAP_ANONYMOUS;
        if (options_.useHugePages) {
#ifdef MAP_HUGETLB
            void* huge = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                              mmapFlags | MAP_HUGETLB, -1, 0);
            if (huge != MAP_FAILED) {
                pool_ = static_cast<T*>(huge);
                mmapBytes_ = bytes;
                usedHugePages_ = true;
            }
#endif
        }

        if (!pool_) {
            void* normal = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, mmapFlags, -1, 0);
            if (normal != MAP_FAILED) {
                pool_ = static_cast<T*>(normal);
                mmapBytes_ = bytes;
            }
        }

        if (pool_) {
#ifdef MADV_HUGEPAGE
            if (!usedHugePages_ && options_.adviseTransparentHugePages) {
                (void)madvise(pool_, mmapBytes_, MADV_HUGEPAGE);
            }
#endif
            if (options_.preferredNumaNode >= 0) {
                bindToNode(pool_, mmapBytes_, options_.preferredNumaNode);
            }
            return;
        }
#endif

        pool_ = static_cast<T*>(::operator new(capacity_ * sizeof(T),
                                               std::align_val_t{alignof(T)}));
    }

    void freePoolStorage() {
#ifdef __linux__
        if (mmapBytes_ != 0) {
            munmap(pool_, mmapBytes_);
            return;
        }
#endif
        ::operator delete(pool_, std::align_val_t{alignof(T)});
    }

#ifdef __linux__
    static void bindToNode(void* addr, size_t bytes, int node) {
#ifdef SYS_mbind
        unsigned long mask = 1UL << static_cast<unsigned long>(node);
        long rc = syscall(SYS_mbind, addr, bytes, MPOL_BIND, &mask,
                          sizeof(mask) * 8, 0);
        (void)rc;
#else
        (void)addr;
        (void)bytes;
        (void)node;
#endif
    }
#endif

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

#ifdef __linux__
    size_t mmapBytes_{0};
#endif

#ifndef NDEBUG
    uint64_t* liveBitmap_{nullptr};
    size_t liveBitmapWords_{0};
#endif
};

} // namespace OrderMatcher
