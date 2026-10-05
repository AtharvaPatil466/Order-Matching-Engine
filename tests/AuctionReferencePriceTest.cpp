// AuctionReferencePriceTest — the clearing-price tie rules of roadmap 1.8-H7.
//
// WHAT WAS WRONG (finding AUCT-14). Price discovery only considers limit
// prices. A call period that collected market orders on both sides and no
// limits had no candidate price at all, so the cross never ran: every market
// order was cancelled unfilled although buyers and sellers were both there.
// Venues cross that book at the reference price.
//
// WHAT IS PINNED.
//   1. Market orders alone on both sides cross at the last trade price (the
//      book keeps no separate prior close), for min(buy, sell); the surplus
//      side's remainder is cancelled. The indicative says the same.
//   2. With no trade yet there is no reference, and nothing crosses.
//   3. Tie rules, docs/MatchingRules.md Rule 7.2: on equal volume and zero
//      imbalance the price nearest the reference wins; with no reference the
//      HIGHEST tied price wins.

#include "EventListener.h"
#include "OrderBook.h"

#include <cassert>
#include <cstdio>
#include <map>
#include <vector>

using namespace OrderMatcher;

namespace {

struct Recorder : EventListener {
    std::map<OrderId, OrderUpdate> last;
    std::vector<Trade> trades;
    void onOrderUpdate(const OrderUpdate& u) override { last[u.orderId] = u; }
    void onTrade(const Trade& t) override { trades.push_back(t); }
};

void marketsOnlyCrossAtLastTradePrice() {
    OrderBook book(1);
    Recorder rec;
    book.setEventListener(&rec);
    book.setCircuitBreakerThreshold(1e9);

    // A continuous print at 100 is the reference.
    book.addOrder(1, 1, Side::Buy, 100, 10, OrderType::Limit);
    book.addOrder(2, 2, Side::Sell, 100, 10, OrderType::Limit);
    assert(rec.trades.size() == 1 && rec.trades[0].price == 100);

    book.setTradingState(TradingState::PreOpen);
    book.addOrder(3, 3, Side::Buy, 0, 60, OrderType::Market);
    book.addOrder(4, 4, Side::Sell, 0, 40, OrderType::Market);

    const AuctionResult ind = book.computeAuctionState();
    assert(ind.hasCross && ind.indicativePrice == 100 && ind.pairedVolume == 40 &&
           "the indicative showed no cross for a book of market orders on both sides");
    assert(ind.imbalanceQty == 20 && ind.imbalanceSide == Side::Buy);

    book.setTradingState(TradingState::Continuous);  // the opening cross
    assert(rec.trades.size() == 2 &&
           "market orders on both sides were cancelled instead of crossed");
    assert(rec.trades[1].price == 100 && rec.trades[1].quantity == 40);
    assert(rec.last[4].status == OrderStatus::Filled);
    assert(rec.last[3].status == OrderStatus::Cancelled && rec.last[3].filledQty == 40 &&
           "the surplus market buy's remainder must be cancelled after the cross");
    assert(book.getOrder(3) == nullptr && book.getOrder(4) == nullptr);
    std::puts("  market orders alone cross at the last trade price");
}

void marketsOnlyWithNoTradeDoNotCross() {
    OrderBook book(1);
    Recorder rec;
    book.setEventListener(&rec);
    book.setCircuitBreakerThreshold(1e9);
    book.setTradingState(TradingState::PreOpen);
    book.addOrder(1, 1, Side::Buy, 0, 50, OrderType::Market);
    book.addOrder(2, 2, Side::Sell, 0, 50, OrderType::Market);

    assert(!book.computeAuctionState().hasCross);
    book.setTradingState(TradingState::Continuous);
    assert(rec.trades.empty() && "there is no price to cross at before any trade");
    assert(rec.last[1].status == OrderStatus::Cancelled);
    assert(rec.last[2].status == OrderStatus::Cancelled);
    std::puts("  with no trade yet, market orders alone do not cross");
}

// Bid 10 @ 101 and ask 10 @ 100: both candidates pair 10 with zero imbalance.
Price clearingPriceWithReference(Price reference) {
    OrderBook book(1);
    book.setCircuitBreakerThreshold(1e9);
    book.setTradingState(TradingState::PreOpen);
    book.addOrder(1, 1, Side::Buy, 101, 10, OrderType::Limit);
    book.addOrder(2, 2, Side::Sell, 100, 10, OrderType::Limit);
    book.setReferencePrice(reference);  // the first order seeded it; override
    const AuctionResult r = book.computeAuctionState();
    assert(r.hasCross && r.pairedVolume == 10 && r.imbalanceQty == 0);
    return r.indicativePrice;
}

void zeroImbalanceTieRules() {
    assert(clearingPriceWithReference(100) == 100 && "nearest the reference wins the tie");
    assert(clearingPriceWithReference(101) == 101 && "nearest the reference wins the tie");
    assert(clearingPriceWithReference(0) == 101 &&
           "with no reference, the highest tied price wins");
    std::puts("  zero-imbalance ties: nearest the reference, else the highest price");
}

}  // namespace

int main() {
    marketsOnlyCrossAtLastTradePrice();
    marketsOnlyWithNoTradeDoNotCross();
    zeroImbalanceTieRules();
    std::puts("AuctionReferencePriceTest passed");
    return 0;
}
