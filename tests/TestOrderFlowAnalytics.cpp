// TestOrderFlowAnalytics — Google Test suite for OrderFlowAnalytics.
//
// Test coverage:
//  1.  Roll spread with perfectly negatively serially correlated prices → known value
//  2.  Roll spread with positive serial correlation → returns 0
//  3.  Roll spread with fewer than 3 prices → returns 0
//  4.  queueImbalance(100, 50) → 1/3
//  5.  weightedQueueImbalance with equal levels → 0
//  6.  weightedQueueImbalance skewed bid-heavy → positive
//  7.  arrivalRatePerUs: 5 arrivals 1000μs apart → ≈ 0.001
//  8.  fillProbability at distance 0 for mixed observations → (0,1)
//  9.  fillProbability at distance 0 with no observations → 0.5
//  10. orderToTradeRatio = 3 orders, 1 trade → 3.0
//  11. reset() clears all state

#include <gtest/gtest.h>

#include "OrderFlowAnalytics.h"

#include <cmath>

using namespace OrderMatcher;

// ─────────────────────────────────────────────────────────────────────────────
// 1. Roll spread — perfectly negatively serially correlated prices
//
// Construct a series with known Δp auto-covariance.
// Prices: 100, 101, 100, 101, 100  → diffs: +1, -1, +1, -1
// Product pairs: (+1*-1), (-1*+1), (+1*-1) → each = -1
// cov = mean(-1, -1, -1) = -1
// Roll spread = 2 * sqrt(1) = 2.0
// ─────────────────────────────────────────────────────────────────────────────
TEST(OrderFlowAnalytics, RollSpreadNegativeCorrelation) {
    OrderFlowAnalytics oa;
    oa.recordTradePrice(100.0);
    oa.recordTradePrice(101.0);
    oa.recordTradePrice(100.0);
    oa.recordTradePrice(101.0);
    oa.recordTradePrice(100.0);

    // diffs: [+1, -1, +1, -1]
    // consecutive products: -1, -1, -1  → cov = -1 → spread = 2*sqrt(1) = 2
    EXPECT_NEAR(oa.rollSpread(), 2.0, 1e-9);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Roll spread with positive serial correlation → returns 0
// ─────────────────────────────────────────────────────────────────────────────
TEST(OrderFlowAnalytics, RollSpreadPositiveCorrelation) {
    OrderFlowAnalytics oa;
    // Trending prices → positive autocovariance
    oa.recordTradePrice(100.0);
    oa.recordTradePrice(101.0);
    oa.recordTradePrice(102.0);
    oa.recordTradePrice(103.0);
    oa.recordTradePrice(104.0);

    // diffs: [1, 1, 1, 1] → products: [1, 1, 1] → cov = 1 > 0
    EXPECT_DOUBLE_EQ(oa.rollSpread(), 0.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Roll spread with fewer than 3 prices → returns 0
// ─────────────────────────────────────────────────────────────────────────────
TEST(OrderFlowAnalytics, RollSpreadTooFewPrices) {
    OrderFlowAnalytics oa;

    // 0 prices
    EXPECT_DOUBLE_EQ(oa.rollSpread(), 0.0);

    // 1 price
    oa.recordTradePrice(100.0);
    EXPECT_DOUBLE_EQ(oa.rollSpread(), 0.0);

    // 2 prices
    oa.recordTradePrice(101.0);
    EXPECT_DOUBLE_EQ(oa.rollSpread(), 0.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. queueImbalance(100, 50) = (100-50)/(100+50) = 50/150 = 1/3
// ─────────────────────────────────────────────────────────────────────────────
TEST(OrderFlowAnalytics, QueueImbalanceKnownValue) {
    double expected = (100.0 - 50.0) / (100.0 + 50.0);  // 1/3
    EXPECT_NEAR(OrderFlowAnalytics::queueImbalance(100.0, 50.0), expected, 1e-9);
}

TEST(OrderFlowAnalytics, QueueImbalanceZeroTotal) {
    EXPECT_DOUBLE_EQ(OrderFlowAnalytics::queueImbalance(0.0, 0.0), 0.0);
}

TEST(OrderFlowAnalytics, QueueImbalanceSymmetric) {
    EXPECT_DOUBLE_EQ(OrderFlowAnalytics::queueImbalance(75.0, 75.0), 0.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. weightedQueueImbalance with equal bid/ask at each level → 0
// ─────────────────────────────────────────────────────────────────────────────
TEST(OrderFlowAnalytics, WeightedQueueImbalanceEqual) {
    std::vector<double> bid = {100.0, 80.0, 60.0};
    std::vector<double> ask = {100.0, 80.0, 60.0};
    EXPECT_NEAR(OrderFlowAnalytics::weightedQueueImbalance(bid, ask), 0.0, 1e-9);
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. weightedQueueImbalance skewed bid-heavy → positive
// ─────────────────────────────────────────────────────────────────────────────
TEST(OrderFlowAnalytics, WeightedQueueImbalanceBidHeavy) {
    // Bid side has more volume at every level
    std::vector<double> bid = {200.0, 100.0, 50.0};
    std::vector<double> ask = {50.0,  25.0,  10.0};
    double imb = OrderFlowAnalytics::weightedQueueImbalance(bid, ask);
    EXPECT_GT(imb, 0.0);
    EXPECT_LE(imb, 1.0);
}

TEST(OrderFlowAnalytics, WeightedQueueImbalanceEmptyLevels) {
    std::vector<double> empty;
    EXPECT_DOUBLE_EQ(
        OrderFlowAnalytics::weightedQueueImbalance(empty, empty), 0.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. arrivalRatePerUs: 5 arrivals 1000 μs apart → rate ≈ 0.001 events/μs
// ─────────────────────────────────────────────────────────────────────────────
TEST(OrderFlowAnalytics, ArrivalRateKnownValue) {
    OrderFlowAnalytics oa;
    // 5 arrivals spaced exactly 1000 μs apart → 4 intervals of 1000 μs
    // mean inter-arrival = 1000 μs → λ = 1/1000 = 0.001
    for (uint64_t i = 0; i < 5; ++i)
        oa.recordArrival(i * 1000);

    EXPECT_NEAR(oa.arrivalRatePerUs(), 0.001, 1e-9);
}

TEST(OrderFlowAnalytics, ArrivalRateSingleRecord) {
    OrderFlowAnalytics oa;
    oa.recordArrival(5000);
    EXPECT_DOUBLE_EQ(oa.arrivalRatePerUs(), 0.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// 8. fillProbability at distance 0 for mixed observations → between 0 and 1
// ─────────────────────────────────────────────────────────────────────────────
TEST(OrderFlowAnalytics, FillProbabilityMixedObservations) {
    OrderFlowAnalytics oa;
    // 5 fills and 5 non-fills at various distances
    oa.recordFillObservation(0.0, true);
    oa.recordFillObservation(1.0, true);
    oa.recordFillObservation(2.0, false);
    oa.recordFillObservation(3.0, false);
    oa.recordFillObservation(0.5, true);
    oa.recordFillObservation(4.0, false);
    oa.recordFillObservation(1.5, true);
    oa.recordFillObservation(5.0, false);

    double p = oa.fillProbability(0.0);
    EXPECT_GT(p, 0.0);
    EXPECT_LT(p, 1.0);
    // Sanity: probability should be in [0,1]
    EXPECT_NEAR(p, p, 0.01);  // trivial — just confirms no NaN
}

// Higher distance → lower fill probability (deeper in book)
TEST(OrderFlowAnalytics, FillProbabilityDecreaseWithDistance) {
    OrderFlowAnalytics oa;
    oa.recordFillObservation(0.0, true);
    oa.recordFillObservation(1.0, true);
    oa.recordFillObservation(2.0, false);
    oa.recordFillObservation(3.0, false);
    oa.recordFillObservation(4.0, false);
    oa.recordFillObservation(5.0, false);

    double pClose = oa.fillProbability(0.0);
    double pFar   = oa.fillProbability(10.0);
    EXPECT_GT(pClose, pFar);
}

// ─────────────────────────────────────────────────────────────────────────────
// 9. fillProbability with no observations → 0.5
// ─────────────────────────────────────────────────────────────────────────────
TEST(OrderFlowAnalytics, FillProbabilityNoObservations) {
    OrderFlowAnalytics oa;
    EXPECT_NEAR(oa.fillProbability(0.0), 0.5, 1e-9);
}

TEST(OrderFlowAnalytics, FillProbabilityFewerThan4Observations) {
    OrderFlowAnalytics oa;
    oa.recordFillObservation(0.0, true);
    oa.recordFillObservation(1.0, false);
    oa.recordFillObservation(2.0, true);
    // Only 3 observations → default 0.5
    EXPECT_NEAR(oa.fillProbability(0.0), 0.5, 1e-9);
}

TEST(OrderFlowAnalytics, FillProbabilityAllFilled) {
    OrderFlowAnalytics oa;
    // All observations are fills → cannot distinguish → returns 0.5
    oa.recordFillObservation(0.0, true);
    oa.recordFillObservation(1.0, true);
    oa.recordFillObservation(2.0, true);
    oa.recordFillObservation(3.0, true);
    EXPECT_NEAR(oa.fillProbability(0.0), 0.5, 1e-9);
}

// ─────────────────────────────────────────────────────────────────────────────
// 10. orderToTradeRatio: 3 orders, 1 trade → 3.0
// ─────────────────────────────────────────────────────────────────────────────
TEST(OrderFlowAnalytics, OrderToTradeRatioKnownValue) {
    OrderFlowAnalytics oa;
    oa.recordOrderEvent();
    oa.recordOrderEvent();
    oa.recordOrderEvent();
    oa.recordTradeEvent();
    EXPECT_NEAR(oa.orderToTradeRatio(), 3.0, 1e-9);
}

TEST(OrderFlowAnalytics, OrderToTradeRatioNoTrades) {
    OrderFlowAnalytics oa;
    oa.recordOrderEvent();
    oa.recordOrderEvent();
    EXPECT_DOUBLE_EQ(oa.orderToTradeRatio(), 0.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// 11. reset() clears all state
// ─────────────────────────────────────────────────────────────────────────────
TEST(OrderFlowAnalytics, ResetClearsAllState) {
    OrderFlowAnalytics oa;

    // Populate every sub-system
    oa.recordTradePrice(100.0);
    oa.recordTradePrice(101.0);
    oa.recordTradePrice(100.0);
    oa.recordArrival(0);
    oa.recordArrival(1000);
    oa.recordFillObservation(0.0, true);
    oa.recordFillObservation(1.0, false);
    oa.recordFillObservation(2.0, true);
    oa.recordFillObservation(3.0, false);
    oa.recordOrderEvent();
    oa.recordOrderEvent();
    oa.recordTradeEvent();

    // Sanity check: state is non-zero before reset
    EXPECT_GT(oa.rollSpread(), 0.0);
    EXPECT_NEAR(oa.arrivalRatePerUs(), 0.001, 1e-9);
    EXPECT_NEAR(oa.orderToTradeRatio(), 2.0, 1e-9);

    oa.reset();

    // Everything should return zero / default after reset
    EXPECT_DOUBLE_EQ(oa.rollSpread(), 0.0);
    EXPECT_DOUBLE_EQ(oa.arrivalRatePerUs(), 0.0);
    EXPECT_NEAR(oa.fillProbability(0.0), 0.5, 1e-9);
    EXPECT_DOUBLE_EQ(oa.orderToTradeRatio(), 0.0);
}
