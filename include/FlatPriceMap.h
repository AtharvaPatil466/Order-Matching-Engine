#pragma once

#include "IntrusiveList.h"
#include "Types.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace OrderMatcher {

// Two-tier flat price-level map for O(1) insert/remove/best-price access.
//
// The map spans a fixed, contiguous band of price ticks [minPrice, maxPrice]
// chosen at startup (see setRange). It is deliberately split into two tiers so
// that the hot working set stays tiny even though the addressable price band is
// large (~200K ticks):
//
//   Tier 1 — best-price bitmap (compact, always hot):
//     A two-level bitmap (bitmap0_ / bitmap1_) with one bit per price index.
//     A set bit means "this price level is active". Used for O(1)-ish best-price
//     lookup and ordered iteration. Sized by capacity but only a few words are
//     touched around the active band, so it stays resident in cache.
//
//   Tier 2 — active-level slab (a few KB in practice):
//     OrderList objects are NOT stored inline in a capacity-sized array (that
//     array would be multiple megabytes — larger than L3 — and mostly empty).
//     Instead they live in a compact slab that is grown in fixed, never-moving
//     blocks and holds ONE OrderList per *currently active* price level. Active
//     levels number in the dozens to low hundreds, so the resting-order working
//     set is a few KB rather than megabytes.
//
//     A per-price directory (slots_) maps price index -> slab handle, or
//     INVALID_SLOT when the level is inactive. Handles into the slab are stable
//     across growth (blocks never move / reallocate), so an OrderList* handed
//     out by at()/bestLevel() stays valid until that level is deactivated.
//
// Slab lifetime / recycling: a slab slot is allocated on the FIRST insert into
// a price level and returned to a free list on the LAST removal (when the level
// becomes empty). Recycled slots are always empty (a level is only deactivated
// once its OrderList is empty), so a reused handle never carries stale orders.
class FlatPriceMap {
public:
    // Default supported price band width (in price ticks). A configured
    // [minPrice, maxPrice] band must satisfy (maxPrice - minPrice) < capacity;
    // startup config validation (ConfigValidator) checks this against the real
    // limit rather than a hard-coded copy.
    static constexpr size_t DEFAULT_CAPACITY = 200001;

    explicit FlatPriceMap(Side side, size_t capacity = DEFAULT_CAPACITY)
        : side_(side), capacity_(capacity) {
        allocateStorage();
        bestIdx_ = 0;
        hasBest_ = false;
    }

    ~FlatPriceMap() {
        for (OrderList* block : slabBlocks_) {
            delete[] block;
        }
        ::operator delete(storage_, std::align_val_t{alignof(uint64_t)});
    }

    FlatPriceMap(const FlatPriceMap&) = delete;
    FlatPriceMap& operator=(const FlatPriceMap&) = delete;

    FlatPriceMap(FlatPriceMap&&) = delete;
    FlatPriceMap& operator=(FlatPriceMap&&) = delete;

    // Lazy single-argument range setup: anchors the band at minPrice and
    // derives maxPrice from the fixed capacity. Retained for the existing
    // call-site that centres the band on the first order seen.
    void setRange(Price minPrice) {
        minPrice_ = minPrice;
        maxPrice_ = minPrice + static_cast<Price>(capacity_) - 1;
        rangeSet_ = true;
    }

    // Explicit startup range configuration + validation (P1-9).
    //
    // The supported price band MUST be configured once at startup with the
    // real [minPrice, maxPrice] the venue trades — it is NOT meant to be
    // derived lazily from the first order. This overload validates that the
    // requested band actually fits the fixed FlatPriceMap capacity and rejects
    // (throws) with a clear message otherwise, so a misconfiguration fails fast
    // at init instead of silently truncating the book.
    //
    // Once configured, a price outside [minPrice, maxPrice] is a DELIBERATE
    // fat-finger-style reject via inRange() (insert() returns false and the
    // caller surfaces an explicit rejection) — it is NOT a silent capacity miss
    // or dropped order.
    void setRange(Price minPrice, Price maxPrice) {
        if (minPrice > maxPrice) {
            throw std::invalid_argument(
                "FlatPriceMap::setRange: minPrice must be <= maxPrice");
        }
        // Inclusive width == span + 1. Compare span against capacity to avoid
        // the +1 overflowing for a full-int64 span; unsigned subtraction with
        // minPrice <= maxPrice yields the exact tick distance.
        uint64_t span = static_cast<uint64_t>(maxPrice) -
                        static_cast<uint64_t>(minPrice);
        if (span >= static_cast<uint64_t>(capacity_)) {
            throw std::invalid_argument(
                "FlatPriceMap::setRange: price range exceeds map capacity");
        }
        minPrice_ = minPrice;
        maxPrice_ = maxPrice;
        rangeSet_ = true;
    }

    // True iff a range has been configured AND price sits within it. A false
    // result for an in-config-but-out-of-band price is the intended reject.
    bool inRange(Price price) const {
        return rangeSet_ && price >= minPrice_ && price <= maxPrice_;
    }

    bool rangeSet() const { return rangeSet_; }
    Price minPrice() const { return minPrice_; }
    Price maxPrice() const { return maxPrice_; }
    size_t capacity() const { return capacity_; }

    bool insert(Price price, Order* order) {
        if (!inRange(price)) {
            return false;
        }

        size_t idx = priceToIndex(price);
        if (slots_[idx] == INVALID_SLOT) {
            // First order at this price -> activate the level: grab a slab slot.
            uint32_t handle = allocSlot();
            slots_[idx] = handle;
            slabEntry(handle).push_back(order);
            ++levelCount_;
            setBit(idx);
            updateBestAfterInsert(idx);
        } else {
            slabEntry(slots_[idx]).push_back(order);
        }
        return true;
    }

    void remove(Price price, Order* order) {
        if (!inRange(price)) {
            return;
        }

        size_t idx = priceToIndex(price);
        uint32_t handle = slots_[idx];
        if (handle == INVALID_SLOT) {
            return;  // no active level here — nothing to remove
        }

        OrderList& list = slabEntry(handle);
        list.remove(order);
        if (list.empty()) {
            deactivate(idx, handle);
            if (hasBest_ && idx == bestIdx_) {
                scanForNewBest();
            }
        }
    }

    // Returns the active level at price, or nullptr when the price is inactive
    // OR its level is (transiently) empty — matching the original inline-array
    // semantics exactly. An active level is non-empty except for the brief
    // window where the caller has drained the best level but not yet called
    // eraseBest(); this keeps at()/contains() behaviour identical to before.
    OrderList* at(Price price) {
        if (!inRange(price)) {
            return nullptr;
        }
        uint32_t handle = slots_[priceToIndex(price)];
        if (handle == INVALID_SLOT) {
            return nullptr;
        }
        OrderList& list = slabEntry(handle);
        return list.empty() ? nullptr : &list;
    }

    const OrderList* at(Price price) const {
        if (!inRange(price)) {
            return nullptr;
        }
        uint32_t handle = slots_[priceToIndex(price)];
        if (handle == INVALID_SLOT) {
            return nullptr;
        }
        const OrderList& list = slabEntry(handle);
        return list.empty() ? nullptr : &list;
    }

    bool contains(Price price) const {
        if (!inRange(price)) {
            return false;
        }
        uint32_t handle = slots_[priceToIndex(price)];
        return handle != INVALID_SLOT && !slabEntry(handle).empty();
    }

    Price bestPrice() const {
        if (!hasBest_) {
            return side_ == Side::Buy ? 0 : std::numeric_limits<Price>::max();
        }
        return indexToPrice(bestIdx_);
    }

    OrderList* bestLevel() {
        return hasBest_ ? &slabEntry(slots_[bestIdx_]) : nullptr;
    }

    void eraseBest() {
        if (!hasBest_) {
            return;
        }
        uint32_t handle = slots_[bestIdx_];
        // Only erase a best level that the caller has already drained empty.
        if (handle == INVALID_SLOT || !slabEntry(handle).empty()) {
            return;
        }

        deactivate(bestIdx_, handle);
        scanForNewBest();
    }

    bool empty() const { return levelCount_ == 0; }
    size_t size() const { return levelCount_; }

    template <typename Fn>
    void forEachLevel(Fn&& fn) {
        forEachLevelImpl(std::forward<Fn>(fn));
    }

    template <typename Fn>
    void forEachLevel(Fn&& fn) const {
        forEachLevelImpl(std::forward<Fn>(fn));
    }

    template <typename Fn>
    void forEachLevelWhile(Fn&& fn) {
        forEachLevelWhileImpl(std::forward<Fn>(fn));
    }

    template <typename Fn>
    void forEachLevelWhile(Fn&& fn) const {
        forEachLevelWhileImpl(std::forward<Fn>(fn));
    }

private:
    // Slab blocks are fixed-size and never moved once allocated, so handles and
    // OrderList pointers stay valid across growth. Power-of-two size lets the
    // handle -> (block, offset) split compile to a shift + mask.
    static constexpr uint32_t SLAB_BLOCK = 256;
    static constexpr uint32_t INVALID_SLOT = std::numeric_limits<uint32_t>::max();

    template <typename Fn>
    void forEachLevelImpl(Fn&& fn) const {
        if (levelCount_ == 0 || !hasBest_) {
            return;
        }

        if (side_ == Side::Buy) {
            iterateDescending(bestIdx_, [&](size_t idx) {
                fn(indexToPrice(idx), levelAt(idx));
                return true;
            });
        } else {
            iterateAscending(bestIdx_, [&](size_t idx) {
                fn(indexToPrice(idx), levelAt(idx));
                return true;
            });
        }
    }

    template <typename Fn>
    void forEachLevelWhileImpl(Fn&& fn) const {
        if (levelCount_ == 0 || !hasBest_) {
            return;
        }

        if (side_ == Side::Buy) {
            iterateDescending(bestIdx_, [&](size_t idx) {
                return fn(indexToPrice(idx), levelAt(idx));
            });
        } else {
            iterateAscending(bestIdx_, [&](size_t idx) {
                return fn(indexToPrice(idx), levelAt(idx));
            });
        }
    }

    // Directory + both bitmap tiers share one contiguous allocation. The
    // OrderList objects themselves live in the separately-grown slab, so this
    // block is only ~(4 bytes/tick + bitmap) instead of sizeof(OrderList)/tick.
    void allocateStorage() {
        bitmap0Words_ = (capacity_ + 63) / 64;
        bitmap1Words_ = (bitmap0Words_ + 63) / 64;

        size_t slotBytes = sizeof(uint32_t) * capacity_;
        size_t bitmap0Bytes = sizeof(uint64_t) * bitmap0Words_;
        size_t bitmap1Bytes = sizeof(uint64_t) * bitmap1Words_;
        size_t alignPad = alignof(uint64_t) - 1;
        size_t totalBytes = slotBytes + alignPad + bitmap0Bytes + bitmap1Bytes;

        storage_ = static_cast<std::byte*>(
            ::operator new(totalBytes, std::align_val_t{alignof(uint64_t)}));

        std::byte* cursor = storage_;
        slots_ = reinterpret_cast<uint32_t*>(cursor);
        // All price levels start inactive (no slab slot assigned).
        std::memset(slots_, 0xFF, slotBytes);  // 0xFFFFFFFF == INVALID_SLOT

        cursor += slotBytes;
        uintptr_t aligned = (reinterpret_cast<uintptr_t>(cursor) + alignPad) &
                            ~static_cast<uintptr_t>(alignPad);
        cursor = reinterpret_cast<std::byte*>(aligned);
        bitmap0_ = reinterpret_cast<uint64_t*>(cursor);
        std::memset(bitmap0_, 0, bitmap0Bytes);

        cursor += bitmap0Bytes;
        bitmap1_ = reinterpret_cast<uint64_t*>(cursor);
        std::memset(bitmap1_, 0, bitmap1Bytes);
    }

    // ── Slab (Tier 2) ────────────────────────────────────────────────────────
    OrderList& slabEntry(uint32_t handle) const {
        return slabBlocks_[handle / SLAB_BLOCK][handle % SLAB_BLOCK];
    }

    OrderList& levelAt(size_t idx) const { return slabEntry(slots_[idx]); }

    uint32_t allocSlot() {
        if (!freeSlots_.empty()) {
            uint32_t handle = freeSlots_.back();
            freeSlots_.pop_back();
            // Recycled slots are always empty (levels deactivate only when
            // empty), so there is no stale order state to clear.
            assert(slabEntry(handle).empty());
            return handle;
        }
        uint32_t handle = slabHighWater_++;
        if (handle / SLAB_BLOCK >= slabBlocks_.size()) {
            slabBlocks_.push_back(new OrderList[SLAB_BLOCK]);
        }
        return handle;
    }

    // Deactivate a price level: recycle its (now-empty) slab slot, clear the
    // directory entry and the bitmap bit. Does NOT rescan for a new best; the
    // caller decides whether the best pointer was affected.
    void deactivate(size_t idx, uint32_t handle) {
        assert(slabEntry(handle).empty());
        slots_[idx] = INVALID_SLOT;
        freeSlots_.push_back(handle);
        --levelCount_;
        clearBit(idx);
    }

    template <typename Fn>
    void iterateAscending(size_t startIdx, Fn&& fn) const {
        size_t l0Word = startIdx / 64;
        size_t bitOffset = startIdx % 64;
        size_t l1Word = l0Word / 64;
        size_t l1Bit = l0Word % 64;

        while (l1Word < bitmap1Words_) {
            uint64_t l1Mask = bitmap1_[l1Word];
            if (l1Word == (startIdx / 64) / 64) {
                l1Mask &= (~0ULL << l1Bit);
            }

            while (l1Mask != 0) {
                size_t subWord = static_cast<size_t>(__builtin_ctzll(l1Mask));
                size_t wordIdx = l1Word * 64 + subWord;
                if (wordIdx >= bitmap0Words_) {
                    return;
                }

                uint64_t word = bitmap0_[wordIdx];
                if (wordIdx == l0Word) {
                    word &= (~0ULL << bitOffset);
                }

                while (word != 0) {
                    size_t bit = static_cast<size_t>(__builtin_ctzll(word));
                    size_t idx = wordIdx * 64 + bit;
                    if (idx >= capacity_) {
                        return;
                    }
                    if (!fn(idx)) {
                        return;
                    }
                    word &= (word - 1);
                }

                l1Mask &= (l1Mask - 1);
                bitOffset = 0;
            }

            ++l1Word;
            l1Bit = 0;
        }
    }

    template <typename Fn>
    void iterateDescending(size_t startIdx, Fn&& fn) const {
        size_t l0Word = startIdx / 64;
        size_t bitOffset = startIdx % 64;
        size_t l1Word = l0Word / 64;
        size_t l1Bit = l0Word % 64;

        while (true) {
            uint64_t l1Mask = bitmap1_[l1Word];
            if (l1Word == (startIdx / 64) / 64) {
                l1Mask &= (l1Bit == 63 ? ~0ULL : ((1ULL << (l1Bit + 1)) - 1));
            }

            while (l1Mask != 0) {
                size_t subWord = 63U - static_cast<size_t>(__builtin_clzll(l1Mask));
                size_t wordIdx = l1Word * 64 + subWord;
                if (wordIdx >= bitmap0Words_) {
                    l1Mask ^= (1ULL << subWord);
                    continue;
                }

                uint64_t word = bitmap0_[wordIdx];
                if (wordIdx == l0Word) {
                    word &= (bitOffset == 63 ? ~0ULL : ((1ULL << (bitOffset + 1)) - 1));
                }

                while (word != 0) {
                    size_t bit = 63U - static_cast<size_t>(__builtin_clzll(word));
                    size_t idx = wordIdx * 64 + bit;
                    if (!fn(idx)) {
                        return;
                    }
                    word &= ~(1ULL << bit);
                }

                l1Mask &= ~(1ULL << subWord);
                bitOffset = 63;
            }

            if (l1Word == 0) {
                break;
            }
            --l1Word;
            l1Bit = 63;
        }
    }

    size_t priceToIndex(Price price) const {
        return static_cast<size_t>(price - minPrice_);
    }

    Price indexToPrice(size_t idx) const {
        return minPrice_ + static_cast<Price>(idx);
    }

    void setBit(size_t idx) {
        size_t w0 = idx / 64;
        size_t b0 = idx % 64;
        bitmap0_[w0] |= (1ULL << b0);

        size_t w1 = w0 / 64;
        size_t b1 = w0 % 64;
        bitmap1_[w1] |= (1ULL << b1);
    }

    void clearBit(size_t idx) {
        size_t w0 = idx / 64;
        size_t b0 = idx % 64;
        bitmap0_[w0] &= ~(1ULL << b0);
        if (bitmap0_[w0] == 0) {
            size_t w1 = w0 / 64;
            size_t b1 = w0 % 64;
            bitmap1_[w1] &= ~(1ULL << b1);
        }
    }

    void updateBestAfterInsert(size_t idx) {
        if (!hasBest_) {
            bestIdx_ = idx;
            hasBest_ = true;
            return;
        }

        if (side_ == Side::Buy) {
            if (idx > bestIdx_) {
                bestIdx_ = idx;
            }
        } else if (idx < bestIdx_) {
            bestIdx_ = idx;
        }
    }

    void scanForNewBest() {
        if (levelCount_ == 0) {
            hasBest_ = false;
            return;
        }

        size_t newBest = bestIdx_;
        bool found = false;

        if (side_ == Side::Buy) {
            iterateDescending(capacity_ - 1, [&](size_t idx) {
                newBest = idx;
                found = true;
                return false;
            });
        } else {
            iterateAscending(0, [&](size_t idx) {
                newBest = idx;
                found = true;
                return false;
            });
        }

        bestIdx_ = newBest;
        hasBest_ = found;
    }

    Side side_;
    size_t capacity_;
    uint32_t* slots_{nullptr};      // price index -> slab handle (or INVALID_SLOT)
    uint64_t* bitmap0_{nullptr};
    uint64_t* bitmap1_{nullptr};
    std::byte* storage_{nullptr};   // backing block for slots_ + both bitmaps
    std::vector<OrderList*> slabBlocks_;  // never-moving Tier-2 slab blocks
    std::vector<uint32_t> freeSlots_;     // recycled slab handles
    uint32_t slabHighWater_{0};     // next never-used slab handle
    size_t bitmap0Words_{0};
    size_t bitmap1Words_{0};
    size_t levelCount_{0};
    size_t bestIdx_{0};
    bool hasBest_{false};
    bool rangeSet_{false};
    Price minPrice_{0};
    Price maxPrice_{0};
};

} // namespace OrderMatcher
