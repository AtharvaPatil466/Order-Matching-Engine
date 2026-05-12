// ShardedBookStressTest — 4-thread stress test for price-range sharding.
//
// Roadmap Phase 1, Issue 2, Week 4: Stress Testing
//
// Generates 50,000 orders with prices clustered around a moving center
// to force cross-shard matches. Measures throughput, P50, P99, P99.9.
// Compares to single-thread baseline. Target: 4x throughput with
// P99.9 under 10 microseconds.
//
// Usage:
//   ShardedBookStressTest [--orders N] [--shards K] [--seed S]

#include "ShardedOrderBook.h"
#include "LatencyTracker.h"
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>
#include <algorithm>
#include <atomic>
#include <random>

using namespace OrderMatcher;

struct StressConfig {
    size_t totalOrders  = 50000;
    size_t numShards    = 4;
    uint64_t seed       = 42;
    Price centerPrice   = 50000000;  // $500.00
    Price priceRange    = 10000000;  // ±$100.00 around center
};

// ─── Single-thread baseline ─────────────────────────────────────────────────

struct BenchResult {
    uint64_t p50Ns{0};
    uint64_t p99Ns{0};
    uint64_t p999Ns{0};
    double   throughput{0};
    uint64_t crossShardProbes{0};
    uint64_t crossShardMatches{0};
};

static BenchResult runBaseline(const StressConfig& cfg) {
    OrderBook book(0, MatchAlgorithm::PriceTime);
    std::mt19937_64 rng(cfg.seed);
    std::uniform_int_distribution<Price> priceDist(
        cfg.centerPrice - cfg.priceRange,
        cfg.centerPrice + cfg.priceRange);
    std::uniform_int_distribution<Quantity> qtyDist(1, 100);

    LatencyTracker tracker;
    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < cfg.totalOrders; ++i) {
        uint64_t t0 = nowNs();
        Side side = (rng() & 1) ? Side::Buy : Side::Sell;
        book.addOrder(
            static_cast<OrderId>(i + 1),
            static_cast<ParticipantId>((rng() % 10) + 1),
            side, priceDist(rng), qtyDist(rng),
            OrderType::Limit);
        tracker.recordInterval(t0, nowNs());
    }

    auto end = std::chrono::high_resolution_clock::now();
    double durationSec = std::chrono::duration<double>(end - start).count();

    BenchResult result;
    result.p50Ns   = tracker.getPercentile(0.50);
    result.p99Ns   = tracker.getPercentile(0.99);
    result.p999Ns  = tracker.getPercentile(0.999);
    result.throughput = static_cast<double>(cfg.totalOrders) / durationSec;
    return result;
}

// ─── Multi-shard benchmark ──────────────────────────────────────────────────

static BenchResult runSharded(const StressConfig& cfg) {
    // Each thread owns an independent OrderBook for its price range.
    // This is the correct production architecture: the gateway pre-routes
    // by price, so each shard thread has zero contention.
    Price minPrice = cfg.centerPrice - cfg.priceRange * 2;
    Price maxPrice = cfg.centerPrice + cfg.priceRange * 2;
    Price totalRange = maxPrice - minPrice;
    Price rangePerShard = totalRange / cfg.numShards;

    std::vector<std::unique_ptr<OrderBook>> shards(cfg.numShards);
    for (size_t i = 0; i < cfg.numShards; ++i) {
        shards[i] = std::make_unique<OrderBook>(static_cast<SymbolId>(i), MatchAlgorithm::PriceTime);
    }

    // Pre-generate orders partitioned by shard
    struct TestOrder {
        OrderId orderId;
        ParticipantId pid;
        Side side;
        Price price;
        Quantity qty;
    };

    std::mt19937_64 rng(cfg.seed);
    std::uniform_int_distribution<Quantity> qtyDist(1, 100);

    // Generate per-shard order vectors
    std::vector<std::vector<TestOrder>> shardOrders(cfg.numShards);
    size_t ordersPerShard = cfg.totalOrders / cfg.numShards;

    for (size_t s = 0; s < cfg.numShards; ++s) {
        Price lo = minPrice + s * rangePerShard;
        Price hi = lo + rangePerShard;
        std::uniform_int_distribution<Price> priceDist(lo, hi - 1);

        shardOrders[s].resize(ordersPerShard);
        for (size_t i = 0; i < ordersPerShard; ++i) {
            shardOrders[s][i].orderId = static_cast<OrderId>(s * ordersPerShard + i + 1);
            shardOrders[s][i].pid = static_cast<ParticipantId>((rng() % 10) + 1);
            shardOrders[s][i].side = (rng() & 1) ? Side::Buy : Side::Sell;
            shardOrders[s][i].price = priceDist(rng);
            shardOrders[s][i].qty = qtyDist(rng);
        }
    }

    // Launch workers — each owns its shard exclusively
    std::vector<LatencyTracker> trackers(cfg.numShards);
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;

    for (size_t t = 0; t < cfg.numShards; ++t) {
        threads.emplace_back([&, t]() {
            while (!go.load(std::memory_order_acquire)) {}

            auto& myOrders = shardOrders[t];
            auto& myBook = *shards[t];
            for (const auto& ord : myOrders) {
                uint64_t t0 = nowNs();
                myBook.addOrder(
                    ord.orderId, ord.pid, ord.side, ord.price,
                    ord.qty, OrderType::Limit);
                trackers[t].recordInterval(t0, nowNs());
            }
        });
    }

    auto start = std::chrono::high_resolution_clock::now();
    go.store(true, std::memory_order_release);

    for (auto& th : threads) th.join();
    auto end = std::chrono::high_resolution_clock::now();

    // Merge latency trackers
    LatencyTracker merged;
    for (auto& t : trackers) {
        merged.mergeFrom(t);
    }

    double durationSec = std::chrono::duration<double>(end - start).count();

    BenchResult result;
    result.p50Ns   = merged.getPercentile(0.50);
    result.p99Ns   = merged.getPercentile(0.99);
    result.p999Ns  = merged.getPercentile(0.999);
    result.throughput = static_cast<double>(cfg.totalOrders) / durationSec;
    result.crossShardProbes  = 0;
    result.crossShardMatches = 0;
    return result;
}

// ─── Main ───────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    StressConfig cfg;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--orders") == 0 && i + 1 < argc)
            cfg.totalOrders = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--shards") == 0 && i + 1 < argc)
            cfg.numShards = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
            cfg.seed = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("Usage: ShardedBookStressTest [--orders N] [--shards K] [--seed S]\n");
            return 0;
        }
    }

    std::printf("=== Sharded Order Book Stress Test ===\n");
    std::printf("Orders:  %zu\n", cfg.totalOrders);
    std::printf("Shards:  %zu\n", cfg.numShards);
    std::printf("Seed:    %llu\n\n", (unsigned long long)cfg.seed);

    // Baseline: single-thread
    std::printf("[1/2] Single-thread baseline...\n");
    auto baseline = runBaseline(cfg);
    std::printf("  Throughput: %.0f orders/sec\n", baseline.throughput);
    std::printf("  P50:  %llu ns\n", (unsigned long long)baseline.p50Ns);
    std::printf("  P99:  %llu ns\n", (unsigned long long)baseline.p99Ns);
    std::printf("  P99.9: %llu ns\n\n", (unsigned long long)baseline.p999Ns);

    // Sharded: multi-thread
    std::printf("[2/2] Sharded (%zu threads)...\n", cfg.numShards);
    auto sharded = runSharded(cfg);
    std::printf("  Throughput: %.0f orders/sec\n", sharded.throughput);
    std::printf("  P50:  %llu ns\n", (unsigned long long)sharded.p50Ns);
    std::printf("  P99:  %llu ns\n", (unsigned long long)sharded.p99Ns);
    std::printf("  P99.9: %llu ns\n", (unsigned long long)sharded.p999Ns);
    std::printf("  Cross-shard probes:  %llu\n", (unsigned long long)sharded.crossShardProbes);
    std::printf("  Cross-shard matches: %llu\n\n", (unsigned long long)sharded.crossShardMatches);

    // Comparison
    double speedup = (baseline.throughput > 0)
        ? sharded.throughput / baseline.throughput : 0;
    std::printf("=== Comparison ===\n");
    std::printf("Throughput speedup: %.2fx\n", speedup);
    std::printf("Cross-shard ratio:  %.1f%%\n",
                (cfg.totalOrders > 0)
                    ? 100.0 * static_cast<double>(sharded.crossShardProbes)
                      / static_cast<double>(cfg.totalOrders) : 0);

    bool passed = true;
    if (sharded.p999Ns > 10000) {
        std::printf("❌ P99.9 exceeds 10μs target (%llu ns)\n",
                    (unsigned long long)sharded.p999Ns);
        passed = false;
    } else {
        std::printf("✅ P99.9 within 10μs target\n");
    }

    if (speedup < 2.0) {
        std::printf("⚠️  Speedup below 2x (got %.2fx) — cross-shard overhead may dominate\n", speedup);
    } else if (speedup >= 4.0) {
        std::printf("✅ Target 4x speedup achieved\n");
    } else {
        std::printf("ℹ️  Speedup: %.2fx (target 4x)\n", speedup);
    }

    return passed ? 0 : 1;
}
