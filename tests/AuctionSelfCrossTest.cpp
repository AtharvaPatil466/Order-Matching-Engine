// AuctionSelfCrossTest — a participant's self-crossing orders cannot set the print.
//
// WHAT WAS WRONG (roadmap 1.8-H5, finding AUCT-3). Price discovery counted
// every resting order, including a participant's own buy and sell that
// self-trade prevention would never let trade with each other. STP only ran
// in the execution loop, AFTER the clearing price was fixed. So one
// participant could add a buy and a sell of 1000 at 99 and move an honest
// 105 cross to 99, with the published paired volume going from 100 to 1100;
// STP then cancelled its pair unfilled, so it carried no risk for it.
//
// WHAT IS PINNED. Self-crossing pairs are resolved by the owner's STP mode
// (same newer/older mapping as before) BEFORE discovery. The indicative and
// the print are what the honest orders alone produce, the indicative still
// equals the executed price, and the pair is resolved during the uncross.

#include "EventListener.h"
#include "OrderBook.h"

#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace OrderMatcher;

namespace {

struct Recorder : EventListener {
    std::vector<Trade> trades;
    std::map<OrderId, OrderUpdate> last;
    void onTrade(const Trade& t) override { trades.push_back(t); }
    void onOrderUpdate(const OrderUpdate& u) override { last[u.orderId] = u; }
};

constexpr ParticipantId kManipulator = 99;

// Honest book: buy 105 x100 (reference 105) and sell 95 x100 clear at 105.
void addHonestOrders(OrderBook& book) {
    book.addOrder(1, 10, Side::Buy, 105, 100, OrderType::Limit);
    book.addOrder(2, 20, Side::Sell, 95, 100, OrderType::Limit);
}

void selfCrossDoesNotMoveThePrint() {
    OrderBook book(1);
    Recorder rec;
    book.setEventListener(&rec);
    book.setCircuitBreakerThreshold(1e9);
    book.setTradingState(TradingState::AuctionOpen);
    addHonestOrders(book);
    book.addOrder(3, kManipulator, Side::Buy, 99, 1000, OrderType::Limit);
    book.addOrder(4, kManipulator, Side::Sell, 99, 1000, OrderType::Limit);  // newer

    const AuctionResult a = book.computeAuctionState();
    assert(a.pairedVolume == 100 &&
           "the self-crossing pair inflated the published paired volume");
    assert(a.indicativePrice == 105 && "the self-crossing pair moved the indicative");

    rec.trades.clear();
    book.uncross();
    assert(rec.trades.size() == 1 && rec.trades[0].price == a.indicativePrice &&
           rec.trades[0].price == 105 && rec.trades[0].quantity == 100);
    for (const Trade& t : rec.trades) assert(t.buyerId != t.sellerId);

    // Default mode cancels the newer of the pair (the sell) at the cross; the
    // older buy is a genuine order and rests.
    assert(rec.last[4].status == OrderStatus::CancelledBySTP);
    assert(book.getOrder(3) && book.getOrder(3)->remainingQty == 1000);
    std::string err;
    assert(book.validateIntegrity(&err));
    std::puts("  self-crossing pair moves neither the indicative nor the print");
}

void selfCrossWithMarketOrder() {
    // A parked market order crosses every opposite order of its owner.
    OrderBook book(1);
    Recorder rec;
    book.setEventListener(&rec);
    book.setCircuitBreakerThreshold(1e9);
    book.setTradingState(TradingState::AuctionOpen);
    addHonestOrders(book);
    book.addOrder(3, kManipulator, Side::Sell, 104, 1000, OrderType::Limit);
    book.addOrder(4, kManipulator, Side::Buy, 0, 1000, OrderType::Market);   // newer

    // The newer market buy is the one STP cancels, so the auction is the book
    // without it: the genuine sell 104 x1000 leaves 95 as the only price with
    // no imbalance. With the market buy counted it cleared 1100 at 104.
    const AuctionResult a = book.computeAuctionState();
    assert(a.indicativePrice == 95 && a.pairedVolume == 100 &&
           "a market order against its owner's own sell moved the auction");
    book.uncross();
    assert(rec.last[4].status == OrderStatus::CancelledBySTP);
    assert(book.getSnapshot().lastTradePrice == 95);
    for (const Trade& t : rec.trades) assert(t.buyerId != t.sellerId);
    std::puts("  a parked market order is netted against its owner's own orders");
}

void cancelBothRemovesTheWholePair() {
    OrderBook book(1);
    Recorder rec;
    book.setEventListener(&rec);
    book.setCircuitBreakerThreshold(1e9);
    book.setSTPMode(kManipulator, STPMode::CancelBoth);
    book.setTradingState(TradingState::AuctionOpen);
    addHonestOrders(book);
    book.addOrder(3, kManipulator, Side::Buy, 99, 1000, OrderType::Limit);
    book.addOrder(4, kManipulator, Side::Sell, 99, 1000, OrderType::Limit);

    const AuctionResult a = book.computeAuctionState();
    assert(a.indicativePrice == 105 && a.pairedVolume == 100 && a.imbalanceQty == 0);
    book.uncross();
    assert(rec.last[3].status == OrderStatus::CancelledBySTP);
    assert(rec.last[4].status == OrderStatus::CancelledBySTP);
    assert(book.getSnapshot().lastTradePrice == 105);
    std::string err;
    assert(book.validateIntegrity(&err));
    std::puts("  CancelBoth removes the pair before discovery");
}

}  // namespace

int main() {
    selfCrossDoesNotMoveThePrint();
    selfCrossWithMarketOrder();
    cancelBothRemovesTheWholePair();
    std::puts("AuctionSelfCrossTest passed");
    return 0;
}
