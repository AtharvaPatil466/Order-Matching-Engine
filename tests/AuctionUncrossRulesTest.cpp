// AuctionUncrossRulesTest — round-8 review fixes to the uncross (MIDS-4, MIDS-5)
// and to on-close orders that outlive a halted close.
//
// MIDS-4: removing a parked market order (cancel, STP) swap-erased it, so the
//   last parked order jumped into its slot and the cross filled it ahead of
//   orders that arrived before it. The parked lists keep arrival order.
// MIDS-5: the self-cross plan cancelled a participant's buy and sell whenever
//   they crossed each other, even when one of them could not trade at the
//   clearing price. A 100-lot cross at 105 became no cross. Only pairs that
//   are both executable at the clearing price are resolved now (Rule 4.4).
// Stale on-close: AuctionClose -> Halted -> PostClose ran no cross, so released
//   MOCs, LOCs and parked market orders survived into the next session.

#include "EventListener.h"
#include "OrderBook.h"

#include <gtest/gtest.h>

#include <map>
#include <vector>

using namespace OrderMatcher;

namespace {

struct Recorder : EventListener {
    std::vector<Trade> trades;
    std::map<OrderId, OrderUpdate> last;
    void onTrade(const Trade& t) override { trades.push_back(t); }
    void onOrderUpdate(const OrderUpdate& u) override { last[u.orderId] = u; }
};

struct Book {
    OrderBook book{0};
    Recorder rec;
    explicit Book(TradingState s) {
        book.setEventListener(&rec);
        book.setCircuitBreakerThreshold(1e9);
        book.setTradingState(s);
    }
};

}  // namespace

TEST(AuctionPriority, CancellingAParkedMarketOrderKeepsTheOthersInArrivalOrder) {
    Book b(TradingState::AuctionOpen);
    b.book.addOrder(1, 11, Side::Buy, 0, 5, OrderType::Market);
    b.book.addOrder(2, 12, Side::Buy, 0, 5, OrderType::Market);
    b.book.addOrder(3, 13, Side::Buy, 0, 5, OrderType::Market);
    b.book.addOrder(9, 19, Side::Sell, 1000, 5, OrderType::Limit);
    b.book.cancelOrder(1);
    b.book.uncross();
    ASSERT_EQ(b.rec.trades.size(), 1u);
    EXPECT_EQ(b.rec.trades[0].buyOrderId, 2u) << "#2 arrived before #3";
}

TEST(AuctionPriority, AnStpCutParkedMarketOrderKeepsTheOthersInArrivalOrder) {
    Book b(TradingState::AuctionOpen);
    b.book.addOrder(8, 7, Side::Sell, 1000, 5, OrderType::Limit);  // P, older
    b.book.addOrder(1, 11, Side::Buy, 0, 5, OrderType::Market);
    b.book.addOrder(2, 7, Side::Buy, 0, 5, OrderType::Market);     // P, newer: cut
    b.book.addOrder(3, 12, Side::Buy, 0, 5, OrderType::Market);
    b.book.addOrder(4, 13, Side::Buy, 0, 5, OrderType::Market);
    b.book.addOrder(9, 19, Side::Sell, 1000, 5, OrderType::Limit);
    b.book.uncross();
    EXPECT_EQ(b.rec.last[2].status, OrderStatus::CancelledBySTP);
    ASSERT_EQ(b.rec.trades.size(), 2u);
    EXPECT_EQ(b.rec.trades[0].buyOrderId, 1u);
    EXPECT_EQ(b.rec.trades[1].buyOrderId, 3u) << "#3 arrived before #4";
}

TEST(AuctionPriority, CancellingAnMocKeepsTheOthersInArrivalOrderAtTheClose) {
    Book b(TradingState::Continuous);
    b.book.addOrder(1, 11, Side::Buy, 0, 5, OrderType::MOC);
    b.book.addOrder(2, 12, Side::Buy, 0, 5, OrderType::MOC);
    b.book.addOrder(3, 13, Side::Buy, 0, 5, OrderType::MOC);
    b.book.cancelOrder(1);
    b.book.setTradingState(TradingState::AuctionClose);
    b.book.addOrder(9, 19, Side::Sell, 1000, 5, OrderType::Limit);
    b.book.setTradingState(TradingState::PostClose);
    ASSERT_EQ(b.rec.trades.size(), 1u);
    EXPECT_EQ(b.rec.trades[0].buyOrderId, 2u);
}

TEST(StaleOnClose, AHaltedCloseCancelsReleasedMocLocAndParkedMarketOrders) {
    Book b(TradingState::Continuous);
    b.book.addOrder(1, 11, Side::Buy, 0, 5, OrderType::MOC);
    b.book.addOrder(2, 12, Side::Sell, 1010, 5, OrderType::LOC);
    b.book.setTradingState(TradingState::AuctionClose);
    b.book.addOrder(3, 13, Side::Buy, 0, 5, OrderType::Market);
    b.book.setTradingState(TradingState::Halted);
    b.book.setTradingState(TradingState::PostClose);
    EXPECT_TRUE(b.rec.trades.empty());
    for (OrderId id : {1u, 2u, 3u}) {
        EXPECT_EQ(b.book.getOrder(id), nullptr) << "#" << id << " survived the close";
        EXPECT_EQ(b.rec.last[id].status, OrderStatus::Cancelled) << "#" << id;
    }
}

TEST(StaleOnClose, AHaltedVolatilityAuctionCancelsItsParkedMarketOrders) {
    Book b(TradingState::Continuous);
    b.book.setTradingState(TradingState::VolatilityAuction);
    b.book.addOrder(3, 13, Side::Buy, 0, 5, OrderType::Market);
    b.book.setTradingState(TradingState::Halted);
    b.book.setTradingState(TradingState::PostClose);
    EXPECT_EQ(b.book.getOrder(3), nullptr);
    EXPECT_EQ(b.rec.last[3].status, OrderStatus::Cancelled);
}

