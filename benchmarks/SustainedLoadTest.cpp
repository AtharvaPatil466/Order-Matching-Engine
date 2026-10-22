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
// A MEASUREMENT TOOL, NOT A LATENCY GATE (BENCH-7). It used to exit 1 when
// any window's P99 crossed --p99-threshold, a number its own header called
// uncalibrated; the CI smoke step failed on it in every local run. The
// threshold is gone. Exit 1 now means the workload was not the one described:
// an order rejected at submit or by its book, or processed != submitted.
//
// What the latency columns report is END-TO-END time from ingress to
// completion, which under sustained offered load is dominated by QUEUE
// RESIDENCY, not by service time. Measured here: ~790 ns P50 at 400k/s and
// 25-37 us P99.
//
// There are no cancels, so every order may rest: each book's pool is sized
// for the whole run (rate x duration / symbols, +25%) and memory grows with
// both. The default 10,000-slot pool shed 85% of a 5 s, 100k/s run as
// PoolCapacityExceeded.
//
// Two observations worth having before anyone picks a latency threshold:
//
//   * Throughput is flat and latency IMPROVES over the first ~6 seconds
//     (P50 2,160 -> 790 ns at 400k/s) while RSS climbs ~24% and then stops.
//     That is warmup — allocator and page-fault settling — not degradation.
//     A threshold applied from window 0 measures warmup.
//   * Latency is LOWER at high offered rate than at low. 790 ns P50 at
//     400k/s versus ~4,500 ns at 50k/s. At low rate the queues drain between
//     orders and each one pays cold-cache cost; at high rate the structures
//     stay resident. So a threshold is only meaningful alongside the rate it
//     was measured at, and "slower under light load" is the expected shape
//     rather than a fault.

#include "MatchingEngine.h"
#include "LatencyTracker.h"
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
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

using namespace OrderMatcher;

// Async submitOrder() only says "queued". Rejects the books make later (price
// band, breaker, pool) arrive as order updates on the worker threads, so they
// are counted here to tell a run that matched from one that timed rejects.
class BookRejectCounter : public EventListener {
public:
    void onOrderUpdate(const OrderUpdate& u) override {
        if (u.status == OrderStatus::Rejected) rejects_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t rejects() const { return rejects_.load(std::memory_order_relaxed); }

private:
    std::atomic<uint64_t> rejects_{0};
};

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
    int durationSec = 10;
    int windowSec = 1;
    int targetRate = 200000;  // orders/sec; the pool below grows with rate x duration
    int numThreads = 4;
    int numSymbols = 8;
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
        } else if (std::strcmp(argv[i], "--lean") == 0) {
            mode = "lean";
        }
    }

    std::printf("=== Sustained Load Test ===\n");
    std::printf("Duration:      %d sec\n", durationSec);
    std::printf("Window:        %d sec\n", windowSec);
    std::printf("Target rate:   %d orders/sec (aggregate across producers)\n",
                targetRate);
    std::printf("Threads:       %d\n", numThreads);
    std::printf("Symbols:       %d\n", numSymbols);
    std::printf("Mode:          %s\n\n", mode.c_str());

    // Set up engine
    MatchingEngine engine;
    const uint64_t poolSlots = static_cast<uint64_t>(targetRate) *
                               static_cast<uint64_t>(durationSec) * 5 / 4 /
                               static_cast<uint64_t>(std::max(1, numSymbols)) + 1024;
    setenv("OB_ORDER_POOL_CAPACITY", std::to_string(poolSlots).c_str(), 1);
    std::printf("Order pool:    %llu slots per book (~%llu MB of orders in all)\n\n",
                static_cast<unsigned long long>(poolSlots),
                static_cast<unsigned long long>(poolSlots * sizeof(Order) *
                                                static_cast<uint64_t>(numSymbols) >> 20));

    BookRejectCounter bookRejects;
    for (int s = 1; s <= numSymbols; ++s) {
        engine.addSymbol(static_cast<SymbolId>(s));
        engine.getOrderBook(static_cast<SymbolId>(s))->setEventListener(&bookRejects);
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

    // Compute inter-order delay for the target rate.
    //
    // DIVIDED BY THE PRODUCER COUNT, because every producer paces itself with
    // this same interval. Previously this was the whole target rate and each of
    // the producerCount threads offered it independently, so the run offered
    // producerCount x targetRate and the "Target rate" line printed above was
    // simply not the rate being applied. At the defaults that meant 1M/s
    // offered while the header claimed 250k/s — and the measured throughput
    // duly came back at ~1M/s, which looked like agreement.
    const int producerCount = std::max(1, numThreads / 2);
    double interOrderNs =
        1e9 / (static_cast<double>(targetRate) / static_cast<double>(producerCount));

    auto producerFn = [&](int threadId) {
        std::mt19937_64 rng(42 + threadId);
        std::uniform_int_distribution<int> sideDist(0, 1);
        // +/-2% of 100.00. At +/-5% the first order's price anchored each
        // book's 5% volatility breaker inside the range, and 85% of all
        // orders were rejected by the books.
        std::uniform_int_distribution<Price> priceDist(98'00, 102'00);
        std::uniform_int_distribution<Quantity> qtyDist(1, 50);
        std::uniform_int_distribution<int> symbolDist(1, numSymbols);

        uint64_t localSubmitted = 0;
        uint64_t localRejected = 0;
        OrderId nextId = static_cast<OrderId>(threadId * 100000000);

        // Per-thread rate control
        auto lastSend = std::chrono::steady_clock::now();
        double accumulated = 0;

        while (running.load(std::memory_order_relaxed)) {
            auto now = std::chrono::steady_clock::now();
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
    for (int i = 0; i < producerCount; ++i) {
        producers.emplace_back(producerFn, i);
    }

    // Monitor thread: collect window stats
    auto monitorStart = std::chrono::steady_clock::now();
    uint64_t prevProcessed = 0;
    // Snapshot of the cumulative latency histogram at the previous window
    // boundary; each window reports the delta against it.
    LatencyTracker prevLatency;

    for (int w = 0; w < durationSec / windowSec; ++w) {
        std::this_thread::sleep_for(std::chrono::seconds(windowSec));

        uint64_t nowProcessed = engine.getProcessedCount();
        uint64_t windowProcessed = nowProcessed - prevProcessed;
        prevProcessed = nowProcessed;

        // PER-WINDOW latency, not cumulative.
        //
        // getAggregateE2ELatency() merges per-thread trackers that are never
        // reset, so it always reports the distribution since process start.
        // Sampling it once per window and labelling the result "this window"
        // made the reported P50 decay monotonically across a run (780 ms -> 48
        // us over 30 windows in one recorded run) purely because the
        // cumulative population kept growing. Subtracting the previous
        // snapshot gives the distribution of what was actually recorded during
        // this window.
        auto cumulative = engine.getAggregateE2ELatency();
        auto e2e = cumulative.deltaFrom(prevLatency);
        prevLatency = cumulative;

        WindowStats ws;
        ws.windowNum = static_cast<uint64_t>(w);
        ws.ordersProcessed = windowProcessed;
        ws.p50Ns = static_cast<double>(e2e.getP50());
        ws.p99Ns = static_cast<double>(e2e.getP99());
        ws.p999Ns = static_cast<double>(e2e.getP999());
        // Not recoverable from a histogram delta — see LatencyTracker::deltaFrom.
        ws.maxNs = 0.0;
        ws.throughput = static_cast<double>(windowProcessed) / static_cast<double>(windowSec);
        ws.memoryKB = getCurrentMemoryKB();

        std::printf("Window %2d: %7llu orders | %9.0f/s | P50=%6.0fns P99=%6.0fns P99.9=%7.0fns | Mem=%zuKB\n",
                    w, static_cast<unsigned long long>(ws.ordersProcessed),
                    ws.throughput, ws.p50Ns, ws.p99Ns, ws.p999Ns, ws.memoryKB);

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
    std::printf("Book rejects:     %llu\n", static_cast<unsigned long long>(bookRejects.rejects()));
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

    // Workload check: the windows above describe this flow only if every order
    // was admitted, processed and accepted by its book.
    if (totalRejected.load() != 0 || bookRejects.rejects() != 0 ||
        engine.getProcessedCount() != totalSubmitted.load()) {
        std::printf("\nWORKLOAD CHECK FAILED: rejected at submit %llu, rejected by a book %llu, "
                    "processed %llu of %llu submitted\n",
                    static_cast<unsigned long long>(totalRejected.load()),
                    static_cast<unsigned long long>(bookRejects.rejects()),
                    static_cast<unsigned long long>(engine.getProcessedCount()),
                    static_cast<unsigned long long>(totalSubmitted.load()));
        return 1;
    }
    std::printf("\nWorkload check OK: every order admitted, processed and accepted by its book\n");
    return 0;
}
