// RiskLedgerTest — roadmap 1.9 I2, I3, I6: risk on the admission path.
//
// One reservation ledger per participant and side: admission reserves an
// order's full quantity, a fill turns it into position, and whatever leaves
// without trading (cancel, IOC/FOK remainder, STP cut, kill, expiry, DAY close,
// replace, modify) gives it back through one hook in OrderBook. Each case runs
// on both engine paths: sync matches on the caller's thread, async on a worker.

#include "MatchingEngine.h"
#include "OrderBook.h"
#include "TempPath.h"
#include "Types.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace OrderMatcher;

namespace {

constexpr SymbolId kSym = 1;
constexpr Price kPx = 1'000'000;
constexpr ParticipantId kA = 1;
constexpr ParticipantId kB = 2;

}  // namespace

// ─── STP-9: checkRiskLimits is side-aware ───────────────────────────────────

TEST(BookPositionLimit, SellReducingALongIsNotTreatedAsIncreasingIt) {
    OrderBook book(kSym);
    book.setCircuitBreakerThreshold(1e9);
    book.setRiskLimits(kA, RiskLimits{0, 0, /*maxPositionSize=*/100});

    // A is long 80.
    ASSERT_TRUE(std::holds_alternative<OrderId>(
        book.addOrder(1, kB, Side::Sell, kPx, 80, OrderType::Limit)));
    ASSERT_TRUE(std::holds_alternative<OrderId>(
        book.addOrder(2, kA, Side::Buy, kPx, 80, OrderType::Limit)));
    ASSERT_EQ(book.getParticipantStats(kA).netPosition, 80);

    // Selling 150 leaves it short 70, inside the limit. Read side-blind it was
    // 80 + 150 = 230.
    EXPECT_TRUE(std::holds_alternative<OrderId>(
        book.addOrder(3, kA, Side::Sell, kPx + 100, 150, OrderType::Limit)));
    // Selling 200 leaves it short 120.
    EXPECT_FALSE(std::holds_alternative<OrderId>(
        book.addOrder(4, kA, Side::Sell, kPx + 100, 200, OrderType::Limit)));
    // Buying 30 takes it to 110.
    EXPECT_FALSE(std::holds_alternative<OrderId>(
        book.addOrder(5, kA, Side::Buy, kPx - 100, 30, OrderType::Limit)));
}

// ─── Both engine paths ─────────────────────────────────────────────────────

namespace {

constexpr SymbolId kSym2 = 2;

// A book with the breaker out of the way, so a deliberately
// crossing order is admitted.
class RiskPath : public ::testing::TestWithParam<bool> {
protected:
    MatchingEngine engine;
    void SetUp() override {
        for (SymbolId s : {kSym, kSym2}) {
            engine.addSymbol(s);
            engine.getOrderBook(s)->setCircuitBreakerThreshold(1e9);
        }
        if (async()) engine.startAsync(2, 4096); else engine.start();
    }
    bool async() const { return GetParam(); }
    void drain() { if (async()) engine.waitForDrain(); }
    OrderBook& book(SymbolId s = kSym) { return *engine.getOrderBook(s); }

    SubmitResult add(OrderId id, ParticipantId pid, Side side, Price px, Quantity qty,
                     OrderType type = OrderType::Limit) {
        return engine.submitOrder(kSym, id, pid, side, px, qty, type);
    }
    Quantity leaves(OrderId id, SymbolId s = kSym) {
        const Order* o = book(s).getOrder(id);
        return o ? o->remainingQty : 0;
    }
    // The async path acks at the queue, so only the sync path can answer here;
    // both end with the book unchanged, which each case checks after a drain.
    void expectRefused(const SubmitResult& r, RejectReason reason) {
        if (!async()) EXPECT_EQ(r.rejectReason, reason);
    }
};

INSTANTIATE_TEST_SUITE_P(BothPaths, RiskPath, ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& i) {
                             return i.param ? "Async" : "Sync";
                         });

}  // namespace

// ─── THR-3: reservation at admission ────────────────────────────────────────

// Two working orders on opposite sides do not net against each other: either
// can fill alone. Net of working, a participant could stack buy 100 / sell 100
// without end against a limit of 100.
TEST_P(RiskPath, WorkingOrdersDoNotNetAcrossSides) {
    engine.setPositionLimit(kA, 100);
    ASSERT_TRUE(add(1, kA, Side::Buy, kPx - 100, 100).isAccepted());
    ASSERT_TRUE(add(2, kA, Side::Sell, kPx + 100, 100).isAccepted());
    EXPECT_EQ(add(3, kA, Side::Buy, kPx - 100, 1).rejectReason,
              RejectReason::PositionLimitExceeded);
    EXPECT_EQ(add(4, kA, Side::Sell, kPx + 100, 1).rejectReason,
              RejectReason::PositionLimitExceeded);
    drain();
    EXPECT_EQ(leaves(1), 100u);
    EXPECT_EQ(leaves(2), 100u);
    EXPECT_EQ(leaves(3), 0u);
    EXPECT_EQ(leaves(4), 0u);
}

// Concurrent admissions: every producer can pass a check against the same
// position before any of them books. A buys and B sells across two symbols on
// two workers, so reservations race fills too. A only ever buys, so its filled
// position never falls and the final figure is its peak.
TEST_P(RiskPath, ConcurrentAdmissionsNeverExceedALimit) {
    constexpr int64_t kLimit = 100;
    constexpr int kThreads = 8;
    constexpr int kOrdersPerThread = 600;
    engine.setPositionLimit(kA, kLimit);
    engine.setPositionLimit(kB, kLimit);

    std::atomic<OrderId> nextId{1};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            while (!go.load(std::memory_order_acquire)) {}
            for (int i = 0; i < kOrdersPerThread; ++i) {
                const OrderId id = nextId.fetch_add(1, std::memory_order_relaxed);
                const SymbolId sym = (i % 2) ? kSym : kSym2;
                const ParticipantId pid = (t % 2) ? kA : kB;
                const Side side = (t % 2) ? Side::Buy : Side::Sell;
                (void)engine.submitOrder(sym, id, pid, side, kPx, 1, OrderType::Limit);
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();
    drain();

    int64_t aFilled = 0, bFilled = 0;
    for (SymbolId s : {kSym, kSym2}) {
        aFilled += book(s).getParticipantStats(kA).netPosition;
        bFilled += book(s).getParticipantStats(kB).netPosition;
    }
    EXPECT_LE(aFilled, kLimit);
    EXPECT_GE(bFilled, -kLimit);
    EXPECT_LE(engine.getPosition(kA), kLimit);
    EXPECT_GE(engine.getPosition(kB), -kLimit);
    // No order may rest past the limit either.
    Quantity aResting = 0, bResting = 0;
    for (SymbolId s : {kSym, kSym2}) {
        book(s).forEachOrderLocked([&](const Order& o) {
            (o.participantId == kA ? aResting : bResting) += o.remainingQty;
        });
    }
    EXPECT_LE(aFilled + static_cast<int64_t>(aResting), kLimit);
    EXPECT_LE(static_cast<int64_t>(bResting) - bFilled, kLimit);
}
