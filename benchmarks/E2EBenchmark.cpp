#include "MatchingEngine.h"
#include "LatencyTracker.h"
#include "RealisticWorkload.h"  // bench::BookOutcomes
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

using namespace OrderMatcher;

namespace {

std::string getCPUName() {
#if defined(__APPLE__)
    char brand[256];
    size_t len = sizeof(brand);
    if (sysctlbyname("machdep.cpu.brand_string", brand, &len, nullptr, 0) == 0) {
        return brand;
    }
    return "Apple Silicon";
#else
    return "Unknown CPU";
#endif
}

std::string formatLatency(uint64_t latencyNs) {
    std::ostringstream out;
    if (latencyNs >= 1'000'000) {
        out << std::fixed << std::setprecision(2)
            << (static_cast<double>(latencyNs) / 1'000'000.0) << " ms";
    } else if (latencyNs >= 1'000) {
        out << std::fixed << std::setprecision(2)
            << (static_cast<double>(latencyNs) / 1'000.0) << " us";
    } else {
        out << latencyNs << " ns";
    }
    return out.str();
}

void paceUntil(std::chrono::steady_clock::time_point deadline) {
    using namespace std::chrono;

    while (true) {
        auto now = steady_clock::now();
        if (now >= deadline) {
            return;
        }

        auto remaining = deadline - now;
        if (remaining > microseconds(50)) {
            std::this_thread::sleep_for(remaining - microseconds(20));
        } else {
            std::this_thread::yield();
        }
    }
}

void seedLiquidity(MatchingEngine& engine, size_t numSymbols) {
    uint64_t nextOrderId = 1;

    for (size_t s = 0; s < numSymbols; ++s) {
        SymbolId symbolId = static_cast<SymbolId>(s);
        auto* book = engine.getOrderBook(symbolId);
        if (!book) {
            continue;
        }

        for (int i = 0; i < 1000; ++i) {
            book->addOrder(nextOrderId++, 999, Side::Buy,
                           980000 + (i % 2000), 100, OrderType::Limit);
            book->addOrder(nextOrderId++, 999, Side::Sell,
                           1020000 - (i % 2000), 100, OrderType::Limit);
        }
    }
}

// Order i of a scenario: buys on even i, sells on odd. One in four crosses
// the seeded book — i % 8 == 0 buys, i % 8 == 5 sells, so both sides
// aggress — and the rest rest inside it. The aggressive prices stay inside
// the 5% volatility band anchored on the first seeded order (980,000): the
// old 1,030,000 was 5.1% above it, so the breaker rejected every aggressive
// order, and since i % 4 == 0 is always even they were all buys.
Side sideFor(size_t i) { return (i % 2 == 0) ? Side::Buy : Side::Sell; }
Price priceFor(size_t i) {
    const bool buy = sideFor(i) == Side::Buy;
    if (i % 8 == 0 || i % 8 == 5) return buy ? 1021000 : 979000;
    return buy ? static_cast<Price>(975000 + (i % 1000))
               : static_cast<Price>(1025000 - (i % 1000));
}

struct ScenarioResult {
    std::string name;
    uint64_t submitted = 0;
    uint64_t accepted = 0;
    uint64_t rateLimited = 0;
    uint64_t backpressureRejected = 0;
    uint64_t dropped = 0;
    uint64_t bookRejected = 0;  // shed by a book after "Accepted" (async = queued)
    uint64_t stpCancelled = 0;
    double wallSeconds = 0.0;
    LatencyTracker latency;
};

void printScenario(const ScenarioResult& result) {
    std::cout << "\n--- " << result.name << " ---\n"
              << "  Submitted:      " << result.submitted << "\n"
              << "  Accepted:       " << result.accepted << "\n"
              << "  Rate-limited:   " << result.rateLimited << "\n"
              << "  BP rejected:    " << result.backpressureRejected << "\n"
              << "  Queue dropped:  " << result.dropped << "\n"
              << "  Book rejected:  " << result.bookRejected << "\n"
              << "  STP-cancelled:  " << result.stpCancelled << "\n"
              << "  Wall time:      " << std::fixed << std::setprecision(3)
              << result.wallSeconds << " s\n";

    double acceptedThroughput =
        (result.wallSeconds > 0.0)
            ? static_cast<double>(result.accepted) / result.wallSeconds
            : 0.0;
    std::cout << "  Accepted TPS:   " << std::setprecision(2)
              << (acceptedThroughput / 1e6) << " M ops/sec\n";

    if (result.latency.getCount() == 0) {
        std::cout << "  Latency:        no accepted orders\n";
        return;
    }

    std::cout << "  Mean:           " << formatLatency(static_cast<uint64_t>(result.latency.getMean())) << "\n"
              << "  P50:            " << formatLatency(result.latency.getP50()) << "\n"
              << "  P90:            " << formatLatency(result.latency.getP90()) << "\n"
              << "  P99:            " << formatLatency(result.latency.getP99()) << "\n"
              << "  P99.9:          " << formatLatency(result.latency.getP999()) << "\n"
              << "  Max:            " << formatLatency(result.latency.getMax()) << "\n";
}

ScenarioResult finalizeScenario(MatchingEngine& engine, const std::string& name,
                                uint64_t submitted,
                                std::chrono::steady_clock::time_point start,
                                const bench::BookOutcomes& outcomes) {
    engine.waitForDrain();
    auto end = std::chrono::steady_clock::now();

    ScenarioResult result;
    result.name = name;
    result.submitted = submitted;
    result.accepted = engine.getProcessedCount();
    result.rateLimited = engine.getRateLimitedCount();
    result.backpressureRejected = engine.getBackpressureRejectCount();
    result.dropped = engine.getDroppedCount();
    result.wallSeconds = std::chrono::duration<double>(end - start).count();
    result.latency = engine.getAggregateE2ELatency();
    engine.stopAsync();
    result.bookRejected = outcomes.rejected.load();
    result.stpCancelled = outcomes.stpCancelled.load();
    return result;
}

ScenarioResult runSingleOrderScenario(size_t samples, size_t numSymbols, size_t engineThreads) {
    bench::BookOutcomes outcomes;  // declared first: outlives the engine's books
    MatchingEngine engine;
    for (size_t s = 0; s < numSymbols; ++s) {
        engine.addSymbol(static_cast<SymbolId>(s));
        engine.getOrderBook(static_cast<SymbolId>(s))->setEventListener(&outcomes);
    }
    seedLiquidity(engine, numSymbols);
    engine.startAsync(engineThreads, 1024);

    std::mt19937 rng(42);
    std::uniform_int_distribution<uint32_t> symbolDist(0, numSymbols - 1);
    uint64_t nextOrderId = 1'000'000;

    auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < samples; ++i) {
        SymbolId symbolId = static_cast<SymbolId>(symbolDist(rng));
        Side side = sideFor(i);
        Price price = priceFor(i);

        engine.processOrder(symbolId, nextOrderId++, (i % 8) + 1, side, price, 10,
                            OrderType::Limit);
        engine.waitForDrain();
    }

    return finalizeScenario(engine, "Single-order E2E (queue drained between submits)",
                            samples, start, outcomes);
}

ScenarioResult runBurstScenario(size_t burstSize, size_t numSymbols, size_t engineThreads) {
    bench::BookOutcomes outcomes;  // declared first: outlives the engine's books
    MatchingEngine engine;
    for (size_t s = 0; s < numSymbols; ++s) {
        engine.addSymbol(static_cast<SymbolId>(s));
        engine.getOrderBook(static_cast<SymbolId>(s))->setEventListener(&outcomes);
    }
    seedLiquidity(engine, numSymbols);
    engine.startAsync(engineThreads, 32768);

    std::mt19937 rng(42);
    std::uniform_int_distribution<uint32_t> symbolDist(0, numSymbols - 1);
    uint64_t nextOrderId = 2'000'000;

    auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < burstSize; ++i) {
        SymbolId symbolId = static_cast<SymbolId>(symbolDist(rng));
        Side side = sideFor(i);
        Price price = priceFor(i);

        engine.processOrder(symbolId, nextOrderId++, (i % 10) + 1, side, price, 10,
                            OrderType::Limit);
    }

    return finalizeScenario(engine, "Burst E2E (10K queued immediately)", burstSize, start,
                            outcomes);
}

ScenarioResult runRateLimitedScenario(size_t numProducers, size_t ordersPerProducer,
                                      size_t numSymbols, size_t engineThreads,
                                      uint64_t ratePerParticipant, uint64_t burstSize,
                                      double producerRateFraction,
                                      double backpressureThreshold) {
    bench::BookOutcomes outcomes;  // declared first: outlives the engine's books
    MatchingEngine engine;
    for (size_t s = 0; s < numSymbols; ++s) {
        engine.addSymbol(static_cast<SymbolId>(s));
        engine.getOrderBook(static_cast<SymbolId>(s))->setEventListener(&outcomes);
    }
    seedLiquidity(engine, numSymbols);
    engine.setRateLimit(ratePerParticipant, burstSize);
    engine.setBackpressureThreshold(backpressureThreshold);
    engine.startAsync(engineThreads, 8192);

    const uint64_t submitted = numProducers * ordersPerProducer;
    const double producerRate = static_cast<double>(ratePerParticipant) * producerRateFraction;
    const auto sendInterval =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / producerRate));

    std::atomic<bool> go{false};
    std::atomic<uint64_t> nextOrderId{3'000'000};
    const auto startAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);

    std::vector<std::thread> producers;
    producers.reserve(numProducers);

    for (size_t producer = 0; producer < numProducers; ++producer) {
        producers.emplace_back([&, producer]() {
            std::mt19937 rng(42 + static_cast<uint32_t>(producer));
            std::uniform_int_distribution<uint32_t> symbolDist(0, numSymbols - 1);

            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            auto nextSend = startAt;
            for (size_t i = 0; i < ordersPerProducer; ++i) {
                paceUntil(nextSend);

                SymbolId symbolId = static_cast<SymbolId>(symbolDist(rng));
                Side side = sideFor(i);
                Price price = priceFor(i);

                engine.processOrder(symbolId, nextOrderId.fetch_add(1), producer + 1, side,
                                    price, 10, OrderType::Limit);
                nextSend += sendInterval;
            }
        });
    }

    auto start = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);

    for (auto& producer : producers) {
        producer.join();
    }

    std::string name = "Rate-limited E2E (" + std::to_string(numProducers)
        + " producers, limit=" + std::to_string(ratePerParticipant) + "/s, pace="
        + std::to_string(static_cast<int>(producerRateFraction * 100)) + "%)";
    return finalizeScenario(engine, name, submitted, start, outcomes);
}

} // namespace

int main() {
    std::cout << "╔═══════════════════════════════════════════════════════╗\n"
              << "║        END-TO-END LATENCY BENCHMARK                   ║\n"
              << "╠═══════════════════════════════════════════════════════╣\n"
              << "║  CPU:  " << getCPUName() << "\n"
              << "║  Measures: ingress -> processing complete             ║\n"
              << "║  Includes: queueing delay + processing time           ║\n"
              << "╚═══════════════════════════════════════════════════════╝\n";

    // Three in four orders rest and nothing cancels, so the largest scenario
    // leaves ~13,000 live orders per book: the default 10,000-slot pool shed
    // 17,997 of its 60,000 orders as PoolCapacityExceeded. An explicit
    // OB_ORDER_POOL_CAPACITY from the caller still wins.
    ::setenv("OB_ORDER_POOL_CAPACITY", "32768", /*overwrite=*/0);

    bool workloadOk = true;
    // A scenario's latencies describe its order mix only if no book shed or
    // STP-cancelled an order the engine had "Accepted" (queued).
    auto report = [&](const ScenarioResult& r) {
        printScenario(r);
        workloadOk = workloadOk && r.bookRejected == 0 && r.stpCancelled == 0;
    };

    report(runSingleOrderScenario(
        /*samples=*/10000,
        /*numSymbols=*/4,
        /*engineThreads=*/2
    ));

    report(runBurstScenario(
        /*burstSize=*/10000,
        /*numSymbols=*/4,
        /*engineThreads=*/2
    ));

    report(runRateLimitedScenario(
        /*numProducers=*/10,
        /*ordersPerProducer=*/2000,
        /*numSymbols=*/4,
        /*engineThreads=*/2,
        /*ratePerParticipant=*/5000,
        /*burstSize=*/50,
        /*producerRateFraction=*/0.80,
        /*backpressureThreshold=*/0.80
    ));

    report(runRateLimitedScenario(
        /*numProducers=*/20,
        /*ordersPerProducer=*/3000,
        /*numSymbols=*/4,
        /*engineThreads=*/4,
        /*ratePerParticipant=*/10000,
        /*burstSize=*/100,
        /*producerRateFraction=*/0.85,
        /*backpressureThreshold=*/0.80
    ));

    std::cout << "\n═══════════════════════════════════════════════════════\n"
              << "All E2E benchmarks complete.\n"
              << "═══════════════════════════════════════════════════════\n";
    if (!workloadOk) {
        std::cerr << "WORKLOAD CHECK FAILED: a book rejected or STP-cancelled orders\n";
        return 1;
    }
    return 0;
}
