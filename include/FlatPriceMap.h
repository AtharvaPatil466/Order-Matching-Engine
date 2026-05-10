#pragma once

#include "IntrusiveList.h"
#include "Types.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace OrderMatcher {

// Flat array-based price level map for O(1) insert/remove/best-price access.
// Levels and both bitmap tiers are allocated in one contiguous block at
// construction to avoid fragmented heap allocations.
class FlatPriceMap {
public:
    explicit FlatPriceMap(Side side, size_t capacity = 200001)
        : side_(side), capacity_(capacity) {
        allocateStorage();
        bestIdx_ = 0;
        hasBest_ = false;
    }

    ~FlatPriceMap() {
        if (levels_) {
            for (size_t i = 0; i < capacity_; ++i) {
                levels_[i].~OrderList();
            }
        }
        ::operator delete(storage_, std::align_val_t{alignof(OrderList)});
    }

    FlatPriceMap(const FlatPriceMap&) = delete;
    FlatPriceMap& operator=(const FlatPriceMap&) = delete;

    FlatPriceMap(FlatPriceMap&&) = delete;
    FlatPriceMap& operator=(FlatPriceMap&&) = delete;

    void setRange(Price minPrice) {
        minPrice_ = minPrice;
        maxPrice_ = minPrice + static_cast<Price>(capacity_) - 1;
        rangeSet_ = true;
    }

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
        bool wasEmpty = levels_[idx].empty();
        levels_[idx].push_back(order);
        if (wasEmpty) {
            ++levelCount_;
            setBit(idx);
            updateBestAfterInsert(idx);
        }
        return true;
    }

    void remove(Price price, Order* order) {
        if (!inRange(price)) {
            return;
        }

        size_t idx = priceToIndex(price);
        levels_[idx].remove(order);
        if (levels_[idx].empty()) {
            --levelCount_;
            clearBit(idx);
            if (hasBest_ && idx == bestIdx_) {
                scanForNewBest();
            }
        }
    }

    OrderList* at(Price price) {
        if (!inRange(price)) {
            return nullptr;
        }
        size_t idx = priceToIndex(price);
        return levels_[idx].empty() ? nullptr : &levels_[idx];
    }

    const OrderList* at(Price price) const {
        if (!inRange(price)) {
            return nullptr;
        }
        size_t idx = priceToIndex(price);
        return levels_[idx].empty() ? nullptr : &levels_[idx];
    }

    bool contains(Price price) const {
        return inRange(price) && !levels_[priceToIndex(price)].empty();
    }

    Price bestPrice() const {
        if (!hasBest_) {
            return side_ == Side::Buy ? 0 : std::numeric_limits<Price>::max();
        }
        return indexToPrice(bestIdx_);
    }

    OrderList* bestLevel() {
        return hasBest_ ? &levels_[bestIdx_] : nullptr;
    }

    void eraseBest() {
        if (!hasBest_ || !levels_[bestIdx_].empty()) {
            return;
        }

        --levelCount_;
        clearBit(bestIdx_);
        scanForNewBest();
    }

    bool empty() const { return levelCount_ == 0; }
    size_t size() const { return levelCount_; }

    template <typename Fn>
    void forEachLevel(Fn&& fn) {
        forEachLevelImpl(levels_, std::forward<Fn>(fn));
    }

    template <typename Fn>
    void forEachLevel(Fn&& fn) const {
        forEachLevelImpl(levels_, std::forward<Fn>(fn));
    }

    template <typename Fn>
    void forEachLevelWhile(Fn&& fn) {
        forEachLevelWhileImpl(levels_, std::forward<Fn>(fn));
    }

    template <typename Fn>
    void forEachLevelWhile(Fn&& fn) const {
        forEachLevelWhileImpl(levels_, std::forward<Fn>(fn));
    }

private:
    template <typename Levels, typename Fn>
    void forEachLevelImpl(Levels& lvls, Fn&& fn) const {
        if (levelCount_ == 0 || !hasBest_) {
            return;
        }

        if (side_ == Side::Buy) {
            iterateDescending(bestIdx_, [&](size_t idx) {
                fn(indexToPrice(idx), lvls[idx]);
                return true;
            });
        } else {
            iterateAscending(bestIdx_, [&](size_t idx) {
                fn(indexToPrice(idx), lvls[idx]);
                return true;
            });
        }
    }

    template <typename Levels, typename Fn>
    void forEachLevelWhileImpl(Levels& lvls, Fn&& fn) const {
        if (levelCount_ == 0 || !hasBest_) {
            return;
        }

        if (side_ == Side::Buy) {
            iterateDescending(bestIdx_, [&](size_t idx) {
                return fn(indexToPrice(idx), lvls[idx]);
            });
        } else {
            iterateAscending(bestIdx_, [&](size_t idx) {
                return fn(indexToPrice(idx), lvls[idx]);
            });
        }
    }

    void allocateStorage() {
        bitmap0Words_ = (capacity_ + 63) / 64;
        bitmap1Words_ = (bitmap0Words_ + 63) / 64;

        size_t levelBytes = sizeof(OrderList) * capacity_;
        size_t bitmap0Bytes = sizeof(uint64_t) * bitmap0Words_;
        size_t bitmap1Bytes = sizeof(uint64_t) * bitmap1Words_;
        size_t alignPad = alignof(uint64_t) - 1;
        size_t totalBytes = levelBytes + alignPad + bitmap0Bytes + bitmap1Bytes;

        storage_ = static_cast<std::byte*>(
            ::operator new(totalBytes, std::align_val_t{alignof(OrderList)}));

        std::byte* cursor = storage_;
        levels_ = reinterpret_cast<OrderList*>(cursor);
        for (size_t i = 0; i < capacity_; ++i) {
            new (&levels_[i]) OrderList();
        }

        cursor += levelBytes;
        uintptr_t aligned = (reinterpret_cast<uintptr_t>(cursor) + alignPad) &
                            ~static_cast<uintptr_t>(alignPad);
        cursor = reinterpret_cast<std::byte*>(aligned);
        bitmap0_ = reinterpret_cast<uint64_t*>(cursor);
        std::memset(bitmap0_, 0, bitmap0Bytes);

        cursor += bitmap0Bytes;
        bitmap1_ = reinterpret_cast<uint64_t*>(cursor);
        std::memset(bitmap1_, 0, bitmap1Bytes);
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
    OrderList* levels_{nullptr};
    uint64_t* bitmap0_{nullptr};
    uint64_t* bitmap1_{nullptr};
    std::byte* storage_{nullptr};
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
