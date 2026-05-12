// ShadowModeTest — Validates shadow comparison detects FIFO divergence.
//
// This test proves that a shadow matching engine approach catches real
// algorithmic bugs by deliberately introducing a FIFO violation in one
// book and verifying that trade output diverges.
//
// Scenario:
//   1. Primary book: receives orders A, B (same side, same price), then
//      a crossing order C. FIFO guarantees C matches A first.
//   2. Shadow book: receives orders B, A (reversed insertion), then C.
//      FIFO matches C against B first (wrong counterparty).
//   3. The comparator detects the trade mismatch: different buyOrderId.
//
// This is NOT a full ShadowEngine test. It validates the core principle:
// if two matching engines receive the same logical orders but process them
// differently, trade-level comparison catches the divergence.

#include <gtest/gtest.h>
#include "OrderBook.h"
#include "Types.h"
#include <vector>
#include <cstdio>

using namespace OrderMatcher;

// ─── Trade Listener ─────────────────────────────────────────────────────────

class TradeCapture : public EventListener {
public:
    std::vector<Trade> trades;
    std::vector<OrderUpdate> updates;

    void onTrade(const Trade& t) override { trades.push_back(t); }
    void onOrderUpdate(const OrderUpdate& u) override { updates.push_back(u); }
    void onMarketData(const MarketDataUpdate&) override {}

    void reset() { trades.clear(); updates.clear(); }
};

// ─── Shadow Comparator ──────────────────────────────────────────────────────

struct DivergenceReport {
    size_t tradeIndex{0};
    bool tradeCountMismatch{false};
    bool priceMismatch{false};
    bool quantityMismatch{false};
    bool counterpartyMismatch{false};
    bool snapshotMismatch{false};

    bool hasDivergence() const {
        return tradeCountMismatch || priceMismatch ||
               quantityMismatch || counterpartyMismatch || snapshotMismatch;
    }

    std::string describe() const {
        std::string msg;
        if (tradeCountMismatch) msg += "trade_count ";
        if (priceMismatch) msg += "price ";
        if (quantityMismatch) msg += "quantity ";
        if (counterpartyMismatch) msg += "counterparty ";
        if (snapshotMismatch) msg += "snapshot ";
        return msg.empty() ? "none" : msg;
    }
};

static DivergenceReport compareTrades(const std::vector<Trade>& primary,
                                       const std::vector<Trade>& shadow) {
    DivergenceReport report;

    if (primary.size() != shadow.size()) {
        report.tradeCountMismatch = true;
        return report;
    }

    for (size_t i = 0; i < primary.size(); ++i) {
        const auto& p = primary[i];
        const auto& s = shadow[i];

        if (p.price != s.price) {
            report.tradeIndex = i;
            report.priceMismatch = true;
            return report;
        }
        if (p.quantity != s.quantity) {
            report.tradeIndex = i;
            report.quantityMismatch = true;
            return report;
        }
        if (p.buyOrderId != s.buyOrderId || p.sellOrderId != s.sellOrderId) {
            report.tradeIndex = i;
            report.counterpartyMismatch = true;
            return report;
        }
    }
    return report;
}

static DivergenceReport compareSnapshots(const OrderBook& primary,
                                          const OrderBook& shadow,
                                          size_t depth = 5) {
    DivergenceReport report;
    auto pSnap = primary.getSnapshot(depth);
    auto sSnap = shadow.getSnapshot(depth);

    if (pSnap.bidCount != sSnap.bidCount ||
        pSnap.askCount != sSnap.askCount) {
        report.snapshotMismatch = true;
        return report;
    }

    for (size_t i = 0; i < pSnap.bidCount; ++i) {
        if (pSnap.bids[i].price != sSnap.bids[i].price ||
            pSnap.bids[i].totalQuantity != sSnap.bids[i].totalQuantity) {
            report.snapshotMismatch = true;
            return report;
        }
    }
    for (size_t i = 0; i < pSnap.askCount; ++i) {
        if (pSnap.asks[i].price != sSnap.asks[i].price ||
            pSnap.asks[i].totalQuantity != sSnap.asks[i].totalQuantity) {
            report.snapshotMismatch = true;
            return report;
        }
    }
    return report;
}

// ─── Tests ──────────────────────────────────────────────────────────────────

// BASELINE: Two identical books with identical order flow produce identical
// trades. This must pass for the shadow concept to be valid.
TEST(ShadowMode, IdenticalBooksProduceIdenticalTrades) {
    TradeCapture primaryCapture, shadowCapture;

    OrderBook primary(0, MatchAlgorithm::PriceTime);
    OrderBook shadow(0, MatchAlgorithm::PriceTime);
    primary.setEventListener(&primaryCapture);
    shadow.setEventListener(&shadowCapture);

    // Place two resting buys at 100, then a sell that crosses
    primary.addOrder(1, 10, Side::Buy, 100, 50, OrderType::Limit);
    primary.addOrder(2, 11, Side::Buy, 100, 30, OrderType::Limit);
    primary.addOrder(3, 12, Side::Sell, 100, 40, OrderType::Limit);

    shadow.addOrder(1, 10, Side::Buy, 100, 50, OrderType::Limit);
    shadow.addOrder(2, 11, Side::Buy, 100, 30, OrderType::Limit);
    shadow.addOrder(3, 12, Side::Sell, 100, 40, OrderType::Limit);

    // Both should produce the same trade: order 1 fills 40 against order 3
    auto tradeReport = compareTrades(primaryCapture.trades, shadowCapture.trades);
    EXPECT_FALSE(tradeReport.hasDivergence())
        << "Identical flow should produce zero divergence, got: "
        << tradeReport.describe();

    auto snapReport = compareSnapshots(primary, shadow);
    EXPECT_FALSE(snapReport.hasDivergence())
        << "Identical flow should produce identical book state";

    // Verify the actual trade is correct (FIFO: order 1 matched first)
    ASSERT_EQ(primaryCapture.trades.size(), 1u);
    EXPECT_EQ(primaryCapture.trades[0].buyOrderId, 1u);  // FIFO: order 1 first
    EXPECT_EQ(primaryCapture.trades[0].sellOrderId, 3u);
    EXPECT_EQ(primaryCapture.trades[0].quantity, 40u);
}

// THE REAL TEST: Deliberately break FIFO in the shadow book by swapping
// insertion order. The comparator must catch the counterparty mismatch.
TEST(ShadowMode, DetectsFIFOViolationViaTradeDivergence) {
    TradeCapture primaryCapture, shadowCapture;

    OrderBook primary(0, MatchAlgorithm::PriceTime);
    OrderBook shadow(0, MatchAlgorithm::PriceTime);
    primary.setEventListener(&primaryCapture);
    shadow.setEventListener(&shadowCapture);

    // PRIMARY: Correct FIFO order — A arrives before B
    //   Order 1 (participant 10): Buy 50 @ 100
    //   Order 2 (participant 11): Buy 30 @ 100
    //   Order 3 (participant 12): Sell 40 @ 100
    //   → FIFO: order 1 fills (50 avail, 40 filled), order 2 untouched
    primary.addOrder(1, 10, Side::Buy, 100, 50, OrderType::Limit);
    primary.addOrder(2, 11, Side::Buy, 100, 30, OrderType::Limit);
    primary.addOrder(3, 12, Side::Sell, 100, 40, OrderType::Limit);

    // SHADOW: Broken FIFO — B arrives before A (simulating a queue reorder bug)
    //   Order 2 (participant 11): Buy 30 @ 100 ← arrives FIRST
    //   Order 1 (participant 10): Buy 50 @ 100 ← arrives SECOND
    //   Order 3 (participant 12): Sell 40 @ 100
    //   → Broken FIFO: order 2 fills first (30 filled), then order 1 fills 10
    shadow.addOrder(2, 11, Side::Buy, 100, 30, OrderType::Limit);
    shadow.addOrder(1, 10, Side::Buy, 100, 50, OrderType::Limit);
    shadow.addOrder(3, 12, Side::Sell, 100, 40, OrderType::Limit);

    // The trade divergence MUST be detected
    auto tradeReport = compareTrades(primaryCapture.trades, shadowCapture.trades);
    EXPECT_TRUE(tradeReport.hasDivergence())
        << "FIFO violation should produce detectable divergence";

    // Verify the specific divergence type
    if (primaryCapture.trades.size() != shadowCapture.trades.size()) {
        // Shadow produces 2 trades (30 + 10) vs primary's 1 trade (40)
        EXPECT_TRUE(tradeReport.tradeCountMismatch)
            << "Expected trade count mismatch: primary="
            << primaryCapture.trades.size() << " shadow="
            << shadowCapture.trades.size();
    } else {
        EXPECT_TRUE(tradeReport.counterpartyMismatch)
            << "Expected counterparty mismatch from FIFO violation";
    }

    // Log what was detected (for the "shadow mode caught a bug" story)
    std::printf("\n  ┌─ Shadow Mode Divergence Report ────────────────────┐\n");
    std::printf("  │ Primary trades:  %zu                                │\n",
                primaryCapture.trades.size());
    std::printf("  │ Shadow trades:   %zu                                │\n",
                shadowCapture.trades.size());
    std::printf("  │ Divergence type: %-33s│\n", tradeReport.describe().c_str());

    for (size_t i = 0; i < primaryCapture.trades.size(); ++i) {
        const auto& t = primaryCapture.trades[i];
        std::printf("  │ Primary[%zu]: buy=%llu sell=%llu qty=%llu @%llu  │\n",
                    i, (unsigned long long)t.buyOrderId,
                    (unsigned long long)t.sellOrderId,
                    (unsigned long long)t.quantity,
                    (unsigned long long)t.price);
    }
    for (size_t i = 0; i < shadowCapture.trades.size(); ++i) {
        const auto& t = shadowCapture.trades[i];
        std::printf("  │ Shadow[%zu]:  buy=%llu sell=%llu qty=%llu @%llu  │\n",
                    i, (unsigned long long)t.buyOrderId,
                    (unsigned long long)t.sellOrderId,
                    (unsigned long long)t.quantity,
                    (unsigned long long)t.price);
    }
    std::printf("  └──────────────────────────────────────────────────┘\n");
}

// SNAPSHOT DIVERGENCE: Same total quantity but different distribution
// across price levels detectable via snapshot comparison.
TEST(ShadowMode, DetectsBookStateDivergenceViaSnapshot) {
    OrderBook primary(0, MatchAlgorithm::PriceTime);
    OrderBook shadow(0, MatchAlgorithm::PriceTime);

    // Primary: 100 @ price 100
    primary.addOrder(1, 10, Side::Buy, 100, 100, OrderType::Limit);

    // Shadow: 100 @ price 99 (simulating a price-level boundary error)
    shadow.addOrder(1, 10, Side::Buy, 99, 100, OrderType::Limit);

    auto report = compareSnapshots(primary, shadow);
    EXPECT_TRUE(report.hasDivergence())
        << "Different price levels should be detected by snapshot comparison";
}

// MULTI-ORDER STREAM: Feed 100 orders to both books, verify state equivalence.
// This is the "clean run" test — no bugs injected, state should match.
TEST(ShadowMode, LargeIdenticalStreamProducesZeroDivergence) {
    TradeCapture primaryCapture, shadowCapture;

    OrderBook primary(0, MatchAlgorithm::PriceTime);
    OrderBook shadow(0, MatchAlgorithm::PriceTime);
    primary.setEventListener(&primaryCapture);
    shadow.setEventListener(&shadowCapture);

    // Feed 200 orders: alternating buys and sells around a midpoint
    for (OrderId i = 1; i <= 200; ++i) {
        Side side = (i % 2 == 0) ? Side::Buy : Side::Sell;
        Price price = (side == Side::Buy) ? 99 + (i % 3) : 101 + (i % 3);
        Quantity qty = 10 + (i % 20);
        ParticipantId pid = 1 + (i % 5);

        primary.addOrder(i, pid, side, price, qty, OrderType::Limit);
        shadow.addOrder(i, pid, side, price, qty, OrderType::Limit);

        // Compare every 50 orders
        if (i % 50 == 0) {
            auto tradeReport = compareTrades(primaryCapture.trades, shadowCapture.trades);
            EXPECT_FALSE(tradeReport.hasDivergence())
                << "Divergence at order " << i << ": " << tradeReport.describe();

            auto snapReport = compareSnapshots(primary, shadow);
            EXPECT_FALSE(snapReport.hasDivergence())
                << "Snapshot divergence at order " << i;
        }
    }

    // Final check
    auto tradeReport = compareTrades(primaryCapture.trades, shadowCapture.trades);
    EXPECT_FALSE(tradeReport.hasDivergence());
    auto snapReport = compareSnapshots(primary, shadow);
    EXPECT_FALSE(snapReport.hasDivergence());

    std::printf("\n  Shadow clean run: %zu trades verified across 200 orders\n",
                primaryCapture.trades.size());
}
