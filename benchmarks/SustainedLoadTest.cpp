// SustainedLoadTest — sustained throughput and latency degradation test.
//
// Roadmap Phase 5, Week 17: Sustained Load Test
//
// Runs a continuous workload for a configurable duration (default 30s),
// at a configurable target rate. Measures:
//   - Achieved throughput (orders/sec)
//   - Latency percentiles over 1s windows (to detect degradation)
//   - Queue depth over time
//   - Memory growth (via getrusage)
//
// Exit code 0 if all P99 windows are under the specified threshold.
// Exit code 1 if any window exceeds the threshold (regression detected).

#include "MatchingEngine.h"
#include "Utils.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

#if defined(__linux__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

using namespace OrderMatcher;

struct WindowStats {
    uint64_t windowNum{0};
    uint64_t ordersProcessed{0};
    double   p50Ns{0};
    double   p99Ns{0};
    double   p999Ns{0};
    double   maxNs{0};
    double   throughput{0};  // orders/sec
    size_t   memoryKB{0};
};

size_t getCurrentMemoryKB() {
#if defined(__linux__) || defined(__APPLE__)
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        #if defined(__APPLE__)
        return static_cast<size_t>(usage.ru_maxrss / 1024);  // bytes on macOS
        #else
        return static_cast<size_t>(usage.ru_maxrss);  // KB on Linux
        #endif
    }
#endif
    return 0;
}

int main(int argc, char* argv[]) {
    int durationSec = 30;
    int windowSec = 1;
    int targetRate = 500000;  // 500K orders/sec target
    int numThreads = 4;
    int numSymbols = 8;
    double p99ThresholdNs = 5000;  // 5μs P99 threshold
    std::string mode = "full";

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            durationSec = std::stoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--window") == 0 && i + 1 < argc) {
            windowSec = std::stoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            targetRate = std::stoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            numThreads = std::stoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--symbols") == 0 && i + 1 < argc) {
            numSymbols = std::stoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--p99-threshold") == 0 && i + 1 < argc) {
            p99ThresholdNs = std::stod(argv[++i]);
        } else if (std::strcmp(argv[i], "--lean") == 0) {
            mode = "lean";
        }
    }

    std::printf("=== Sustained Load Test ===\n");
    std::printf("Duration:      %d sec\n", durationSec);
    std::printf("Window:        %d sec\n", windowSec);
    std::printf("Target rate:   %d orders/sec\n", targetRate);
    std::printf("Threads:       %d\n", numThreads);
    std::printf("Symbols:       %d\n", numSymbols);
    std::printf("P99 threshold: %.0f ns\n", p99ThresholdNs);
    std::printf("Mode:          %s\n\n", mode.c_str());

    // Set up engine
    MatchingEngine engine;
    for (int s = 1; s <= numSymbols; ++s) {
        engine.addSymbol(static_cast<SymbolId>(s));
    }
    engine.startAsync(static_cast<size_t>(numThreads), 1024 * 1024);

    // Set up risk limits in full mode
    if (mode == "full") {
        for (int s = 1; s <= numSymbols; ++s) {
            RiskLimits limits;
            limits.maxOrderSize = 10000;
            limits.maxPositionSize = 1000000;
            engine.setRiskLimits(static_cast<SymbolId>(s), 1, limits);
            engine.setRiskLimits(static_cast<SymbolId>(s), 2, limits);
        }
    }

    // Producer threads
    std::atomic<bool> running{true};
    std::atomic<uint64_t> totalSubmitted{0};
    std::atomic<uint64_t> totalRejected{0};

    // Window tracking
    std::vector<WindowStats> windows;
    std::mutex windowMu;

    // Compute inter-order delay for target rate
    double interOrderNs = 1e9 / static_cast<double>(targetRate);

    auto producerFn = [&](int threadId) {
        std::mt19937_64 rng(42 + threadId);
        std::uniform_int_distribution<int> sideDist(0, 1);
        std::uniform_int_distribution<Price> priceDist(95'00, 105'00);
        std::uniform_int_distribution<Quantity> qtyDist(1, 50);
        std::uniform_int_distribution<int> symbolDist(1, numSymbols);

        uint64_t localSubmitted = 0;
        uint64_t localRejected = 0;
        OrderId nextId = static_cast<OrderId>(threadId * 100000000);

        // Per-thread rate control
        auto lastSend = std::chrono::high_resolution_clock::now();
        double accumulated = 0;

        while (running.load(std::memory_order_relaxed)) {
            auto now = std::chrono::high_resolution_clock::now();
            double elapsed = static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    now - lastSend).count());
            accumulated += elapsed;
            lastSend = now;

            while (accumulated >= interOrderNs) {
                accumulated -= interOrderNs;

                Side side = sideDist(rng) == 0 ? Side::Buy : Side::Sell;
                Price price = priceDist(rng);
                Quantity qty = qtyDist(rng);
                SymbolId sym = static_cast<SymbolId>(symbolDist(rng));

                auto result = engine.submitOrder(
                    sym, nextId++, static_cast<ParticipantId>(threadId + 1),
                    side, price, qty, OrderType::Limit);

                if (result.isAccepted()) {
                    ++localSubmitted;
                } else {
                    ++localRejected;
                }
            }

            // Brief pause to prevent tight spinning
            std::this_thread::yield();
        }

        totalSubmitted.fetch_add(localSubmitted, std::memory_order_relaxed);
        totalRejected.fetch_add(localRejected, std::memory_order_relaxed);
    };

    // Start producer threads
    std::vector<std::thread> producers;
    int producerCount = std::max(1, numThreads / 2);
    for (int i = 0; i < producerCount; ++i) {
        producers.emplace_back(producerFn, i);
    }

    // Monitor thread: collect window stats
    auto monitorStart = std::chrono::steady_clock::now();
    uint64_t prevProcessed = 0;
    bool regressionDetected = false;

    for (int w = 0; w < durationSec / windowSec; ++w) {
        std::this_thread::sleep_for(std::chrono::seconds(windowSec));

        uint64_t nowProcessed = engine.getProcessedCount();
        uint64_t windowProcessed = nowProcessed - prevProcessed;
        prevProcessed = nowProcessed;

        // Get E2E latency for this window
        auto e2e = engine.getAggregateE2ELatency();

        WindowStats ws;
        ws.windowNum = static_cast<uint64_t>(w);
        ws.ordersProcessed = windowProcessed;
        ws.p50Ns = static_cast<double>(e2e.getP50());
        ws.p99Ns = static_cast<double>(e2e.getP99());
        ws.p999Ns = static_cast<double>(e2e.getP999());
        ws.maxNs = static_cast<double>(e2e.getMax());
        ws.throughput = static_cast<double>(windowProcessed) / static_cast<double>(windowSec);
        ws.memoryKB = getCurrentMemoryKB();

        std::printf("Window %2d: %7llu orders | %9.0f/s | P50=%6.0fns P99=%6.0fns P99.9=%7.0fns | Mem=%zuKB\n",
                    w, static_cast<unsigned long long>(ws.ordersProcessed),
                    ws.throughput, ws.p50Ns, ws.p99Ns, ws.p999Ns, ws.memoryKB);

        if (ws.p99Ns > p99ThresholdNs && ws.ordersProcessed > 0) {
            regressionDetected = true;
        }

        {
            std::lock_guard<std::mutex> lock(windowMu);
            windows.push_back(ws);
        }
    }

    // Stop
    running.store(false);
    for (auto& t : producers) {
        if (t.joinable()) t.join();
    }
    engine.waitForDrain();

    auto totalDuration = std::chrono::steady_clock::now() - monitorStart;
    double totalSec = std::chrono::duration<double>(totalDuration).count();

    std::printf("\n=== Summary ===\n");
    std::printf("Total duration:   %.1f sec\n", totalSec);
    std::printf("Total submitted:  %llu\n", static_cast<unsigned long long>(totalSubmitted.load()));
    std::printf("Total rejected:   %llu\n", static_cast<unsigned long long>(totalRejected.load()));
    std::printf("Total processed:  %llu\n", static_cast<unsigned long long>(engine.getProcessedCount()));
    std::printf("Avg throughput:   %.0f orders/sec\n",
                static_cast<double>(engine.getProcessedCount()) / totalSec);

    // Check for memory growth (regression indicator)
    if (windows.size() >= 2) {
        double memGrowthPct = 0;
        if (windows.front().memoryKB > 0) {
            memGrowthPct = static_cast<double>(windows.back().memoryKB - windows.front().memoryKB)
                           / static_cast<double>(windows.front().memoryKB) * 100.0;
        }
        std::printf("Memory growth:    %.1f%%\n", memGrowthPct);
        if (memGrowthPct > 50.0) {
            std::printf("⚠️  Memory growth > 50%% — possible leak\n");
        }
    }

    if (regressionDetected) {
        std::printf("\n❌ REGRESSION: At least one window exceeded P99 threshold of %.0f ns\n",
                    p99ThresholdNs);
        return 1;
    }

    std::printf("\n✅ All windows within P99 threshold (%.0f ns)\n", p99ThresholdNs);
    return 0;
}
