// DeterministicBenchmark — reproducible benchmark with HW timestamps.
//
// Roadmap Phase 5, Week 17: Deterministic Benchmark Harness
//
// Accepts a fixed seed, runs the exact same workload N times, reports
// P50, P99, P99.9, P99.99, P99.999 across all runs. Uses hardware
// timestamp counters (rdtsc on x86, cntvct_el0 on ARM).
//
// Outputs a JSON file with the full latency distribution.

#include "MatchingEngine.h"
#include "Utils.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace OrderMatcher;

// ─── Hardware timestamp counter ──────────────────────────────────────────

inline uint64_t hwTimestamp() {
#if defined(__x86_64__) || defined(__i386__)
    unsigned int lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
#elif defined(__aarch64__)
    uint64_t val;
    asm volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#else
    return static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
#endif
}

// Calibrate TSC to nanoseconds
double calibrateTscToNs() {
    constexpr int CALIBRATION_MS = 100;
    uint64_t tscStart = hwTimestamp();
    auto wallStart = std::chrono::high_resolution_clock::now();
    
    std::this_thread::sleep_for(std::chrono::milliseconds(CALIBRATION_MS));
    
    uint64_t tscEnd = hwTimestamp();
    auto wallEnd = std::chrono::high_resolution_clock::now();
    
    double wallNs = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            wallEnd - wallStart).count());
    double tscDelta = static_cast<double>(tscEnd - tscStart);
    
    return wallNs / tscDelta;  // ns per TSC tick
}

// ─── Percentile calculation ──────────────────────────────────────────────

struct LatencyDistribution {
    uint64_t count{0};
    double   p50{0};
    double   p99{0};
    double   p999{0};
    double   p9999{0};
    double   p99999{0};
    double   mean{0};
    double   minNs{0};
    double   maxNs{0};
    double   stddev{0};
};

LatencyDistribution computeDistribution(std::vector<double>& latencies) {
    LatencyDistribution dist;
    if (latencies.empty()) return dist;
    
    std::sort(latencies.begin(), latencies.end());
    dist.count = latencies.size();
    
    auto percentile = [&](double p) -> double {
        size_t idx = static_cast<size_t>(p * static_cast<double>(latencies.size() - 1));
        return latencies[idx];
    };
    
    dist.p50    = percentile(0.50);
    dist.p99    = percentile(0.99);
    dist.p999   = percentile(0.999);
    dist.p9999  = percentile(0.9999);
    dist.p99999 = percentile(0.99999);
    dist.minNs  = latencies.front();
    dist.maxNs  = latencies.back();
    
    // Mean
    double sum = 0;
    for (double l : latencies) sum += l;
    dist.mean = sum / static_cast<double>(dist.count);
    
    // Stddev
    double sqSum = 0;
    for (double l : latencies) {
        double diff = l - dist.mean;
        sqSum += diff * diff;
    }
    dist.stddev = std::sqrt(sqSum / static_cast<double>(dist.count));
    
    return dist;
}

// ─── Generate deterministic order flow ───────────────────────────────────

struct OrderSpec {
    Side      side;
    Price     price;
    Quantity  qty;
    OrderType type;
};

std::vector<OrderSpec> generateWorkload(uint64_t seed, size_t count) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> sideDist(0, 1);
    std::uniform_int_distribution<Price> priceDist(95'0000, 105'0000);  // $95–$105
    std::uniform_int_distribution<Quantity> qtyDist(1, 100);
    
    std::vector<OrderSpec> orders;
    orders.reserve(count);
    
    for (size_t i = 0; i < count; ++i) {
        OrderSpec spec;
        spec.side  = sideDist(rng) == 0 ? Side::Buy : Side::Sell;
        spec.price = priceDist(rng);
        spec.qty   = qtyDist(rng);
        spec.type  = OrderType::Limit;
        orders.push_back(spec);
    }
    
    return orders;
}

// ─── Write JSON output ──────────────────────────────────────────────────

// Returns false if the report could not be written. The caller must propagate
// that into the exit code: a benchmark that prints its results to stdout and
// silently fails to write its JSON looks identical to a successful run, and the
// CI gate then compares whatever partial set of files it happens to find. An
// ofstream that fails to open sets failbit and turns every subsequent
// operator<< into a no-op without raising, so the check has to be explicit.
bool writeJsonReport(const std::string& path,
                     const LatencyDistribution& dist,
                     uint64_t seed, int iterations,
                     double tscNsRatio, const std::string& mode) {
    std::ofstream out(path);
    if (!out) {
        std::perror(("Failed to open: " + path).c_str());
        return false;
    }
    out << "{\n";
    out << "  \"benchmark\": \"DeterministicBenchmark\",\n";
    out << "  \"seed\": " << seed << ",\n";
    out << "  \"iterations\": " << iterations << ",\n";
    out << "  \"mode\": \"" << mode << "\",\n";
    out << "  \"tsc_ns_ratio\": " << tscNsRatio << ",\n";
    out << "  \"count\": " << dist.count << ",\n";
    out << "  \"latency_ns\": {\n";
    out << "    \"p50\": " << dist.p50 << ",\n";
    out << "    \"p99\": " << dist.p99 << ",\n";
    out << "    \"p999\": " << dist.p999 << ",\n";
    out << "    \"p9999\": " << dist.p9999 << ",\n";
    out << "    \"p99999\": " << dist.p99999 << ",\n";
    out << "    \"mean\": " << dist.mean << ",\n";
    out << "    \"min\": " << dist.minNs << ",\n";
    out << "    \"max\": " << dist.maxNs << ",\n";
    out << "    \"stddev\": " << dist.stddev << "\n";
    out << "  }\n";
    out << "}\n";

    // Close explicitly and re-check: a full disk or a short write surfaces at
    // flush time, long after the stream was opened successfully.
    out.close();
    if (!out) {
        std::perror(("Failed to write: " + path).c_str());
        return false;
    }
    return true;
}

// ─── Main ───────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    uint64_t seed = 42;
    int iterations = 100;
    size_t ordersPerIteration = 10000;
    std::string outputPath = "benchmark_results.json";
    std::string mode = "full";
    
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = std::stoull(argv[++i]);
        } else if (std::strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            iterations = std::stoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--orders") == 0 && i + 1 < argc) {
            ordersPerIteration = std::stoull(argv[++i]);
        } else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            outputPath = argv[++i];
        } else if (std::strcmp(argv[i], "--lean") == 0) {
            mode = "lean";
        }
    }
    
    std::printf("=== Deterministic Benchmark ===\n");
    std::printf("Seed: %llu\n", static_cast<unsigned long long>(seed));
    std::printf("Iterations: %d\n", iterations);
    std::printf("Orders/iteration: %zu\n", ordersPerIteration);
    std::printf("Mode: %s\n\n", mode.c_str());
    
    // Calibrate TSC
    std::printf("Calibrating TSC...\n");
    double tscNsRatio = calibrateTscToNs();
    std::printf("TSC ratio: %.4f ns/tick\n\n", tscNsRatio);
    
    // Generate deterministic workload
    auto workload = generateWorkload(seed, ordersPerIteration);
    
    // Collect all latencies across all iterations
    std::vector<double> allLatencies;
    allLatencies.reserve(static_cast<size_t>(iterations) * ordersPerIteration);
    
    for (int iter = 0; iter < iterations; ++iter) {
        MatchingEngine engine;
        engine.addSymbol(1);
        engine.start();
        
        // Set up risk limits if in full mode
        if (mode == "full") {
            RiskLimits limits;
            limits.maxOrderSize = 1000;
            limits.maxOrderNotional = 200'0000 * 1000;
            limits.maxPositionSize = 10000;
            engine.setRiskLimits(1, 1, limits);
            engine.setRiskLimits(1, 2, limits);
            
            if (auto* book = engine.getOrderBook(1)) {
                book->setCircuitBreakerThreshold(0.10);
                book->setReferencePrice(100'0000);
            }
        }
        
        for (size_t i = 0; i < workload.size(); ++i) {
            const auto& spec = workload[i];
            
            uint64_t tscStart = hwTimestamp();
            
            engine.processOrder(1, static_cast<OrderId>(i + 1),
                               (i % 2) + 1,  // Alternate participants
                               spec.side, spec.price, spec.qty, spec.type);
            
            uint64_t tscEnd = hwTimestamp();
            double latencyNs = static_cast<double>(tscEnd - tscStart) * tscNsRatio;
            allLatencies.push_back(latencyNs);
        }
        
        engine.stop();
        
        if ((iter + 1) % 10 == 0) {
            std::printf("  Completed iteration %d/%d\n", iter + 1, iterations);
        }
    }
    
    // Compute distribution
    auto dist = computeDistribution(allLatencies);
    
    std::printf("\n=== Results (%s mode) ===\n", mode.c_str());
    std::printf("Total samples: %llu\n", static_cast<unsigned long long>(dist.count));
    std::printf("P50:     %.0f ns\n", dist.p50);
    std::printf("P99:     %.0f ns\n", dist.p99);
    std::printf("P99.9:   %.0f ns\n", dist.p999);
    std::printf("P99.99:  %.0f ns\n", dist.p9999);
    std::printf("P99.999: %.0f ns\n", dist.p99999);
    std::printf("Mean:    %.0f ns\n", dist.mean);
    std::printf("Min:     %.0f ns\n", dist.minNs);
    std::printf("Max:     %.0f ns\n", dist.maxNs);
    std::printf("Stddev:  %.0f ns\n", dist.stddev);
    
    // Write JSON. A failure here is fatal: the CI gate's entire input is this
    // file, so reporting success without producing it is how a partial run
    // silently shrinks the sample set one side is reduced over.
    if (!writeJsonReport(outputPath, dist, seed, iterations, tscNsRatio, mode)) {
        return 1;
    }
    std::printf("\nResults written to: %s\n", outputPath.c_str());

    return 0;
}
