// MarketCollarTest — roadmap 1.9 I4.
//
// MATCH-16: market protection was off by default and no binary turned it on, so
// one market order could walk the whole book; and pro-rata books ignored it even
// when it was set. It is now on by default (5%, like the circuit breaker) under
// both algorithms.

#include "OrderBook.h"

#include <gtest/gtest.h>

#include <limits>
#include <variant>

using namespace OrderMatcher;

namespace {

constexpr Price kTouch = 1'000'000;      // 100.0000
constexpr Price kCollarUp = 1'050'000;   // touch + 5%
constexpr Price kCollarDown = 950'000;   // touch - 5%

void rest(OrderBook& b, OrderId id, Side side, Price px, Quantity qty) {
    ASSERT_TRUE(std::holds_alternative<OrderId>(b.addOrder(id, 1, side, px, qty, OrderType::Limit)));
}

AddOrderResult market(OrderBook& b, OrderId id, Side side, Quantity qty, Quantity minQty = 0) {
    return b.addOrder(id, 9, side, 0, qty, OrderType::Market, 0, 0, TimeInForce::GTC, 0, 0,
                      PegType::None, 0, 0, minQty);
}

class Collar : public ::testing::TestWithParam<MatchAlgorithm> {};

// The collar is inclusive: at the bound trades, one tick past it does not.
TEST_P(Collar, MarketBuyStopsAtDefaultCollar) {
    OrderBook b(1, GetParam());
    rest(b, 1, Side::Sell, kTouch, 10);
    rest(b, 2, Side::Sell, kCollarUp, 10);
    rest(b, 3, Side::Sell, kCollarUp + 1, 10);

    ASSERT_TRUE(std::holds_alternative<OrderId>(market(b, 4, Side::Buy, 30)));

    EXPECT_EQ(b.getOrder(1), nullptr);
    EXPECT_EQ(b.getOrder(2), nullptr);
    ASSERT_NE(b.getOrder(3), nullptr);
    EXPECT_EQ(b.getOrder(3)->remainingQty, 10u);
    EXPECT_EQ(b.getOrder(4), nullptr) << "a market remainder is cancelled, never rested";
}

TEST_P(Collar, MarketSellStopsAtDefaultCollar) {
    OrderBook b(1, GetParam());
    rest(b, 1, Side::Buy, kTouch, 10);
    rest(b, 2, Side::Buy, kCollarDown, 10);
    rest(b, 3, Side::Buy, kCollarDown - 1, 10);

    ASSERT_TRUE(std::holds_alternative<OrderId>(market(b, 4, Side::Sell, 30)));

    EXPECT_EQ(b.getOrder(1), nullptr);
    EXPECT_EQ(b.getOrder(2), nullptr);
    ASSERT_NE(b.getOrder(3), nullptr);
    EXPECT_EQ(b.getOrder(3)->remainingQty, 10u);
}

// The dry run has to agree with the sweep, or a minimum is met with liquidity
// the order cannot reach and it trades less than the minimum.
TEST_P(Collar, MinQtyCountsOnlyWhatTheCollarLetsItReach) {
    OrderBook b(1, GetParam());
    rest(b, 1, Side::Sell, kTouch, 4);
    rest(b, 2, Side::Sell, kCollarUp + 1, 4);

    ASSERT_TRUE(std::holds_alternative<OrderId>(market(b, 3, Side::Buy, 8, /*minQty=*/8)));

    EXPECT_NE(b.getOrder(1), nullptr) << "minimum not reachable: cancelled untraded";
    EXPECT_NE(b.getOrder(2), nullptr);
}

TEST_P(Collar, ZeroDisablesIt) {
    OrderBook b(1, GetParam());
    b.setCircuitBreakerThreshold(1e9);
    b.setMarketProtectionPct(0.0);
    rest(b, 1, Side::Sell, kTouch, 10);
    rest(b, 2, Side::Sell, kTouch + 90'000, 10);

    ASSERT_TRUE(std::holds_alternative<OrderId>(market(b, 3, Side::Buy, 20)));

    EXPECT_EQ(b.getOrder(1), nullptr);
    EXPECT_EQ(b.getOrder(2), nullptr);
}

// An elected stop is a market order and gets the same collar, from the touch it
// meets when it is elected: here 100.01, so a bound of 105.0105.
TEST_P(Collar, ElectedStopIsCollared) {
    OrderBook b(1, GetParam());
    // The collar from the elected stop's own touch reaches 1'050'105, 5.01%
    // from the first print; an elected stop's prints face the breaker now
    // (MR-2), so it is moved out of the way to isolate the collar.
    b.setCircuitBreakerThreshold(0.5);
    rest(b, 1, Side::Sell, kTouch, 1);
    rest(b, 2, Side::Sell, 1'000'100, 1);
    rest(b, 3, Side::Sell, 1'050'105, 1);
    rest(b, 4, Side::Sell, 1'050'106, 1);
    ASSERT_TRUE(std::holds_alternative<OrderId>(
        b.addOrder(5, 2, Side::Buy, 0, 3, OrderType::Stop, /*stopPrice=*/kTouch)));
    ASSERT_TRUE(std::holds_alternative<OrderId>(market(b, 6, Side::Buy, 1)));  // prints at the touch

    EXPECT_EQ(b.getOrder(2), nullptr);
    EXPECT_EQ(b.getOrder(3), nullptr);
    EXPECT_NE(b.getOrder(4), nullptr);
}

TEST_P(Collar, TouchNearTheTopOfThePriceRangeDoesNotOverflow) {
    constexpr Price kHuge = std::numeric_limits<Price>::max() - 100;
    OrderBook b(1, GetParam());
    b.setCircuitBreakerThreshold(1e9);
    rest(b, 1, Side::Sell, kHuge, 10);
    ASSERT_TRUE(std::holds_alternative<OrderId>(market(b, 2, Side::Buy, 10)));
    EXPECT_EQ(b.getOrder(1), nullptr);
}

INSTANTIATE_TEST_SUITE_P(BothAlgorithms, Collar,
                         ::testing::Values(MatchAlgorithm::PriceTime, MatchAlgorithm::ProRata),
                         [](const auto& info) {
                             return info.param == MatchAlgorithm::ProRata ? "ProRata" : "PriceTime";
                         });

TEST(CollarDefault, IsFivePercent) {
    EXPECT_EQ(OrderBook(1).getMarketProtectionPct(), 0.05);
}

}  // namespace
