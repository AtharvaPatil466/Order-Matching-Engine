#pragma once

// HugePageAllocator — one place that backs a byte range with 2 MB huge pages
// (Linux) and degrades cleanly everywhere else (P2-15).
//
// Why huge pages: the hot data structures (FlatPriceMap directory + bitmaps,
// the resting-order slab, the Order pool, and the MPSC ring buffers) are large
// enough that 4 KB paging costs real dTLB misses on the matching hot path. A
// 2 MB page maps 512x the address range per TLB entry, so the whole working set
// stays covered by a handful of TLB entries.
//
// Design: a single allocator helper (hugeAlloc / hugeFree) that on Linux tries
//   mmap(..., MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB|MAP_HUGE_2MB, ...)
// and on failure (or non-Linux) falls back to the caller's historical
// allocation (an aligned ::operator new). An opt-in plain-mmap fallback exists
// for callers that also want transparent-huge-page advice or NUMA binding
// (the Order pool); callers that don't (FlatPriceMap, MpscQueue) fall straight
// through to ::operator new so their fallback matches what they used before.
//
// IMPORTANT (deployment): Transparent Huge Pages (THP) MUST be disabled on the
// production host — `echo never > /sys/kernel/mm/transparent_hugepage/enabled`.
// THP background compaction (khugepaged) can stall a core for 10-100 ms, which
// dwarfs the ns-scale matching cost. Explicit huge pages via MAP_HUGETLB are
// reserved up-front (see checkHugePageReservation) and are NOT subject to
// khugepaged compaction, so they give the TLB win without the pause risk.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>

#ifdef __linux__
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/mempolicy.h>
#endif

namespace OrderMatcher {

// Explicit 2 MB huge page size. MAP_HUGE_2MB pins this even on hosts whose
// default huge page size is 1 GB.
inline constexpr size_t HUGE_PAGE_SIZE_2MB = 2ULL * 1024 * 1024;

// Round a byte count up to a whole number of 2 MB huge pages. mmap(MAP_HUGETLB)
// requires the length to be a multiple of the huge page size.
inline size_t roundUpToHugePage(size_t bytes) {
    return (bytes + HUGE_PAGE_SIZE_2MB - 1) & ~(HUGE_PAGE_SIZE_2MB - 1);
}

// Per-allocation tuning. Defaults suit the map/queue callers: try explicit huge
// pages, no THP advice, no NUMA pinning.
struct HugeAllocOptions {
    bool tryHugePages = true;                 // attempt MAP_HUGETLB first
    bool adviseTransparentHugePages = false;  // madvise(MADV_HUGEPAGE) on the mmap fallback
    int preferredNumaNode = -1;               // mbind() to this node when >= 0
};

// Book-keeping needed to release an allocation regardless of which path served
// it. mmapBytes > 0 means "release with munmap"; mmapBytes == 0 means the
// ::operator new fallback was used and it must be freed with ::operator delete.
struct HugeAllocation {
    void* ptr = nullptr;
    size_t mmapBytes = 0;
    size_t alignment = 0;
    bool usedHugePages = false;
};

#ifdef __linux__
// Pin a mapping to a single NUMA node. Best-effort: mbind failure is ignored
// (the mapping is still valid, just not node-local).
inline void hugeBindNumaNode(void* addr, size_t bytes, int node) {
#ifdef SYS_mbind
    unsigned long mask = 1UL << static_cast<unsigned long>(node);
    (void)syscall(SYS_mbind, addr, bytes, MPOL_BIND, &mask, sizeof(mask) * 8, 0);
#else
    (void)addr;
    (void)bytes;
    (void)node;
#endif
}
#endif

// Allocate `bytes` backed by 2 MB huge pages on Linux, falling back to an
// aligned ::operator new on failure or on non-Linux platforms. `alignment` is
// applied to the ::operator new fallback (mmap regions are already page
// aligned, comfortably beyond any scalar alignment).
inline HugeAllocation hugeAlloc(size_t bytes,
                                size_t alignment = alignof(std::max_align_t),
                                HugeAllocOptions options = {}) {
    HugeAllocation out;
    out.alignment = alignment;
    (void)options;  // unused on the portable path; referenced under __linux__

    if (bytes == 0) {
        // Preserve ::operator new(0) semantics (unique, freeable pointer).
        out.ptr = ::operator new(std::size_t{0}, std::align_val_t{alignment});
        return out;
    }

#ifdef __linux__
    const int baseFlags = MAP_PRIVATE | MAP_ANONYMOUS;

#ifdef MAP_HUGETLB
    if (options.tryHugePages) {
        int hugeFlags = baseFlags | MAP_HUGETLB;
#ifdef MAP_HUGE_2MB
        hugeFlags |= MAP_HUGE_2MB;
#endif
        size_t mapBytes = roundUpToHugePage(bytes);
        void* p = mmap(nullptr, mapBytes, PROT_READ | PROT_WRITE, hugeFlags, -1, 0);
        if (p != MAP_FAILED) {
            out.ptr = p;
            out.mmapBytes = mapBytes;
            out.usedHugePages = true;
            if (options.preferredNumaNode >= 0) {
                hugeBindNumaNode(p, mapBytes, options.preferredNumaNode);
            }
            return out;
        }
    }
#endif  // MAP_HUGETLB

    // Plain-mmap fallback is only taken when the caller wants an mmap-only
    // feature (THP advice or NUMA binding). Callers that want neither (the map
    // and queue) fall through to ::operator new so their fallback matches the
    // allocation they used before P2-15.
    if (options.adviseTransparentHugePages || options.preferredNumaNode >= 0) {
        void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, baseFlags, -1, 0);
        if (p != MAP_FAILED) {
            out.ptr = p;
            out.mmapBytes = bytes;
#ifdef MADV_HUGEPAGE
            if (options.adviseTransparentHugePages) {
                (void)madvise(p, bytes, MADV_HUGEPAGE);
            }
#endif
            if (options.preferredNumaNode >= 0) {
                hugeBindNumaNode(p, bytes, options.preferredNumaNode);
            }
            return out;
        }
    }
#endif  // __linux__

    // Portable fallback: the historical aligned ::operator new.
    out.ptr = ::operator new(bytes, std::align_val_t{alignment});
    return out;
}

// Release an allocation obtained from hugeAlloc. Safe on a default-constructed
// (null) HugeAllocation.
inline void hugeFree(const HugeAllocation& a) {
    if (a.ptr == nullptr) {
        return;
    }
#ifdef __linux__
    if (a.mmapBytes != 0) {
        munmap(a.ptr, a.mmapBytes);
        return;
    }
#endif
    ::operator delete(a.ptr, std::align_val_t{a.alignment});
}

// Total 2 MB huge pages reserved on the host (/proc/sys/vm/nr_hugepages), or -1
// when the value is unavailable (non-Linux, or the file cannot be read).
inline long readNrHugePages() {
#ifdef __linux__
    std::FILE* f = std::fopen("/proc/sys/vm/nr_hugepages", "r");
    if (f == nullptr) {
        return -1;
    }
    long n = -1;
    if (std::fscanf(f, "%ld", &n) != 1) {
        n = -1;
    }
    std::fclose(f);
    return n;
#else
    return -1;
#endif
}

struct HugePageCheck {
    long reservedPages = -1;  // nr_hugepages, or -1 if unknown
    long neededPages = 0;     // 2 MB pages required for the working set
    bool sufficient = true;   // true on non-Linux / when the reservation covers need
};

// Startup check: does the host have enough reserved 2 MB pages for a working
// set of `workingSetBytes`? On non-Linux this is a no-op reporting sufficient.
// On Linux, when the reservation is short it returns sufficient=false and (when
// `warn`) prints a one-line diagnosis to stderr so startup can fail fast or
// degrade to the 4 KB fallback with an audit trail rather than silently.
inline HugePageCheck checkHugePageReservation(size_t workingSetBytes, bool warn = true) {
    HugePageCheck c;
    c.neededPages =
        static_cast<long>(roundUpToHugePage(workingSetBytes) / HUGE_PAGE_SIZE_2MB);
    (void)warn;  // unused on the non-Linux path
#ifdef __linux__
    c.reservedPages = readNrHugePages();
    if (c.reservedPages < 0) {
        // Can't read the reservation — assume the allocation fallback copes.
        c.sufficient = true;
        return c;
    }
    c.sufficient = c.reservedPages >= c.neededPages;
    if (!c.sufficient && warn) {
        std::fprintf(stderr,
                     "[hugepages] WARNING: /proc/sys/vm/nr_hugepages=%ld but the "
                     "configured working set needs %ld x 2MB pages. Huge-page "
                     "allocations will fall back to 4KB pages (correct, but with "
                     "more dTLB pressure). Reserve with: "
                     "sysctl -w vm.nr_hugepages=%ld\n",
                     c.reservedPages, c.neededPages, c.neededPages);
    }
#endif
    return c;
}

} // namespace OrderMatcher
