#pragma once

#include <atomic>
#include <vector>
#include <cassert>
#include <new>

namespace OrderMatcher {

// Hardware cache line size (typically 64 bytes on x86/ARM)
#ifdef __cpp_lib_hardware_interference_size
    constexpr size_t CACHE_LINE_SIZE = std::hardware_destructive_interference_size;
#else
    constexpr size_t CACHE_LINE_SIZE = 64;
#endif

template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t size) : buffer(size) {
        assert((size & (size - 1)) == 0 && "Size must be power of 2");
        mask_ = size - 1;
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    bool push(const T& item) {
        const auto current_tail = tail_.load(std::memory_order_relaxed);
        const auto next_tail = (current_tail + 1) & mask_;

        if (next_tail != head_.load(std::memory_order_acquire)) {
            buffer[current_tail] = item;
            tail_.store(next_tail, std::memory_order_release);
            return true;
        }
        return false; // Buffer full
    }

    bool pop(T& item) {
        const auto current_head = head_.load(std::memory_order_relaxed);

        if (current_head == tail_.load(std::memory_order_acquire)) {
            return false; // Buffer empty
        }

        item = buffer[current_head];
        head_.store((current_head + 1) & mask_, std::memory_order_release);
        return true;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    size_t capacity() const {
        return buffer.size();
    }

    size_t size() const {
        auto h = head_.load(std::memory_order_acquire);
        auto t = tail_.load(std::memory_order_acquire);
        return (t - h + buffer.size()) & mask_;
    }

private:
    std::vector<T> buffer;
    size_t mask_;

    // Cache-line padded atomics to prevent false sharing between producer and consumer
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head_;
    char pad_head_[CACHE_LINE_SIZE - sizeof(std::atomic<size_t>)];

    alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail_;
    char pad_tail_[CACHE_LINE_SIZE - sizeof(std::atomic<size_t>)];
};

} // namespace OrderMatcher
