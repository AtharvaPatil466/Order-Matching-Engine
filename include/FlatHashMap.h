#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <new>
#include <type_traits>
#include <utility>

namespace OrderMatcher {

// Open-addressing hash map with robin-hood probing.
// Uses a flat bucket array, supports move-only values, and grows before
// probe chains become pathological.
template <typename Key, typename Value, typename Hash = std::hash<Key>>
class FlatHashMap {
public:
    struct Entry {
        Key key{};
        Value value{};
        uint32_t dist{0}; // 0 = empty, otherwise probe distance + 1
    };

    explicit FlatHashMap(size_t minCapacity = 1024) {
        allocateStorage(capacityForSize(minCapacity));
    }

    ~FlatHashMap() {
        delete[] entries_;
    }

    FlatHashMap(const FlatHashMap&) = delete;
    FlatHashMap& operator=(const FlatHashMap&) = delete;

    FlatHashMap(FlatHashMap&& other) noexcept
        : entries_(other.entries_), capacity_(other.capacity_), mask_(other.mask_),
          size_(other.size_) {
        other.entries_ = nullptr;
        other.capacity_ = 0;
        other.mask_ = 0;
        other.size_ = 0;
    }

    FlatHashMap& operator=(FlatHashMap&& other) noexcept {
        if (this != &other) {
            delete[] entries_;
            entries_ = other.entries_;
            capacity_ = other.capacity_;
            mask_ = other.mask_;
            size_ = other.size_;
            other.entries_ = nullptr;
            other.capacity_ = 0;
            other.mask_ = 0;
            other.size_ = 0;
        }
        return *this;
    }

    template <typename K, typename V>
    Value* insert(K&& key, V&& value) {
        ensureCapacityForInsert();

        Key k(std::forward<K>(key));
        Value v(std::forward<V>(value));
        uint32_t dist = 1;
        size_t idx = hash_(k) & mask_;

        for (;;) {
            Entry& entry = entries_[idx];

            if (entry.dist == 0) {
                entry.key = std::move(k);
                entry.value = std::move(v);
                entry.dist = dist;
                ++size_;
                return &entry.value;
            }

            if (entry.key == k) {
                entry.value = std::move(v);
                return &entry.value;
            }

            if (entry.dist < dist) {
                std::swap(k, entry.key);
                std::swap(v, entry.value);
                std::swap(dist, entry.dist);
            }

            idx = (idx + 1) & mask_;
            ++dist;
        }
    }

    Value& operator[](const Key& key) {
        if (Value* value = find(key)) {
            return *value;
        }
        return *insert(key, Value{});
    }

    [[nodiscard]] Value* find(const Key& key) {
        if (!entries_) {
            return nullptr;
        }

        size_t idx = hash_(key) & mask_;
        uint32_t dist = 1;

        for (;;) {
            Entry& entry = entries_[idx];
            if (entry.dist == 0 || entry.dist < dist) {
                return nullptr;
            }
            if (entry.key == key) {
                return &entry.value;
            }
            idx = (idx + 1) & mask_;
            ++dist;
        }
    }

    [[nodiscard]] const Value* find(const Key& key) const {
        if (!entries_) {
            return nullptr;
        }

        size_t idx = hash_(key) & mask_;
        uint32_t dist = 1;

        for (;;) {
            const Entry& entry = entries_[idx];
            if (entry.dist == 0 || entry.dist < dist) {
                return nullptr;
            }
            if (entry.key == key) {
                return &entry.value;
            }
            idx = (idx + 1) & mask_;
            ++dist;
        }
    }

    bool contains(const Key& key) const {
        return find(key) != nullptr;
    }

    bool erase(const Key& key) {
        if (!entries_) {
            return false;
        }

        size_t idx = hash_(key) & mask_;
        uint32_t dist = 1;

        for (;;) {
            Entry& entry = entries_[idx];
            if (entry.dist == 0 || entry.dist < dist) {
                return false;
            }

            if (entry.key == key) {
                size_t cur = idx;
                for (;;) {
                    size_t next = (cur + 1) & mask_;
                    Entry& nextEntry = entries_[next];
                    if (nextEntry.dist <= 1) {
                        entries_[cur] = Entry{};
                        break;
                    }

                    entries_[cur] = std::move(nextEntry);
                    --entries_[cur].dist;
                    nextEntry = Entry{};
                    cur = next;
                }

                --size_;
                return true;
            }

            idx = (idx + 1) & mask_;
            ++dist;
        }
    }

    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    size_t capacity() const { return capacity_; }
    double loadFactor() const {
        return capacity_ == 0 ? 0.0
                              : static_cast<double>(size_) / static_cast<double>(capacity_);
    }

    void reserve(size_t expectedSize) {
        size_t desiredCapacity = capacityForSize(expectedSize);
        if (desiredCapacity > capacity_) {
            rehash(desiredCapacity);
        }
    }

    template <typename Fn>
    void forEach(Fn&& fn) {
        for (size_t i = 0; i < capacity_; ++i) {
            if (entries_[i].dist != 0) {
                fn(entries_[i].key, entries_[i].value);
            }
        }
    }

    template <typename Fn>
    void forEach(Fn&& fn) const {
        for (size_t i = 0; i < capacity_; ++i) {
            if (entries_[i].dist != 0) {
                fn(entries_[i].key, entries_[i].value);
            }
        }
    }

    void clear() {
        for (size_t i = 0; i < capacity_; ++i) {
            entries_[i] = Entry{};
        }
        size_ = 0;
    }

private:
    static constexpr double kMaxLoadFactor = 0.50;

    static size_t nextPowerOfTwo(size_t value) {
        size_t power = 1;
        while (power < value) {
            power <<= 1;
        }
        return power;
    }

    static size_t capacityForSize(size_t expectedSize) {
        size_t desired = expectedSize == 0 ? 8 : expectedSize;
        size_t minBuckets = static_cast<size_t>(
            static_cast<double>(desired) / kMaxLoadFactor);
        return nextPowerOfTwo(minBuckets + 1);
    }

    void allocateStorage(size_t newCapacity) {
        capacity_ = newCapacity;
        mask_ = capacity_ - 1;
        entries_ = new Entry[capacity_]();
        size_ = 0;
    }

    void ensureCapacityForInsert() {
        if ((size_ + 1) > static_cast<size_t>(capacity_ * kMaxLoadFactor)) {
            rehash(capacity_ == 0 ? 8 : capacity_ * 2);
        }
    }

    void rehash(size_t newCapacity) {
        Entry* oldEntries = entries_;
        size_t oldCapacity = capacity_;

        allocateStorage(nextPowerOfTwo(newCapacity));

        if (!oldEntries) {
            return;
        }

        for (size_t i = 0; i < oldCapacity; ++i) {
            if (oldEntries[i].dist != 0) {
                insert(std::move(oldEntries[i].key), std::move(oldEntries[i].value));
            }
        }

        delete[] oldEntries;
    }

    Entry* entries_{nullptr};
    size_t capacity_{0};
    size_t mask_{0};
    size_t size_{0};
    [[no_unique_address]] Hash hash_{};
};

} // namespace OrderMatcher
