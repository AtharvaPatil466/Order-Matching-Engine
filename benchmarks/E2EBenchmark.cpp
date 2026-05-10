#include "MatchingEngine.h"
#include "LatencyTracker.h"
#include <atomic>
#include <chrono>
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

struct ScenarioResult {
    std::string name;
    uint64_t submitted = 0;
    uint64_t accepted = 0;
    uint64_t rateLimited = 0;
    uint64_t backpressureRejected = 0;
    uint64_t dropped = 0;
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
                                std::chrono::steady_clock::time_point start) {
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
    return result;
}

ScenarioResult runSingleOrderScenario(size_t samples, size_t numSymbols, size_t engineThreads) {
    MatchingEngine engine;
    for (size_t s = 0; s < numSymbols; ++s) {
        engine.addSymbol(static_cast<SymbolId>(s));
    }
    seedLiquidity(engine, numSymbols);
    engine.startAsync(engineThreads, 1024);

    std::mt19937 rng(42);
    std::uniform_int_distribution<uint32_t> symbolDist(0, numSymbols - 1);
    uint64_t nextOrderId = 1'000'000;

    auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < samples; ++i) {
        SymbolId symbolId = static_cast<SymbolId>(symbolDist(rng));
        Side side = (i % 2 == 0) ? Side::Buy : Side::Sell;
        Price price = (i % 4 == 0)
            ? ((side == Side::Buy) ? 1030000 : 970000)
            : ((side == Side::Buy) ? (975000 + (i % 1000)) : (1025000 - (i % 1000)));

        engine.processOrder(symbolId, nextOrderId++, (i % 8) + 1, side, price, 10,
                            OrderType::Limit);
        engine.waitForDrain();
    }

    return finalizeScenario(engine, "Single-order E2E (queue drained between submits)",
                            samples, start);
}

ScenarioResult runBurstScenario(size_t burstSize, size_t numSymbols, size_t engineThreads) {
    MatchingEngine engine;
    for (size_t s = 0; s < numSymbols; ++s) {
        engine.addSymbol(static_cast<SymbolId>(s));
    }
    seedLiquidity(engine, numSymbols);
    engine.startAsync(engineThreads, 32768);

    std::mt19937 rng(42);
    std::uniform_int_distribution<uint32_t> symbolDist(0, numSymbols - 1);
    uint64_t nextOrderId = 2'000'000;

    auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < burstSize; ++i) {
        SymbolId symbolId = static_cast<SymbolId>(symbolDist(rng));
        Side side = (i % 2 == 0) ? Side::Buy : Side::Sell;
        Price price = (i % 4 == 0)
            ? ((side == Side::Buy) ? 1030000 : 970000)
            : ((side == Side::Buy) ? (975000 + (i % 1000)) : (1025000 - (i % 1000)));

        engine.processOrder(symbolId, nextOrderId++, (i % 10) + 1, side, price, 10,
                            OrderType::Limit);
    }

    return finalizeScenario(engine, "Burst E2E (10K queued immediately)", burstSize, start);
}

ScenarioResult runRateLimitedScenario(size_t numProducers, size_t ordersPerProducer,
                                      size_t numSymbols, size_t engineThreads,
                                      uint64_t ratePerParticipant, uint64_t burstSize,
                                      double producerRateFraction,
                                      double backpressureThreshold) {
    MatchingEngine engine;
    for (size_t s = 0; s < numSymbols; ++s) {
        engine.addSymbol(static_cast<SymbolId>(s));
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
                Side side = (i % 2 == 0) ? Side::Buy : Side::Sell;
                Price price = (i % 4 == 0)
                    ? ((side == Side::Buy) ? 1030000 : 970000)
                    : ((side == Side::Buy) ? (975000 + (i % 1000))
                                           : (1025000 - (i % 1000)));

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
    return finalizeScenario(engine, name, submitted, start);
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

    printScenario(runSingleOrderScenario(
        /*samples=*/10000,
        /*numSymbols=*/4,
        /*engineThreads=*/2
    ));

    printScenario(runBurstScenario(
        /*burstSize=*/10000,
        /*numSymbols=*/4,
        /*engineThreads=*/2
    ));

    printScenario(runRateLimitedScenario(
        /*numProducers=*/10,
        /*ordersPerProducer=*/2000,
        /*numSymbols=*/4,
        /*engineThreads=*/2,
        /*ratePerParticipant=*/5000,
        /*burstSize=*/50,
        /*producerRateFraction=*/0.80,
        /*backpressureThreshold=*/0.80
    ));

    printScenario(runRateLimitedScenario(
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
    return 0;
}
