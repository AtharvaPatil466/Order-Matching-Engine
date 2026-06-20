#include <gtest/gtest.h>
#include "OrderBook.h"
#include "MatchingEngine.h"
#include "EventListener.h"
#include "Types.h"
#include <variant>
#include <vector>

using namespace OrderMatcher;

// ─── Test EventListener ──────────────────────────────────────────────────────

class MocLocListener : public EventListener {
public:
    std::vector<Trade>           trades;
    std::vector<OrderUpdate>     updates;
    std::vector<MarketDataUpdate> mdUpdates;

    void onTrade(const Trade& t)          override { trades.push_back(t); }
    void onOrderUpdate(const OrderUpdate& u) override { updates.push_back(u); }
    void onMarketData(const MarketDataUpdate& md) override { mdUpdates.push_back(md); }

    void reset() { trades.clear(); updates.clear(); mdUpdates.clear(); }

    bool hasUpdate(OrderId id, OrderStatus status) const {
        for (const auto& u : updates) {
            if (u.orderId == id && u.status == status) return true;
        }
        return false;
    }
};

// ─── Fixture ─────────────────────────────────────────────────────────────────

class MocLocTest : public ::testing::Test {
protected:
    OrderBook     book;
    MocLocListener listener;

    void SetUp() override {
        book.setEventListener(&listener);
    }
};

// ─── 1. MOC parked during Continuous, not filled immediately ─────────────────

TEST_F(MocLocTest, MOC_ParkedDuringContinuous_NotFilled) {
    // In Continuous state a MOC order must be parked — it should not
    // execute against the resting limit sell on the other side.
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);

    auto res = book.addOrder(2, 2, Side::Buy, /*price=*/0, 50, OrderType::MOC);
    ASSERT_TRUE(std::holds_alternative<OrderId>(res))
        << "MOC in Continuous must be accepted (parked), not rejected";

    // No trades produced yet.
    EXPECT_EQ(listener.trades.size(), 0u)
        << "MOC must not trigger an immediate fill during Continuous";

    // The MOC order exists in lookup but is NOT in the limit book.
    EXPECT_NE(book.getOrder(2), nullptr) << "MOC must be in orderLookup";

    // The limit sell is unaffected.
    ASSERT_NE(book.getOrder(1), nullptr);
    EXPECT_EQ(book.getOrder(1)->remainingQty, 100u);
}

// ─── 2. MOC released when state transitions to AuctionClose ──────────────────

TEST_F(MocLocTest, MOC_ReleasedOnAuctionCloseTransition) {
    // Park a MOC order, then park a resting limit sell so we have
    // counterparty liquidity for the uncross.
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);

    auto res = book.addOrder(2, 2, Side::Buy, /*price=*/0, 50, OrderType::MOC);
    ASSERT_TRUE(std::holds_alternative<OrderId>(res));

    // No trades before the transition.
    EXPECT_EQ(listener.trades.size(), 0u);

    // Transition to AuctionClose — releaseOnCloseOrders() fires.
    book.setTradingState(TradingState::AuctionClose);
    // Still no trades (MOC is now in auctionMarketOrders_; uncross hasn't run).
    EXPECT_EQ(listener.trades.size(), 0u);

    // Run the uncross.
    book.uncross();

    // MOC buy (qty 50) vs limit sell (qty 100): 50 should fill.
    EXPECT_GT(listener.trades.size(), 0u) << "uncross must produce a trade";
    EXPECT_EQ(book.getOrder(2), nullptr)  << "MOC order must be filled/cancelled";
    const Order* sell = book.getOrder(1);
    ASSERT_NE(sell, nullptr);
    EXPECT_EQ(sell->remainingQty, 50u)    << "50 of 100 limit sell should remain";
}

// ─── 3. LOC participates in close at its limit price ─────────────────────────

TEST_F(MocLocTest, LOC_ParticipatesInCloseAtLimitPrice) {
    // A LOC buy at 1,000,000 should fill against a limit sell at 1,000,000
    // during the closing uncross.
    book.addOrder(1, 1, Side::Sell, 1000000, 80, OrderType::Limit);

    // LOC buy: limit price 1,000,000, qty 60.
    auto res = book.addOrder(2, 2, Side::Buy, 1000000, 60, OrderType::LOC);
    ASSERT_TRUE(std::holds_alternative<OrderId>(res))
        << "LOC must be accepted (parked) during Continuous";

    EXPECT_EQ(listener.trades.size(), 0u) << "no trade before AuctionClose";

    book.setTradingState(TradingState::AuctionClose);
    book.uncross();

    EXPECT_GT(listener.trades.size(), 0u) << "LOC must fill at uncross";
    // LOC fully filled: 60 shares.
    EXPECT_EQ(book.getOrder(2), nullptr)  << "LOC fully filled — removed from book";
    ASSERT_NE(book.getOrder(1), nullptr);
    EXPECT_EQ(book.getOrder(1)->remainingQty, 20u)
        << "60 of 80 limit sell consumed";
}

// ─── 4. LOC outside clearing price is cancelled after uncross ────────────────

TEST_F(MocLocTest, LOC_OutsideClearingPrice_CancelledAfterUncross) {
    // Clearing price will be 1,000,000 (established by the two limit orders).
    // The LOC buy at 990,000 is below that clearing price — it will rest in
    // the book at its limit but won't participate in the uncross execution
    // (no counterparty at or below 990,000 on the ask side).
    // After uncross, cancelLocOrders() must cancel it.
    book.addOrder(1, 1, Side::Sell, 1000000, 50, OrderType::Limit);
    book.addOrder(2, 2, Side::Buy,  1000000, 50, OrderType::Limit);

    // LOC at a price that cannot participate in the uncross.
    auto res = book.addOrder(3, 3, Side::Buy, 990000, 30, OrderType::LOC);
    ASSERT_TRUE(std::holds_alternative<OrderId>(res));

    book.setTradingState(TradingState::AuctionClose);
    book.uncross();

    // The crossing limit orders should have traded.
    EXPECT_GT(listener.trades.size(), 0u);
    // The LOC at 990,000 must be cancelled — it did not participate.
    EXPECT_EQ(book.getOrder(3), nullptr)
        << "LOC with limit below clearing price must be cancelled after uncross";
    EXPECT_TRUE(listener.hasUpdate(3, OrderStatus::Cancelled))
        << "cancelled LOC must emit a Cancelled update";
}

// ─── 5. MOC fills at uncross price ───────────────────────────────────────────

TEST_F(MocLocTest, MOC_FillsAtUncrossPrice) {
    // Uncross price will be determined by the limit orders (1,000,000).
    // The MOC sell should execute at that price.
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);

    // MOC sell (no limit price).
    auto res = book.addOrder(2, 2, Side::Sell, /*price=*/0, 70, OrderType::MOC);
    ASSERT_TRUE(std::holds_alternative<OrderId>(res));

    book.setTradingState(TradingState::AuctionClose);
    book.uncross();

    ASSERT_GT(listener.trades.size(), 0u);
    // All trades must be at the uncross price (1,000,000).
    for (const auto& t : listener.trades) {
        EXPECT_EQ(t.price, 1000000)
            << "MOC must fill at the uncross price";
    }
    // MOC sell fully filled (70 ≤ 100 available).
    EXPECT_EQ(book.getOrder(2), nullptr);
    ASSERT_NE(book.getOrder(1), nullptr);
    EXPECT_EQ(book.getOrder(1)->remainingQty, 30u);
}

// ─── 6. Cancel of a parked MOC/LOC order (before AuctionClose) ───────────────

TEST_F(MocLocTest, CancelParkedMOC_BeforeAuctionClose) {
    auto res = book.addOrder(1, 1, Side::Buy, /*price=*/0, 50, OrderType::MOC);
    ASSERT_TRUE(std::holds_alternative<OrderId>(res));
    EXPECT_NE(book.getOrder(1), nullptr);

    book.cancelOrder(1);

    // Must be gone from lookup.
    EXPECT_EQ(book.getOrder(1), nullptr);
    EXPECT_TRUE(listener.hasUpdate(1, OrderStatus::Cancelled))
        << "cancelling a parked MOC must emit Cancelled";
}

TEST_F(MocLocTest, CancelParkedLOC_BeforeAuctionClose) {
    auto res = book.addOrder(1, 1, Side::Buy, 1000000, 40, OrderType::LOC);
    ASSERT_TRUE(std::holds_alternative<OrderId>(res));
    EXPECT_NE(book.getOrder(1), nullptr);

    book.cancelOrder(1);

    EXPECT_EQ(book.getOrder(1), nullptr);
    EXPECT_TRUE(listener.hasUpdate(1, OrderStatus::Cancelled))
        << "cancelling a parked LOC must emit Cancelled";

    // After cancellation, transitioning to AuctionClose and uncrossing
    // must not crash or attempt to cancel the already-removed order.
    EXPECT_NO_FATAL_FAILURE({
        book.setTradingState(TradingState::AuctionClose);
        book.uncross();
    });
}

// ─── 7. MOC/LOC submitted directly during AuctionClose ───────────────────────

TEST_F(MocLocTest, MOC_SubmittedDuringAuctionClose_ParticipatesImmediately) {
    // Place a limit sell first so there is counterparty liquidity.
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);

    // Transition to AuctionClose before submitting the MOC.
    book.setTradingState(TradingState::AuctionClose);

    auto res = book.addOrder(2, 2, Side::Buy, /*price=*/0, 50, OrderType::MOC);
    ASSERT_TRUE(std::holds_alternative<OrderId>(res))
        << "MOC submitted during AuctionClose must be accepted immediately";

    // Still no trades until uncross.
    EXPECT_EQ(listener.trades.size(), 0u);

    book.uncross();

    EXPECT_GT(listener.trades.size(), 0u);
    EXPECT_EQ(book.getOrder(2), nullptr) << "MOC fully filled during AuctionClose uncross";
}

TEST_F(MocLocTest, LOC_SubmittedDuringAuctionClose_ParticipatesImmediately) {
    // Place a limit sell first.
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);

    book.setTradingState(TradingState::AuctionClose);

    // LOC buy at the sell's price — should land in the limit book and fill.
    auto res = book.addOrder(2, 2, Side::Buy, 1000000, 60, OrderType::LOC);
    ASSERT_TRUE(std::holds_alternative<OrderId>(res))
        << "LOC submitted during AuctionClose must be accepted immediately";

    EXPECT_EQ(listener.trades.size(), 0u) << "no trade before uncross";

    book.uncross();

    EXPECT_GT(listener.trades.size(), 0u);
    EXPECT_EQ(book.getOrder(2), nullptr) << "LOC fully filled at uncross";
}

// ─── 8. Multiple MOC orders on both sides ────────────────────────────────────

TEST_F(MocLocTest, MultipleMOC_BothSides_FillAtUncrossPrice) {
    // Anchor limit orders on both sides to establish a clearing price.
    book.addOrder(1, 1, Side::Sell, 1000000, 200, OrderType::Limit);
    book.addOrder(2, 2, Side::Buy,  1000000, 200, OrderType::Limit);

    // Two MOC buys parked before AuctionClose.
    auto m1 = book.addOrder(3, 3, Side::Buy,  /*price=*/0, 50, OrderType::MOC);
    auto m2 = book.addOrder(4, 4, Side::Sell, /*price=*/0, 30, OrderType::MOC);
    ASSERT_TRUE(std::holds_alternative<OrderId>(m1));
    ASSERT_TRUE(std::holds_alternative<OrderId>(m2));

    book.setTradingState(TradingState::AuctionClose);
    book.uncross();

    EXPECT_GT(listener.trades.size(), 0u);
    // MOC orders must not remain in the book after uncross.
    EXPECT_EQ(book.getOrder(3), nullptr);
    EXPECT_EQ(book.getOrder(4), nullptr);
}

// ─── 9. LOC fully filled at the clearing price — not cancelled ────────────────

TEST_F(MocLocTest, LOC_FullyFilled_NotCancelledByPost_Uncross) {
    // LOC buy at 1,000,000 should be fully matched by the limit sell.
    book.addOrder(1, 1, Side::Sell, 1000000, 50, OrderType::Limit);

    auto res = book.addOrder(2, 2, Side::Buy, 1000000, 50, OrderType::LOC);
    ASSERT_TRUE(std::holds_alternative<OrderId>(res));

    book.setTradingState(TradingState::AuctionClose);
    book.uncross();

    // Both orders fully filled — neither should remain.
    EXPECT_EQ(book.getOrder(1), nullptr) << "limit sell fully consumed";
    EXPECT_EQ(book.getOrder(2), nullptr) << "LOC fully filled";
    EXPECT_TRUE(listener.hasUpdate(2, OrderStatus::Filled))
        << "fully filled LOC must emit Filled, not Cancelled";
}

// ─── 10. MOC/LOC admitted during PreOpen, released only on AuctionClose ──────

TEST_F(MocLocTest, MOC_AdmittedInPreOpen_ReleasedOnlyAtAuctionClose) {
    book.setTradingState(TradingState::PreOpen);

    auto res = book.addOrder(1, 1, Side::Buy, /*price=*/0, 50, OrderType::MOC);
    ASSERT_TRUE(std::holds_alternative<OrderId>(res))
        << "MOC must be accepted during PreOpen";

    // Transition to AuctionOpen — must NOT release MOC yet.
    book.setTradingState(TradingState::AuctionOpen);
    EXPECT_EQ(listener.trades.size(), 0u)
        << "MOC must not be released during AuctionOpen transition";

    // Add limit orders for the closing uncross.
    book.addOrder(2, 2, Side::Sell, 1000000, 100, OrderType::Limit);

    // Now transition to AuctionClose — MOC should be released and fill.
    book.setTradingState(TradingState::AuctionClose);
    book.uncross();

    EXPECT_GT(listener.trades.size(), 0u)
        << "MOC must participate in the closing uncross";
    EXPECT_EQ(book.getOrder(1), nullptr);
}

// ─── 11. MatchingEngine integration: MOC via submitOrder ─────────────────────

TEST(MocLocEngine, MOC_ViaMatchingEngine_FillsAtAuctionClose) {
    MatchingEngine engine;
    engine.addSymbol(1);
    engine.start();

    // Limit sell for liquidity.
    engine.submitOrder(1, 101, 1, Side::Sell, 1000000, 100, OrderType::Limit);

    // MOC buy.
    auto r = engine.submitOrder(1, 102, 2, Side::Buy, /*price=*/0, 60, OrderType::MOC);
    EXPECT_TRUE(r.isAccepted()) << "MOC must be accepted";

    auto* book = engine.getOrderBook(1);
    EXPECT_NE(book->getOrder(102), nullptr) << "MOC must be in orderLookup";
    EXPECT_EQ(book->getTradeCount(), 0u)    << "no trades yet";

    // Transition to AuctionClose and uncross.
    book->setTradingState(TradingState::AuctionClose);
    engine.uncross(1);

    EXPECT_GT(book->getTradeCount(), 0u);
    EXPECT_EQ(book->getOrder(102), nullptr) << "MOC filled";
    ASSERT_NE(book->getOrder(101), nullptr);
    EXPECT_EQ(book->getOrder(101)->remainingQty, 40u);
}

TEST(MocLocEngine, LOC_ViaMatchingEngine_CancelledIfOutsideClearingPrice) {
    MatchingEngine engine;
    engine.addSymbol(1);
    engine.start();

    // Establish crossing limit orders at 1,000,000.
    engine.submitOrder(1, 101, 1, Side::Sell, 1000000, 50, OrderType::Limit);
    engine.submitOrder(1, 102, 2, Side::Buy,  1000000, 50, OrderType::Limit);

    // LOC buy at a price below the clearing price.
    auto r = engine.submitOrder(1, 103, 3, Side::Buy, 990000, 30, OrderType::LOC);
    EXPECT_TRUE(r.isAccepted());

    auto* book = engine.getOrderBook(1);
    book->setTradingState(TradingState::AuctionClose);
    engine.uncross(1);

    // The crossing limit orders should have traded.
    EXPECT_GT(book->getTradeCount(), 0u);
    // The LOC at 990,000 must be gone (cancelled post-uncross).
    EXPECT_EQ(book->getOrder(103), nullptr)
        << "LOC below clearing price must be cancelled after uncross";
}
