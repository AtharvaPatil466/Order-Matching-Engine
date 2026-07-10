#pragma once

#include <cstdint>
#include <chrono>
#include <thread>
#include <vector>
#include <iostream>

#ifdef __linux__
#include <sched.h>
#include <pthread.h>
#endif

namespace OrderMatcher {

namespace Utils {

// Pin current thread to a specific CPU core
inline bool pinThread([[maybe_unused]] int core_id) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    pthread_t current_thread = pthread_self();
    if (pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset) != 0) {
        std::cerr << "Error calling pthread_setaffinity_np: " << errno << std::endl;
        return false;
    }
    return true;
#else
    // macOS/Windows do not support pthread_setaffinity_np easily
    // For macOS, thread_policy_set could be used but is complex
    // Printing warning for now
    std::cerr << "Thread pinning not fully supported on this OS" << std::endl;
    return false;
#endif
}

// Low-overhead timestamp using RDTSC (x86) or portable fallback
inline uint64_t rdtsc() {
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
    unsigned int lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
    return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
    uint64_t val;
    __asm__ __volatile__("isb; mrs %0, cntvct_el0" : "=r"(val));
    return val;
#else
    // Fallback using std::chrono
    return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

// ── TSC calibration ────────────────────────────────────────────────────────
// rdtsc() above returns a raw counter whose unit is platform-defined:
//   x86     : CPU timestamp-counter ticks (invariant TSC on modern parts)
//   aarch64 : cntvct_el0 virtual-counter ticks (fixed frequency)
//   other   : steady_clock nanoseconds (ticks == ns)
// To turn a *delta* of ticks into nanoseconds we measure the counter's rate
// once against a trusted wall clock, cache it, and multiply. This lets the hot
// path stamp a cheap counter (rdtsc, ~a few cycles on x86 vs a clock_gettime
// syscall/VDSO call) and defer the ns conversion to when a measurement is
// flushed to a metric — OFF the matching critical path.
//
// PLATFORM NOTE: the *performance* win (rdtsc cheaper than clock_gettime) is
// x86-only. On aarch64 the fallback uses cntvct_el0 (still a userspace counter,
// so correct and monotonic); on other platforms rdtsc() is steady_clock ns, so
// calibration measures ~1 ns/tick and conversion is a no-op. Correctness holds
// everywhere; only x86 sees the latency benefit.

// Measure ns-per-tick by busy-sampling rdtsc() against steady_clock over a
// short fixed wall interval. Returns nanoseconds per counter tick.
inline double measureTscNsPerTick(uint64_t sampleNs = 5'000'000ull /* 5 ms */) {
    const auto wallStart = std::chrono::steady_clock::now();
    const uint64_t tscStart = rdtsc();
    std::chrono::nanoseconds elapsed{0};
    do {
        elapsed = std::chrono::steady_clock::now() - wallStart;
    } while (static_cast<uint64_t>(elapsed.count()) < sampleNs);
    const uint64_t tscEnd = rdtsc();
    const uint64_t ticks = (tscEnd > tscStart) ? (tscEnd - tscStart) : 0;
    if (ticks == 0) return 1.0;  // degenerate — treat ticks as ns
    return static_cast<double>(elapsed.count()) / static_cast<double>(ticks);
}

// Calibrated ns-per-tick, computed exactly once on first use. C++11 guarantees
// thread-safe initialization of function-local statics, so concurrent first
// callers block until calibration completes rather than racing.
inline double tscNsPerTick() {
    static const double kNsPerTick = measureTscNsPerTick();
    return kNsPerTick;
}

// Convert a delta of rdtsc() ticks to nanoseconds using the calibrated rate.
inline uint64_t tscTicksToNs(uint64_t ticks) {
    return static_cast<uint64_t>(static_cast<double>(ticks) * tscNsPerTick());
}

// ── Hot-path latency clock (x86-guarded) ───────────────────────────────────
// The invariant TSC read via `rdtsc` is only a reliable, cheap, monotonic
// counter on x86. On other ISAs the counter is either not cheaper than a
// portable clock or is outright unreliable for this purpose — e.g. on macOS
// Apple Silicon a userspace `mrs cntvct_el0` returns garbage (verified: the
// value does not advance with wall time). So for per-order LATENCY MEASUREMENT
// we take the rdtsc fast path ONLY on x86 and fall back everywhere else to a
// portable monotonic *nanosecond* clock (steady_clock). The performance benefit
// is therefore x86-only; correctness (sane, monotonic ns) holds on every
// platform.
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#  define OB_LATENCY_CLOCK_IS_TSC 1
#else
#  define OB_LATENCY_CLOCK_IS_TSC 0
#endif

// Cheap monotonic counter for hot-path interval measurement. On x86 this is a
// raw TSC tick count (convert the delta with latencyTicksToNs()); elsewhere it
// is already nanoseconds (latencyTicksToNs() is then the identity).
inline uint64_t latencyClockTicks() {
#if OB_LATENCY_CLOCK_IS_TSC
    return rdtsc();
#else
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

// Convert a latencyClockTicks() delta to nanoseconds.
inline uint64_t latencyTicksToNs(uint64_t ticks) {
#if OB_LATENCY_CLOCK_IS_TSC
    return tscTicksToNs(ticks);
#else
    return ticks;  // steady_clock ticks are already nanoseconds
#endif
}

// Force TSC calibration now (e.g. at single-threaded startup) so the ~5 ms
// sample never lands on the hot path. No-op where the latency clock is not the
// TSC, so non-x86 startups pay nothing. Idempotent.
inline void calibrateTsc() {
#if OB_LATENCY_CLOCK_IS_TSC
    (void)tscNsPerTick();
#endif
}

} // namespace Utils

} // namespace OrderMatcher
