// AuctionOpenAtomicTest — nothing trades in an auction state except the uncross,
// and leaving the auction always runs it.
//
// WHAT WAS WRONG (roadmap 1.8-H4, findings AUCT-4 / MATCH-12 / TLA-3 / MATCH-6).
//   - The open was two separate calls: uncross, then setTradingState(
//     Continuous). setTradingState only assigned the field, so orders that
//     arrived in between were admitted under auction rules and carried into
//     continuous trading: a bid 120 over an ask 101 with no trade (a crossed
//     book), and a market order parked all day until the close.
//   - In an auction state an order's arrival still ran the stop sweep, so a
//     stop elected on the stale last print matched continuously; and a
//     cancelReplace that crossed matched on the spot. Both printed trades
//     during the call period, at non-auction prices.
//
// WHAT IS PINNED. Moving an accumulation state to Continuous or PostClose
// runs the uncross under the same lock as the flip. In an accumulation state
// neither the stop sweep nor a replace matches.

#include "EventListener.h"
#include "MatchingEngine.h"
#include "OrderBook.h"

#include <cassert>
#include <cstdio>
#include <limits>
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

bool crossed(const OrderBook& book) {
    const Price bid = book.getBestBid();
    const Price ask = book.getBestAsk();
    return bid > 0 && ask < std::numeric_limits<Price>::max() && bid >= ask;
}

void openingFlipUncrossesGapOrders() {
    OrderBook book(1);
    Recorder rec;
    book.setEventListener(&rec);
    book.setCircuitBreakerThreshold(1e9);
    book.setTradingState(TradingState::PreOpen);
    book.addOrder(1, 10, Side::Buy, 100, 100, OrderType::Limit);
    book.addOrder(2, 20, Side::Sell, 100, 100, OrderType::Limit);
    book.uncross();                                               // the opening cross

    // Arrive after the cross, before the flip: still admitted as PreOpen.
    book.addOrder(3, 30, Side::Buy, 0, 50, OrderType::Market);
    book.addOrder(4, 31, Side::Buy, 120, 50, OrderType::Limit);
    book.addOrder(5, 40, Side::Sell, 101, 50, OrderType::Limit);
    book.setTradingState(TradingState::Continuous);

    assert(!crossed(book) && "continuous trading opened on a crossed book");
    assert(book.getOrder(3) == nullptr &&
           "a market order from the auction survived into continuous trading");
    std::string err;
    assert(book.validateIntegrity(&err));
    std::puts("  the flip to Continuous uncrosses orders that arrived after the cross");
}

void closingFlipUncrossesGapOrders() {
    OrderBook book(1);
    Recorder rec;
    book.setEventListener(&rec);
    book.setCircuitBreakerThreshold(1e9);
    book.setTradingState(TradingState::AuctionClose);
    book.addOrder(1, 10, Side::Buy, 100, 100, OrderType::Limit);
    book.addOrder(2, 20, Side::Sell, 0, 40, OrderType::MOC);
    book.setTradingState(TradingState::PostClose);

    assert(book.getOrder(2) == nullptr && "an MOC outlived the close");
    assert(rec.last[2].status == OrderStatus::Filled && rec.last[2].filledQty == 40);
    std::puts("  the flip to PostClose runs the closing cross");
}

void engineBatchFlipIsTheCross() {
    MatchingEngine engine;
    engine.addSymbol(0);
    engine.start();
    engine.getOrderBook(0)->setCircuitBreakerThreshold(1e9);
    engine.setTradingStateBatch({0}, TradingState::PreOpen);
    engine.submitOrder(0, 1, 10, Side::Buy, 105, 100, OrderType::Limit);
    engine.submitOrder(0, 2, 20, Side::Sell, 101, 100, OrderType::Limit);
    engine.setTradingStateBatch({0}, TradingState::Continuous);
    assert(!crossed(*engine.getOrderBook(0)) && engine.getOrderBook(0)->getTradeCount() == 1 &&
           "setTradingStateBatch(Continuous) opened a crossed book without a cross");
    engine.stop();
    std::puts("  setTradingStateBatch(Continuous) is the opening cross");
}

void stopSweepDoesNotMatchInAuction() {
    OrderBook book(1);
    Recorder rec;
    book.setEventListener(&rec);
    book.setCircuitBreakerThreshold(1e9);
    book.addOrder(1, 10, Side::Buy, 100, 10, OrderType::Limit);
    book.addOrder(2, 20, Side::Sell, 100, 10, OrderType::Limit);   // last print 100
    assert(rec.trades == 1);

    book.setTradingState(TradingState::PreOpen);
    book.addOrder(3, 30, Side::Sell, 101, 10, OrderType::Limit);
    book.addOrder(4, 40, Side::Buy, 105, 10, OrderType::Stop, /*stopPrice=*/100);
    // Any later arrival runs the sweep, which elects the stop on the stale
    // 100 print.
    book.addOrder(5, 50, Side::Buy, 90, 1, OrderType::Limit);
    assert(rec.trades == 1 && "an elected stop traded during the pre-open");
    std::puts("  the stop sweep does not match in an auction state");
}

void replaceDoesNotMatchInAuction() {
    OrderBook book(1);
    Recorder rec;
    book.setEventListener(&rec);
    book.setCircuitBreakerThreshold(1e9);
    book.setTradingState(TradingState::PreOpen);
    book.addOrder(1, 10, Side::Buy, 99, 10, OrderType::Limit);
    book.addOrder(2, 20, Side::Sell, 101, 10, OrderType::Limit);
    assert(book.cancelReplace(1, 102, 10));
    assert(rec.trades == 0 && "a crossing replace traded during the pre-open");
    assert(book.getOrder(1) && book.getOrder(1)->price == 102);

    book.setTradingState(TradingState::Continuous);  // the opening cross
    assert(rec.trades == 1 && !crossed(book));
    std::puts("  a crossing replace rests until the uncross");
}

}  // namespace

int main() {
    openingFlipUncrossesGapOrders();
    closingFlipUncrossesGapOrders();
    engineBatchFlipIsTheCross();
    stopSweepDoesNotMatchInAuction();
    replaceDoesNotMatchInAuction();
    std::puts("AuctionOpenAtomicTest passed");
    return 0;
}
