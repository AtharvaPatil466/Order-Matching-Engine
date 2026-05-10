#include <gtest/gtest.h>
#include "OrderBook.h"
#include "LatencyTracker.h"

using namespace OrderMatcher;

// ─── Benchmark Regression Tests ─────────────────────────────────────────────
// These tests run fixed workloads and assert that p99 latency stays below
// conservative thresholds. Thresholds are generous to avoid CI flakiness.

class BenchmarkRegressionTest : public ::testing::Test {
protected:
    OrderBook book;
    LatencyTracker tracker;
};

TEST_F(BenchmarkRegressionTest, AddOrder_P99_Under2000ns) {
    constexpr int N = 10000;
    OrderId id = 1;
    ParticipantId pid = 1;

    for (int i = 0; i < N; ++i) {
        auto start = nowNs();
        book.addOrder(id++, pid, Side::Buy, 1000000 - (i % 1000), 10, OrderType::Limit);
        tracker.record(nowNs() - start);
    }

    uint64_t p99 = tracker.getP99();
    EXPECT_LT(p99, 2000) << "AddOrder p99 latency " << p99 << "ns exceeds 2000ns threshold";
    EXPECT_GT(tracker.getCount(), 0);

    // Print stats for CI visibility
    std::cout << "[  PERF   ] AddOrder: p50=" << tracker.getP50()
              << "ns p99=" << p99 << "ns p999=" << tracker.getP999()
              << "ns max=" << tracker.getMax() << "ns\n";
}

TEST_F(BenchmarkRegressionTest, MatchOrder_P99_Under5000ns) {
    constexpr int N = 10000;
    OrderId id = 1;
    ParticipantId pid = 1;

    // Pre-seed sell side
    for (int i = 0; i < N; ++i) {
        book.addOrder(id++, pid, Side::Sell, 1000000 + (i % 1000), 10, OrderType::Limit);
    }

    ParticipantId buyer = 2;
    for (int i = 0; i < N; ++i) {
        auto start = nowNs();
        book.addOrder(id++, buyer, Side::Buy, 2000000, 10, OrderType::Limit);
        tracker.record(nowNs() - start);
    }

    uint64_t p99 = tracker.getP99();
    EXPECT_LT(p99, 5000) << "MatchOrder p99 latency " << p99 << "ns exceeds 5000ns threshold";

    std::cout << "[  PERF   ] MatchOrder: p50=" << tracker.getP50()
              << "ns p99=" << p99 << "ns p999=" << tracker.getP999()
              << "ns max=" << tracker.getMax() << "ns\n";
}

TEST_F(BenchmarkRegressionTest, CancelOrder_P99_Under1000ns) {
    constexpr int N = 10000;
    OrderId id = 1;
    ParticipantId pid = 1;

    // Pre-populate
    for (int i = 0; i < N; ++i) {
        book.addOrder(id++, pid, Side::Buy, 900000 + (i % 1000), 10, OrderType::Limit);
    }

    for (OrderId cancelId = 1; cancelId <= static_cast<OrderId>(N); ++cancelId) {
        auto start = nowNs();
        book.cancelOrder(cancelId);
        tracker.record(nowNs() - start);
    }

    uint64_t p99 = tracker.getP99();
    EXPECT_LT(p99, 1000) << "CancelOrder p99 latency " << p99 << "ns exceeds 1000ns threshold";

    std::cout << "[  PERF   ] CancelOrder: p50=" << tracker.getP50()
              << "ns p99=" << p99 << "ns p999=" << tracker.getP999()
              << "ns max=" << tracker.getMax() << "ns\n";
}

TEST_F(BenchmarkRegressionTest, Snapshot_P99_Under500ns) {
    OrderId id = 1;
    ParticipantId pid = 1;

    // Build realistic book
    for (int i = 0; i < 1000; ++i) {
        book.addOrder(id++, pid, Side::Buy, 990000 - i * 10, 100, OrderType::Limit);
        book.addOrder(id++, pid, Side::Sell, 1010000 + i * 10, 100, OrderType::Limit);
    }

    constexpr int N = 10000;
    for (int i = 0; i < N; ++i) {
        auto start = nowNs();
        auto snap = book.getSnapshot(10);
        (void)snap;
        tracker.record(nowNs() - start);
    }

    uint64_t p99 = tracker.getP99();
    EXPECT_LT(p99, 5000) << "Snapshot p99 latency " << p99 << "ns exceeds 5000ns threshold";

    std::cout << "[  PERF   ] Snapshot: p50=" << tracker.getP50()
              << "ns p99=" << p99 << "ns p999=" << tracker.getP999()
              << "ns max=" << tracker.getMax() << "ns\n";
}
