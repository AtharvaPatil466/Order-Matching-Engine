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
