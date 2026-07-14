#pragma once

// FaultInjector — non-invasive, header-only chaos / fault-injection harness.
//
// Zero overhead when disabled: with OB_ENABLE_FAULT_INJECTION undefined (the
// default) shouldFail() is a constexpr-false the optimizer erases. Per-point
// named faults with independent probability and counters; deterministic when
// seeded (reproducible run-to-run); thread-local PRNG so concurrent tests don't
// serialize on a global lock.
//
// At an injection point:
//   if (FaultInjector::instance().shouldFail("journal.fwrite")) return false;
// In a test:
//   auto& fi = FaultInjector::instance();
//   fi.reset(); fi.seed(0xC0FFEE); fi.arm("journal.fwrite", 0.1);
//   ... drive the system ...
//   EXPECT_GT(fi.activations("journal.fwrite"), 0); fi.disarm();

#include <array>
#include <atomic>
#include <cstdint>
#include <random>
#include <string_view>

namespace OrderMatcher {

class FaultInjector {
public:
    // Defined out-of-line in src/FaultInjector.cpp so the singleton is
    // unique across the whole linked binary (a function-local static in a
    // header can produce duplicate instances across a static-library
    // boundary).
    static FaultInjector& instance();

    void reset() {
        for (auto& slot : points_) {
            slot.name = {};
            slot.probability.store(0.0, std::memory_order_relaxed);
            slot.activations.store(0, std::memory_order_relaxed);
        }
        seed_.store(0xA5A5A5A5u, std::memory_order_relaxed);
    }

    // Reseeds the per-thread PRNG on next access. Call before driving a
    // deterministic scenario.
    void seed(uint64_t s) {
        seed_.store(s ? s : 0xA5A5A5A5u, std::memory_order_relaxed);
        // Bump the epoch so existing threads pick up the new seed lazily.
        epoch_.fetch_add(1, std::memory_order_release);
    }

    // Arm a named point with a probability in [0, 1]. Replaces any prior
    // arming for the same name. Returns false if the point table is full
    // (raise kMaxPoints if you hit this).
    bool arm(std::string_view name, double probability) {
        if (probability < 0.0) probability = 0.0;
        if (probability > 1.0) probability = 1.0;
        Slot* slot = findOrAllocate(name);
        if (!slot) return false;
        slot->probability.store(probability, std::memory_order_relaxed);
        return true;
    }

    void disarm(std::string_view name = {}) {
        if (name.empty()) {
            // Disarm everything but keep activation counters for inspection.
            for (auto& slot : points_) {
                slot.probability.store(0.0, std::memory_order_relaxed);
            }
            return;
        }
        if (Slot* slot = find(name)) {
            slot->probability.store(0.0, std::memory_order_relaxed);
        }
    }

    uint64_t activations(std::string_view name) const {
        if (const Slot* slot = find(name)) {
            return slot->activations.load(std::memory_order_relaxed);
        }
        return 0;
    }

#ifdef OB_ENABLE_FAULT_INJECTION
    bool shouldFail(std::string_view name) {
        Slot* slot = find(name);
        if (!slot) return false;
        double p = slot->probability.load(std::memory_order_relaxed);
        if (p <= 0.0) return false;
        if (next_double() < p) {
            slot->activations.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    // Random 64-bit value drawn from the same per-thread, seeded PRNG that
    // shouldFail() uses. Injection sites use this to pick *where* to inject
    // (e.g. which byte to flip) without spinning up their own RNG.
    uint64_t nextU64() {
        thread_local std::mt19937_64 rng;
        thread_local uint64_t        my_epoch = ~uint64_t{0};
        ensureSeeded(rng, my_epoch);
        return rng();
    }
#else
    // Erased at compile time when disabled — preserves the call-site syntax
    // without paying for the lookup or RNG.
    static constexpr bool shouldFail(std::string_view) { return false; }
    static constexpr uint64_t nextU64() { return 0; }
#endif

private:
    static constexpr size_t kMaxPoints = 32;

    struct Slot {
        std::string_view       name;
        std::atomic<double>    probability{0.0};
        std::atomic<uint64_t>  activations{0};
    };

    FaultInjector() { reset(); }

    Slot* find(std::string_view name) {
        for (auto& slot : points_) {
            if (slot.name == name) return &slot;
        }
        return nullptr;
    }
    const Slot* find(std::string_view name) const {
        for (const auto& slot : points_) {
            if (slot.name == name) return &slot;
        }
        return nullptr;
    }

    Slot* findOrAllocate(std::string_view name) {
        if (Slot* s = find(name)) return s;
        for (auto& slot : points_) {
            if (slot.name.empty()) {
                slot.name = name;
                slot.activations.store(0, std::memory_order_relaxed);
                return &slot;
            }
        }
        return nullptr;
    }

    void ensureSeeded(std::mt19937_64& rng, uint64_t& my_epoch) {
        uint64_t cur = epoch_.load(std::memory_order_acquire);
        if (cur != my_epoch) {
            uint64_t s = seed_.load(std::memory_order_relaxed);
            uint64_t tid = reinterpret_cast<uint64_t>(&my_epoch);
            // Mix in thread id only; the epoch is a "reseed needed" trigger
            // and must NOT influence the resulting sequence, otherwise
            // successive seed(X) calls would yield different sequences for
            // the same X.
            rng.seed(s ^ (tid * 0x9E3779B97F4A7C15ull));
            my_epoch = cur;
        }
    }

    double next_double() {
        thread_local std::mt19937_64 rng;
        thread_local uint64_t        my_epoch = ~uint64_t{0};
        ensureSeeded(rng, my_epoch);
        return std::uniform_real_distribution<double>(0.0, 1.0)(rng);
    }

    std::array<Slot, kMaxPoints> points_{};
    std::atomic<uint64_t>        seed_{0xA5A5A5A5u};
    std::atomic<uint64_t>        epoch_{0};
};

}  // namespace OrderMatcher
