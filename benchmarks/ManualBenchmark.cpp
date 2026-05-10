#include "OrderBook.h"
#include "Utils.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <vector>
#include <fstream>
#include <string>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#include <mach/mach_init.h>
#include <mach/thread_policy.h>
#include <mach/thread_act.h>
#include <pthread.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

using namespace OrderMatcher;

// ─── Platform Detection & Metadata ──────────────────────────────────────────

struct PlatformInfo {
    std::string cpu;
    std::string os;
    uint64_t tscFreqHz{0};
    double tickToNs{1.0};

    void detect() {
#if defined(__APPLE__)
        os = "macOS (Apple Silicon)";
        // Get CPU brand string
        char brand[256];
        size_t len = sizeof(brand);
        if (sysctlbyname("machdep.cpu.brand_string", brand, &len, nullptr, 0) == 0) {
            cpu = brand;
        } else {
            cpu = "Apple Silicon (unknown model)";
        }
        // Apple Silicon fixed 24MHz TSC
        tscFreqHz = 24000000;
        tickToNs = 1e9 / static_cast<double>(tscFreqHz);

#elif defined(__linux__)
        os = "Linux";
        // Read CPU model from /proc/cpuinfo
        std::ifstream cpuinfo("/proc/cpuinfo");
        std::string line;
        while (std::getline(cpuinfo, line)) {
            if (line.find("model name") != std::string::npos) {
                auto pos = line.find(':');
                if (pos != std::string::npos)
                    cpu = line.substr(pos + 2);
                break;
            }
        }
        if (cpu.empty()) cpu = "Unknown x86_64";

        // Read TSC frequency — try /sys first, then parse /proc/cpuinfo MHz
        std::ifstream tscFile("/sys/devices/system/cpu/cpu0/tsc_freq_khz");
        if (tscFile.is_open()) {
            uint64_t khz;
            tscFile >> khz;
            tscFreqHz = khz * 1000ULL;
        } else {
            // Fallback: parse "cpu MHz" from /proc/cpuinfo
            cpuinfo.clear();
            cpuinfo.seekg(0);
            while (std::getline(cpuinfo, line)) {
                if (line.find("cpu MHz") != std::string::npos) {
                    auto pos = line.find(':');
                    if (pos != std::string::npos) {
                        double mhz = std::stod(line.substr(pos + 2));
                        tscFreqHz = static_cast<uint64_t>(mhz * 1e6);
                    }
                    break;
                }
            }
        }
        if (tscFreqHz > 0) {
            tickToNs = 1e9 / static_cast<double>(tscFreqHz);
        } else {
            tickToNs = 1.0; // fallback: assume ticks ≈ ns
            tscFreqHz = 1000000000;
        }
#else
        os = "Unknown";
        cpu = "Unknown";
        tickToNs = 1.0;
#endif
    }

    void print() const {
        std::cout << "╔═══════════════════════════════════════════════════════╗\n"
                  << "║          BENCHMARK PLATFORM METADATA                  ║\n"
                  << "╠═══════════════════════════════════════════════════════╣\n"
                  << "║  CPU:     " << cpu << "\n"
                  << "║  OS:      " << os << "\n"
                  << "║  TSC:     " << (tscFreqHz / 1000000) << " MHz"
                  << " (" << std::fixed << std::setprecision(1) << tickToNs << " ns/tick)\n"
#ifdef OB_LEAN_MODE
                  << "║  Mode:    LEAN (risk/OTR/events disabled)\n"
#else
                  << "║  Mode:    FULL (all enterprise features)\n"
#endif
                  << "╚═══════════════════════════════════════════════════════╝\n\n";
    }
};

// ─── Serialized Timestamp ────────────────────────────────────────────────────
// x86_64: CPUID+RDTSC (begin) and RDTSCP+LFENCE (end) serialize the pipeline
//         to prevent out-of-order execution from skewing measurements.
// ARM64:  mach_absolute_time() reads cntvct_el0 directly. ISB barriers are
//         intentionally NOT used — they flush the pipeline (~100-200ns) and
//         add more overhead than they prevent in measurement error.

inline uint64_t rdtsc_begin() {
#if defined(__x86_64__) || defined(_M_X64)
    uint32_t lo, hi;
    asm volatile(
        "cpuid\n\t"
        "rdtsc\n\t"
        : "=a"(lo), "=d"(hi)
        :
        : "rbx", "rcx"
    );
    return ((uint64_t)hi << 32) | lo;
#elif defined(__APPLE__)
    return mach_absolute_time();
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::high_resolution_clock::now().time_since_epoch())
        .count();
#endif
}

inline uint64_t rdtsc_end() {
#if defined(__x86_64__) || defined(_M_X64)
    uint32_t lo, hi;
    asm volatile(
        "rdtscp\n\t"
        "lfence\n\t"
        : "=a"(lo), "=d"(hi)
        :
        : "rcx"
    );
    return ((uint64_t)hi << 32) | lo;
#elif defined(__APPLE__)
    return mach_absolute_time();
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::high_resolution_clock::now().time_since_epoch())
        .count();
#endif
}


// ─── Core Benchmark ─────────────────────────────────────────────────────────

struct BenchStats {
    std::string name;
    std::vector<uint64_t> latencies;
    uint64_t totalTrades;
    uint64_t endBidLevels;
    uint64_t endAskLevels;

    void report(double tickToNs) {
        if (latencies.empty()) return;
        std::sort(latencies.begin(), latencies.end());

        auto getP = [&](double p) {
            return latencies[static_cast<size_t>(p * (latencies.size() - 1))];
        };

        uint64_t sum = std::accumulate(latencies.begin(), latencies.end(), 0ULL);
        double avg = static_cast<double>(sum) / latencies.size();

        std::cout << "\n--- " << name << " ---" << std::endl;
        std::cout << "Ops Measured:    " << latencies.size() << std::endl;
        std::cout << "Trades Executed: " << totalTrades << std::endl;
        std::cout << "Final Levels:    B=" << endBidLevels << ", A=" << endAskLevels << std::endl;
        std::cout << "Throughput:      " << std::fixed << std::setprecision(2)
                  << (latencies.size() / ((sum * tickToNs) / 1e9)) / 1e6 << " M ops/sec" << std::endl;
        std::cout << "Average:         " << std::fixed << std::setprecision(1)
                  << avg * tickToNs << " ns" << std::endl;
        std::cout << "P50 Latency:     " << getP(0.50) * tickToNs << " ns" << std::endl;
        std::cout << "P99 Latency:     " << getP(0.99) * tickToNs << " ns" << std::endl;
        std::cout << "P99.9 Latency:   " << getP(0.999) * tickToNs << " ns" << std::endl;
        std::cout << "Min:             " << latencies.front() * tickToNs << " ns" << std::endl;
        std::cout << "Max:             " << latencies.back() * tickToNs << " ns" << std::endl;
    }
};

void pinThreadToCore(int coreId) {
#if defined(__APPLE__)
    thread_affinity_policy_data_t policy = {coreId};
    thread_policy_set(pthread_mach_thread_np(pthread_self()),
                      THREAD_AFFINITY_POLICY,
                      (thread_policy_t)&policy, 1);
#elif defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(coreId, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif
}

void runHighRigorBenchmark(const PlatformInfo& platform) {
    std::cout << "Starting Industrial-Rigor Benchmark (Serialized Timing)...\n" << std::endl;

    // Pin to core 0 for deterministic timing
    pinThreadToCore(0);

    OrderBook book;
    std::mt19937 rng(42);
    std::uniform_int_distribution<ParticipantId> partDist(1, 1000);
    OrderId nextId = 1;

    // PRE-POPULATE (Balanced Book)
    std::cout << "Pre-populating resting liquidity..." << std::endl;
    book.resetStatus();
    book.addOrder(nextId++, 999, Side::Buy, 1000000, 1, OrderType::Limit);

    for (int i = 0; i < 5000; ++i) {
        book.addOrder(nextId++, partDist(rng), Side::Buy, 980000 + (i % 10000), 100, OrderType::Limit);
        book.addOrder(nextId++, partDist(rng), Side::Sell, 1020000 - (i % 10000), 100, OrderType::Limit);
    }

    std::vector<OrderId> activeOrders;
    activeOrders.reserve(2000000);

    double tickToNs = platform.tickToNs;

    // BENCHMARK 1: ADDS
    size_t numAddOps = 50000;
    BenchStats addStats{"Pure Adds (Limit)", {}, 0, 0, 0};
    addStats.latencies.reserve(numAddOps);
    for (size_t i = 0; i < numAddOps; ++i) {
        OrderId id = nextId++;
        activeOrders.push_back(id);
        uint64_t t1 = rdtsc_begin();
        book.addOrder(id, partDist(rng), Side::Buy, 950000 + (i % 500), 10, OrderType::Limit);
        uint64_t t2 = rdtsc_end();
        addStats.latencies.push_back(t2 - t1);
    }
    addStats.endBidLevels = book.getBidLevelsCount();
    addStats.endAskLevels = book.getAskLevelsCount();
    addStats.report(tickToNs);

    // BENCHMARK 2: MATCHES
    size_t numMatchOps = 20000;
    BenchStats matchStats{"Pure Matches (Aggressive)", {}, 0, 0, 0};
    matchStats.latencies.reserve(numMatchOps);
    uint64_t tradeStart = book.getTradeCount();
    for (size_t i = 0; i < numMatchOps; ++i) {
        OrderId id = nextId++;
        Side s = (i % 2 == 0) ? Side::Buy : Side::Sell;
        Price p = (s == Side::Buy) ? 1040000 : 960000;
        uint64_t t1 = rdtsc_begin();
        book.addOrder(id, partDist(rng), s, p, 10, OrderType::Limit);
        uint64_t t2 = rdtsc_end();
        matchStats.latencies.push_back(t2 - t1);
    }
    matchStats.totalTrades = book.getTradeCount() - tradeStart;
    matchStats.endBidLevels = book.getBidLevelsCount();
    matchStats.endAskLevels = book.getAskLevelsCount();
    matchStats.report(tickToNs);

    // BENCHMARK 3: CANCELS
    size_t numCancelOps = 20000;
    BenchStats cancelStats{"Pure Cancels", {}, 0, 0, 0};
    cancelStats.latencies.reserve(numCancelOps);
    for (size_t i = 0; i < numCancelOps; ++i) {
        if (activeOrders.empty()) break;
        size_t idx = rng() % activeOrders.size();
        OrderId id = activeOrders[idx];
        activeOrders[idx] = activeOrders.back();
        activeOrders.pop_back();

        uint64_t t1 = rdtsc_begin();
        book.cancelOrder(id);
        uint64_t t2 = rdtsc_end();
        cancelStats.latencies.push_back(t2 - t1);
    }
    cancelStats.endBidLevels = book.getBidLevelsCount();
    cancelStats.endAskLevels = book.getAskLevelsCount();
    cancelStats.report(tickToNs);

    std::cout << "\n═══════════════════════════════════════════════════════" << std::endl;
    std::cout << "For production benchmarks on Intel Xeon:" << std::endl;
    std::cout << "  1. Boot with isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3" << std::endl;
    std::cout << "  2. taskset -c 2 ./bin/ManualBenchmark" << std::endl;
    std::cout << "  3. echo 1 > /proc/sys/kernel/sched_rt_runtime_us" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════" << std::endl;
}

int main() {
    PlatformInfo platform;
    platform.detect();
    platform.print();

    runHighRigorBenchmark(platform);
    return 0;
}
