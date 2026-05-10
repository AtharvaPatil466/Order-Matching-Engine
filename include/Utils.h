#pragma once

#include <cstdint>
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

} // namespace Utils

} // namespace OrderMatcher
