// RestingOrderRulesTest — what happens to a resting order that an aggressor
// meets: its minQty (Rule 9.5), and self-trade prevention's DecreaseAndCancel
// and CancelBoth in continuous matching.
//
// WHAT WAS WRONG.
//   - MR-5: a minQty-50 buy that could not trade on arrival rested, and
//     matching never reads minQty, so its first execution was 1 lot as a
//     maker; a replace to a crossing price filled 5.
//   - MR-7: DecreaseAndCancel shrank the resting order from 100 to 70 and told
//     its owner nothing.
//   - CancelBoth was executed by no test in continuous matching.

#include "EventListener.h"
#include "OrderBook.h"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <variant>
#include <vector>

using namespace OrderMatcher;

namespace {

constexpr Price kPx = 1000000;

struct Recorder : EventListener {
    std::vector<Trade> trades;
    std::vector<OrderUpdate> ups;
    std::map<OrderId, OrderUpdate> last;
    void onTrade(const Trade& t) override { trades.push_back(t); }
    void onOrderUpdate(const OrderUpdate& u) override { ups.push_back(u); last[u.orderId] = u; }
};

struct Book {
    Recorder rec;
    Recorder engine;
    std::unique_ptr<OrderBook> book;
    explicit Book(MatchAlgorithm algo = MatchAlgorithm::PriceTime)
        : book(std::make_unique<OrderBook>(0, algo)) {
        book->setEventListener(&rec);
        book->setEngineListener(&engine);
    }
    OrderBook* operator->() { return book.get(); }
};

AddOrderResult limitWithMin(OrderBook& b, OrderId id, ParticipantId p, Side side, Price px,
                            Quantity qty, Quantity minQty) {
    return b.addOrder(id, p, side, px, qty, OrderType::Limit, 0, 0, TimeInForce::GTC, 0, 0,
                      PegType::None, 0, 0, minQty);
}

const char* algoName(MatchAlgorithm a) { return a == MatchAlgorithm::ProRata ? "ProRata" : "PriceTime"; }

// ─── Rule 9.5 ───────────────────────────────────────────────────────────────

TEST(MinQty, AnOrderThatCannotMeetItsMinimumOnArrivalDoesNotRest) {
    Book b;
    limitWithMin(*b.book, 1, 10, Side::Buy, kPx, 100, 50);
    EXPECT_EQ(b->getOrder(1), nullptr) << "a minQty order rested with its minimum unmet";
    EXPECT_EQ(b.rec.last[1].status, OrderStatus::Cancelled);

    ASSERT_TRUE(std::holds_alternative<OrderId>(
        b->addOrder(2, 11, Side::Sell, kPx, 1, OrderType::Limit)));
    EXPECT_TRUE(b.rec.trades.empty()) << "the minQty-50 order executed 1 lot";
}

TEST(MinQty, AnOrderThatMeetsItsMinimumTradesAndRestsTheRest) {
    Book b;
    ASSERT_TRUE(std::holds_alternative<OrderId>(
        b->addOrder(1, 11, Side::Sell, kPx, 60, OrderType::Limit)));
    limitWithMin(*b.book, 2, 10, Side::Buy, kPx, 100, 50);
    ASSERT_EQ(b.rec.trades.size(), 1u);
    EXPECT_EQ(b.rec.trades[0].quantity, 60u);
    ASSERT_NE(b->getOrder(2), nullptr);
    EXPECT_EQ(b->getOrder(2)->remainingQty, 40u);

    // Its first execution is done: the rest trades any size.
    ASSERT_TRUE(std::holds_alternative<OrderId>(
        b->addOrder(3, 12, Side::Sell, kPx, 1, OrderType::Limit)));
    EXPECT_EQ(b.rec.trades.size(), 2u);
}

// An order that rested through an auction (which does not apply minQty) and
// has not executed is replaced to a crossing price: 5 available, minimum 50.
TEST(MinQty, AReplaceDoesNotExecuteBelowAnUnmetMinimum) {
    Book b;
    b->setTradingState(TradingState::PreOpen);
    limitWithMin(*b.book, 1, 10, Side::Buy, kPx - 10000, 100, 50);
    b->setTradingState(TradingState::Continuous);
    ASSERT_NE(b->getOrder(1), nullptr);
    ASSERT_TRUE(std::holds_alternative<OrderId>(
        b->addOrder(2, 11, Side::Sell, kPx, 5, OrderType::Limit)));

    RejectReason why = RejectReason::None;
    EXPECT_FALSE(b->cancelReplace(1, kPx, 100, why, 10));
    EXPECT_EQ(why, RejectReason::FOKInsufficientLiquidity);
    EXPECT_TRUE(b.rec.trades.empty()) << "the replace executed 5 of a minimum of 50";
    ASSERT_NE(b->getOrder(1), nullptr);
    EXPECT_EQ(b->getOrder(1)->price, kPx - 10000) << "a refused replace leaves the order as it was";

    // With the minimum available it trades.
    ASSERT_TRUE(std::holds_alternative<OrderId>(
        b->addOrder(3, 12, Side::Sell, kPx, 50, OrderType::Limit)));
    EXPECT_TRUE(b->cancelReplace(1, kPx, 100, why, 10));
    Quantity bought = 0;
    for (const Trade& t : b.rec.trades) bought += t.quantity;
    EXPECT_EQ(bought, 55u);
}

// ─── DecreaseAndCancel tells the resting owner (MR-7) ───────────────────────

TEST(Stp, DecreaseAndCancelReportsTheRestingOrdersNewLeaves) {
    for (MatchAlgorithm algo : {MatchAlgorithm::PriceTime, MatchAlgorithm::ProRata}) {
        SCOPED_TRACE(algoName(algo));
        Book b(algo);
        b->setSTPMode(10, STPMode::DecreaseAndCancel);
        ASSERT_TRUE(std::holds_alternative<OrderId>(
            b->addOrder(1, 10, Side::Sell, kPx, 100, OrderType::Limit)));
        b.rec.ups.clear();
        b->addOrder(2, 10, Side::Buy, kPx, 30, OrderType::Limit);

        ASSERT_NE(b->getOrder(1), nullptr);
        EXPECT_EQ(b->getOrder(1)->remainingQty, 70u);
        ASSERT_TRUE(b.rec.last.count(1)) << "the resting order's owner heard nothing";
        EXPECT_EQ(b.rec.last[1].remainingQty, 70u);
        EXPECT_EQ(b.rec.last[1].filledQty, 0u);
        EXPECT_EQ(b.rec.last[1].status, OrderStatus::Accepted);
        EXPECT_EQ(b.rec.last[2].status, OrderStatus::CancelledBySTP);
        EXPECT_TRUE(b.rec.trades.empty());
    }
}

// ─── CancelBoth in continuous matching ──────────────────────────────────────

TEST(Stp, CancelBothCancelsBothAfterTheFillsAheadOfTheSelfCross) {
    for (MatchAlgorithm algo : {MatchAlgorithm::PriceTime, MatchAlgorithm::ProRata}) {
        SCOPED_TRACE(algoName(algo));
        Book b(algo);
        b->setSTPMode(10, STPMode::CancelBoth);
        ASSERT_TRUE(std::holds_alternative<OrderId>(
            b->addOrder(1, 20, Side::Sell, kPx, 10, OrderType::Limit)));       // another's, better
        ASSERT_TRUE(std::holds_alternative<OrderId>(
            b->addOrder(2, 10, Side::Sell, kPx + 100, 50, OrderType::Limit))); // own
        ASSERT_TRUE(std::holds_alternative<OrderId>(
            b->addOrder(3, 20, Side::Sell, kPx + 200, 50, OrderType::Limit))); // another's, behind
        b->addOrder(4, 10, Side::Buy, kPx + 200, 40, OrderType::Limit);

        ASSERT_EQ(b.rec.trades.size(), 1u) << "the fill ahead of the self-cross is real";
        EXPECT_EQ(b.rec.trades[0].sellOrderId, 1u);
        EXPECT_EQ(b.rec.trades[0].quantity, 10u);
        EXPECT_EQ(b->getOrder(2), nullptr);
        EXPECT_EQ(b.rec.last[2].status, OrderStatus::CancelledBySTP);
        EXPECT_EQ(b->getOrder(4), nullptr) << "the incoming rested past its own order";
        EXPECT_EQ(b.rec.last[4].status, OrderStatus::CancelledBySTP);
        EXPECT_EQ(b.rec.last[4].filledQty, 10u);
        ASSERT_NE(b->getOrder(3), nullptr) << "the sweep went past the self-cross";
        EXPECT_EQ(b->getOrder(3)->remainingQty, 50u);
    }
}

}  // namespace
