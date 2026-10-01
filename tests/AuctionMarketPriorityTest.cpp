// AuctionMarketPriorityTest — a market order outranks every limit at the cross.
//
// WHAT WAS WRONG (roadmap 1.8-H2, finding AUCT-2 / MATCH-13). uncross() put
// parked market orders into the book AT the clearing price, at the back of
// that level. A limit order at the same price that arrived later filled
// first, and any limit priced through the clearing price filled before them
// too. On an oversubscribed side the market order — the most aggressive
// instruction there is — was cancelled unfilled while a less aggressive limit
// took the fill.
//
// WHAT IS PINNED. On the side with surplus, parked market orders fill first,
// in arrival order, ahead of every limit, whatever its price.

#include "EventListener.h"
#include "OrderBook.h"

#include <cassert>
#include <cstdio>
#include <map>
#include <string>

using namespace OrderMatcher;

namespace {

struct Recorder : EventListener {
    std::map<OrderId, OrderUpdate> last;
    void onOrderUpdate(const OrderUpdate& u) override { last[u.orderId] = u; }
};

void marketBeatsLaterLimitAtClearingPrice() {
    OrderBook book(1);
    Recorder rec;
    book.setEventListener(&rec);
    book.setCircuitBreakerThreshold(1e9);
    book.setTradingState(TradingState::AuctionOpen);

    book.addOrder(1, 10, Side::Buy, 0, 100, OrderType::Market);    // first
    book.addOrder(2, 20, Side::Buy, 100, 100, OrderType::Limit);   // second
    book.addOrder(3, 30, Side::Sell, 100, 100, OrderType::Limit);
    book.uncross();

    assert(rec.last[1].status == OrderStatus::Filled && rec.last[1].filledQty == 100 &&
           "the market order was queued behind a later limit at the clearing price");
    const Order* limit = book.getOrder(2);
    assert(limit && limit->remainingQty == 100 &&
           "the later limit took the fill the market order was owed");
    std::puts("  market order fills ahead of a later limit at the clearing price");
}

void marketBeatsLimitPricedThroughTheCross() {
    OrderBook book(1);
    Recorder rec;
    book.setEventListener(&rec);
    book.setCircuitBreakerThreshold(1e9);
    book.setReferencePrice(100);
    book.setTradingState(TradingState::AuctionOpen);

    book.addOrder(1, 10, Side::Sell, 0, 100, OrderType::Market);   // first
    book.addOrder(2, 20, Side::Sell, 90, 100, OrderType::Limit);   // through the cross
    book.addOrder(3, 30, Side::Buy, 100, 100, OrderType::Limit);
    book.uncross();

    assert(rec.last[1].status == OrderStatus::Filled && rec.last[1].filledQty == 100 &&
           "a limit priced through the cross filled ahead of the market order");
    assert(book.getOrder(2) && book.getOrder(2)->remainingQty == 100);
    std::puts("  market order fills ahead of a limit priced through the cross");
}

void marketsFillInArrivalOrder() {
    OrderBook book(1);
    Recorder rec;
    book.setEventListener(&rec);
    book.setCircuitBreakerThreshold(1e9);
    book.setTradingState(TradingState::AuctionOpen);

    book.addOrder(1, 10, Side::Buy, 0, 60, OrderType::Market);
    book.addOrder(2, 11, Side::Buy, 0, 60, OrderType::Market);
    book.addOrder(3, 30, Side::Sell, 100, 100, OrderType::Limit);
    book.uncross();

    assert(rec.last[1].status == OrderStatus::Filled && rec.last[1].filledQty == 60);
    assert(rec.last[2].status == OrderStatus::Cancelled && rec.last[2].filledQty == 40 &&
           "the second market order should get the remaining 40 and cancel the rest");
    assert(book.getOrder(1) == nullptr && book.getOrder(2) == nullptr &&
           "a market order outlived the cross");
    std::string err;
    assert(book.validateIntegrity(&err));
    std::puts("  market orders fill in arrival order, remainder cancelled");
}

}  // namespace

int main() {
    marketBeatsLaterLimitAtClearingPrice();
    marketBeatsLimitPricedThroughTheCross();
    marketsFillInArrivalOrder();
    std::puts("AuctionMarketPriorityTest passed");
    return 0;
}
