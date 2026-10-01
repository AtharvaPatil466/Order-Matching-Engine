// OnCloseSessionEndTest — an on-close order never outlives its session.
//
// WHAT WAS WRONG (roadmap 1.8-H3, finding AUCT-8 / OTM-9). MOC and LOC orders
// sent before the close wait in onCloseOrders_ and are released only by the
// next transition INTO AuctionClose. Nothing purged that list at PostClose,
// so a session that ended without a closing auction (a halt over the close,
// a volatility auction run into the close) carried acked on-close orders into
// the next day, where they executed at tomorrow's close: an unrequested
// overnight position.
//
// WHAT IS PINNED. Entering PostClose cancels every on-close order still
// parked. A normal closing auction is unaffected.

#include "EventListener.h"
#include "OrderBook.h"

#include <cassert>
#include <cstdio>
#include <map>
#include <string>

using namespace OrderMatcher;

namespace {

struct Recorder : EventListener {
    size_t trades = 0;
    std::map<OrderId, OrderUpdate> last;
    void onTrade(const Trade&) override { ++trades; }
    void onOrderUpdate(const OrderUpdate& u) override { last[u.orderId] = u; }
};

void parkedOnCloseOrdersEndWithTheSession() {
    OrderBook book(1);
    Recorder rec;
    book.setEventListener(&rec);
    book.setCircuitBreakerThreshold(1e9);
    book.addOrder(1, 10, Side::Buy, 100, 100, OrderType::Limit);
    book.addOrder(2, 20, Side::Sell, 0, 50, OrderType::MOC);
    book.addOrder(3, 21, Side::Sell, 99, 50, OrderType::LOC);
    assert(rec.last[2].status == OrderStatus::Accepted);

    // The close never runs: halted over it, then the session ends.
    book.setTradingState(TradingState::Halted);
    book.setTradingState(TradingState::PostClose);
    assert(book.getOrder(2) == nullptr && book.getOrder(3) == nullptr &&
           "an on-close order outlived its session");
    assert(rec.last[2].status == OrderStatus::Cancelled);
    assert(rec.last[3].status == OrderStatus::Cancelled);

    // Next day: the closing cross must not see yesterday's orders.
    book.setTradingState(TradingState::PreOpen);
    book.setTradingState(TradingState::Continuous);
    book.setTradingState(TradingState::AuctionClose);
    book.setTradingState(TradingState::PostClose);
    assert(rec.trades == 0 && "yesterday's MOC traded at today's close");
    std::string err;
    assert(book.validateIntegrity(&err));
    std::puts("  parked MOC/LOC are cancelled at PostClose, not carried to tomorrow");
}

void normalCloseUnaffected() {
    OrderBook book(1);
    Recorder rec;
    book.setEventListener(&rec);
    book.setCircuitBreakerThreshold(1e9);
    book.addOrder(1, 10, Side::Buy, 100, 100, OrderType::Limit);
    book.addOrder(2, 20, Side::Sell, 0, 50, OrderType::MOC);
    book.setTradingState(TradingState::AuctionClose);
    book.setTradingState(TradingState::PostClose);
    assert(rec.last[2].status == OrderStatus::Filled && rec.last[2].filledQty == 50);
    std::puts("  an MOC released into the closing auction still fills");
}

}  // namespace

int main() {
    parkedOnCloseOrdersEndWithTheSession();
    normalCloseUnaffected();
    std::puts("OnCloseSessionEndTest passed");
    return 0;
}
