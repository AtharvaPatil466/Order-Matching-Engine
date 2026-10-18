#include <gtest/gtest.h>
#include "OrderBook.h"
#include "LatencyTracker.h"

#include <variant>
#include <vector>

using namespace OrderMatcher;

// ─── Benchmark Regression Tests ─────────────────────────────────────────────
// These tests run fixed workloads and assert that p99 latency stays below
// conservative thresholds. Thresholds are generous to avoid CI flakiness.
//
// Thresholds are calibrated for an optimized Release build on x86 bare metal.
// Debug runs 3-5x slower and fault injection adds per-op hot-path overhead, so
// enforcing them there yields meaningless false failures. CMake defines
// OB_ENFORCE_LATENCY_THRESHOLD only for a Release, no-FI build; elsewhere each
// test still runs its workload and prints its numbers, then GTEST_SKIPs instead
// of asserting. (Not keyed off NDEBUG: ENABLE_ASSERTIONS=ON compiles -UNDEBUG,
// so NDEBUG is undefined even in Release.)
#ifdef OB_ENFORCE_LATENCY_THRESHOLD
#define OB_EXPECT_P99_UNDER(p99, bound, label)                                  \
    EXPECT_LT((p99), (bound)) << label " p99 latency " << (p99)                 \
                              << "ns exceeds " #bound "ns threshold"
#else
#define OB_EXPECT_P99_UNDER(p99, bound, label)                                  \
    GTEST_SKIP() << "latency threshold enforced only in a Release build "       \
                    "without fault injection (see file header); this build's "  \
                    "timings are not comparable to the x86 Release calibration"
#endif

// Each gate asserts its workload before its latency (GTPERF-1): a gate whose
// orders are all rejected times the reject path, and a 10x slowdown in the
// code it claims to guard passes. These checks are not timing-dependent, so
// they run in every build, including the ones that skip the threshold.
class BenchmarkRegressionTest : public ::testing::Test {
protected:
    // The default 10,000-slot pool sheds new orders at 95% utilisation
    // (PoolCapacityExceeded), so 10,000 resting orders need a bigger pool.
    static constexpr size_t kPoolSlots = 20'000;
    OrderBook book{0, MatchAlgorithm::PriceTime, kPoolSlots};
    LatencyTracker tracker;

    static bool accepted(const AddOrderResult& r) {
        return std::holds_alternative<OrderId>(r);
    }
    size_t restingOrders() const {
        std::vector<const Order*> out(100'000);
        return book.getAllOrders(out.data(), out.size());
    }
};

TEST_F(BenchmarkRegressionTest, AddOrder_P99_Under2000ns) {
    constexpr int N = 10000;
    OrderId id = 1;
    ParticipantId pid = 1;

    int acceptedCount = 0;
    for (int i = 0; i < N; ++i) {
        auto start = nowNs();
        auto r = book.addOrder(id++, pid, Side::Buy, 1000000 - (i % 1000), 10, OrderType::Limit);
        tracker.record(nowNs() - start);
        acceptedCount += accepted(r);
    }
    ASSERT_EQ(acceptedCount, N) << "AddOrder timed rejects, not adds";
    ASSERT_EQ(restingOrders(), static_cast<size_t>(N));

    uint64_t p99 = tracker.getP99();

    // Print stats for CI visibility
    std::cout << "[  PERF   ] AddOrder: p50=" << tracker.getP50()
              << "ns p99=" << p99 << "ns p999=" << tracker.getP999()
              << "ns max=" << tracker.getMax() << "ns\n";

    OB_EXPECT_P99_UNDER(p99, 2000, "AddOrder");
}

TEST_F(BenchmarkRegressionTest, MatchOrder_P99_Under5000ns) {
    constexpr int N = 10000;
    OrderId id = 1;
    ParticipantId pid = 1;

    // Pre-seed sell side
    int seeded = 0;
    for (int i = 0; i < N; ++i) {
        seeded += accepted(book.addOrder(id++, pid, Side::Sell, 1000000 + (i % 1000), 10, OrderType::Limit));
    }
    ASSERT_EQ(seeded, N);

    // Each timed buy fully fills exactly one resting sell. The limit crosses
    // every seeded level but stays inside the 5% volatility breaker: at
    // 2,000,000 the breaker rejected all 10,000 buys.
    constexpr Price kBuyLimit = 1001000;
    ParticipantId buyer = 2;
    int acceptedCount = 0;
    for (int i = 0; i < N; ++i) {
        auto start = nowNs();
        auto r = book.addOrder(id++, buyer, Side::Buy, kBuyLimit, 10, OrderType::Limit);
        tracker.record(nowNs() - start);
        acceptedCount += accepted(r);
    }
    ASSERT_EQ(acceptedCount, N) << "MatchOrder timed rejects, not matches";
    ASSERT_EQ(book.getTradeCount(), static_cast<uint64_t>(N)) << "fill ratio below 100%";
    ASSERT_EQ(restingOrders(), 0u);

    uint64_t p99 = tracker.getP99();

    std::cout << "[  PERF   ] MatchOrder: p50=" << tracker.getP50()
              << "ns p99=" << p99 << "ns p999=" << tracker.getP999()
              << "ns max=" << tracker.getMax() << "ns\n";

    OB_EXPECT_P99_UNDER(p99, 5000, "MatchOrder");
}

TEST_F(BenchmarkRegressionTest, CancelOrder_P99_Under1000ns) {
    constexpr int N = 10000;
    OrderId id = 1;
    ParticipantId pid = 1;

    // Pre-populate
    int seeded = 0;
    for (int i = 0; i < N; ++i) {
        seeded += accepted(book.addOrder(id++, pid, Side::Buy, 900000 + (i % 1000), 10, OrderType::Limit));
    }
    ASSERT_EQ(seeded, N);

    for (OrderId cancelId = 1; cancelId <= static_cast<OrderId>(N); ++cancelId) {
        auto start = nowNs();
        book.cancelOrder(cancelId);
        tracker.record(nowNs() - start);
    }
    ASSERT_EQ(restingOrders(), 0u) << "some timed cancels were no-ops";

    uint64_t p99 = tracker.getP99();

    std::cout << "[  PERF   ] CancelOrder: p50=" << tracker.getP50()
              << "ns p99=" << p99 << "ns p999=" << tracker.getP999()
              << "ns max=" << tracker.getMax() << "ns\n";

    OB_EXPECT_P99_UNDER(p99, 1000, "CancelOrder");
}

TEST_F(BenchmarkRegressionTest, Snapshot_P99_Under500ns) {
    OrderId id = 1;
    ParticipantId pid = 1;

    // Build realistic book
    int seeded = 0;
    for (int i = 0; i < 1000; ++i) {
        seeded += accepted(book.addOrder(id++, pid, Side::Buy, 990000 - i * 10, 100, OrderType::Limit));
        seeded += accepted(book.addOrder(id++, pid, Side::Sell, 1010000 + i * 10, 100, OrderType::Limit));
    }
    ASSERT_EQ(seeded, 2000);

    constexpr int N = 10000;
    size_t levels = 0;
    for (int i = 0; i < N; ++i) {
        auto start = nowNs();
        auto snap = book.getSnapshot(10);
        tracker.record(nowNs() - start);
        levels += snap.bidCount + snap.askCount;
    }
    ASSERT_EQ(levels, static_cast<size_t>(N) * 20);

    uint64_t p99 = tracker.getP99();

    std::cout << "[  PERF   ] Snapshot: p50=" << tracker.getP50()
              << "ns p99=" << p99 << "ns p999=" << tracker.getP999()
              << "ns max=" << tracker.getMax() << "ns\n";

    OB_EXPECT_P99_UNDER(p99, 5000, "Snapshot");
}
