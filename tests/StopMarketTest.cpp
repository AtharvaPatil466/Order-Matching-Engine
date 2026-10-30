// StopMarketTest — audit OTM-4, roadmap 1.6-F2.
//
// WHAT WAS WRONG. An elected Stop became a Limit at its submitted `price`,
// and nothing validated the trigger fields. So the FIX-shaped stop-market (no
// limit price, price 0) swept the bids to zero and rested its remainder as an
// ask at 0, where the next buyer lifted it at 0 (the 0.12 stop-gap refused
// price 0 instead). StopLimit is exempt from the price check and its
// stopLimitPrice was never checked, so StopLimit with stopLimitPrice 0 did the
// same; a sell Stop with stopPrice 0 could never trigger and a buy one
// triggered on the next print; a buy TrailingStop with a negative trail
// elected at once and rested at a negative price.
//
// WHAT IS PINNED.
//   * Stop is stop-market: elected, it trades as a Market order — its `price`
//     is not a limit — and any remainder is cancelled. Nothing rests.
//   * StopLimit is the priced form and rests at stopLimitPrice.
//   * Admission refuses stopPrice <= 0 (Stop, StopLimit, MIT), stopLimitPrice
//     <= 0 (StopLimit), and a TrailingStop whose trail is not inside
//     (0, reference), with InvalidPrice.

#include "OrderBook.h"

#include <cassert>
#include <cstdio>
#include <variant>
#include <vector>

using namespace OrderMatcher;

namespace {

struct Recorder : EventListener {
    std::vector<Trade> trades;
    std::vector<OrderUpdate> updates;
    void onTrade(const Trade& t) override { trades.push_back(t); }
    void onOrderUpdate(const OrderUpdate& u) override { updates.push_back(u); }
    const OrderUpdate* last(OrderId id) const {
        for (auto it = updates.rbegin(); it != updates.rend(); ++it)
            if (it->orderId == id) return &*it;
        return nullptr;
    }
};

bool accepted(const AddOrderResult& r) { return std::holds_alternative<OrderId>(r); }
bool rejectedWith(const AddOrderResult& r, RejectReason want) {
    return std::holds_alternative<RejectReason>(r) && std::get<RejectReason>(r) == want;
}

void relaxed(OrderBook& b) { b.setCircuitBreakerThreshold(1e9); }

// Bids 4950x1, 4900x5, 4000x5. A sell Stop 30 triggers at 4950; the print is
// a sell 1 @ 4950 against the top bid.
void electedStopTradesAsMarketAndCancelsTheRest(Price stopLimit) {
    OrderBook book(1);
    relaxed(book);
    Recorder rec;
    book.setEventListener(&rec);
    book.addOrder(1, 1, Side::Buy, 4950, 1, OrderType::Limit);
    book.addOrder(2, 1, Side::Buy, 4900, 5, OrderType::Limit);
    book.addOrder(3, 1, Side::Buy, 4000, 5, OrderType::Limit);

    const auto r = book.addOrder(10, 2, Side::Sell, stopLimit, 30, OrderType::Stop, 4950);
    if (!accepted(r)) {
        std::fprintf(stderr, "Stop price=%lld stopPrice=4950: rejected reason=%u\n",
                     (long long)stopLimit, (unsigned)std::get<RejectReason>(r));
        assert(false && "a stop-market (Stop) was refused");
    }

    book.addOrder(11, 3, Side::Sell, 4950, 1, OrderType::Limit);  // prints 4950

    Quantity stopFilled = 0;
    for (const Trade& t : rec.trades)
        if (t.sellOrderId == 10) stopFilled += t.quantity;
    const OrderUpdate* fin = rec.last(10);
    std::printf("  [price=%lld] stop filled=%llu inBook=%d bestAsk=%lld bestBid=%lld "
                "final status=%u\n",
                (long long)stopLimit, (unsigned long long)stopFilled,
                book.getOrder(10) != nullptr, (long long)book.getBestAsk(),
                (long long)book.getBestBid(), fin ? (unsigned)fin->status : 99u);

    assert(stopFilled == 10 && "the elected stop must sweep 4900 and 4000 like a market sell");
    assert(book.getOrder(10) == nullptr && "an elected stop-market must not rest");
    assert(book.getAskLevelsCount() == 0 && "nothing may rest on the ask side");
    assert(book.getBidLevelsCount() == 0);
    assert(fin && fin->status == OrderStatus::Cancelled && fin->filledQty == 10 &&
           fin->remainingQty == 0 && "the stop's remainder is cancelled, filled 10");
}

void stopLimitRestsAtItsLimit() {
    OrderBook book(1);
    relaxed(book);
    book.addOrder(1, 1, Side::Buy, 4950, 1, OrderType::Limit);
    book.addOrder(2, 1, Side::Buy, 4900, 5, OrderType::Limit);
    book.addOrder(3, 1, Side::Buy, 4000, 5, OrderType::Limit);
    assert(accepted(book.addOrder(10, 2, Side::Sell, 0, 30, OrderType::StopLimit, 4950,
                                  0, TimeInForce::GTC, 0, /*stopLimitPrice=*/4800)));
    book.addOrder(11, 3, Side::Sell, 4950, 1, OrderType::Limit);
    const Order* o = book.getOrder(10);
    assert(o && o->remainingQty == 25 && o->price == 4800 &&
           "StopLimit sells 5 @ 4900, stops at its 4800 limit and rests 25 there");
    assert(book.getBestAsk() == 4800 && book.getBestBid() == 4000);
}

void triggerFieldsMustBePositive() {
    OrderBook book(1);
    relaxed(book);
    book.addOrder(1, 1, Side::Buy, 5000, 10, OrderType::Limit);
    book.addOrder(2, 1, Side::Sell, 5100, 10, OrderType::Limit);
    book.addOrder(3, 3, Side::Buy, 5100, 1, OrderType::Limit);  // last print 5100

    struct Case { const char* what; OrderId id; Side side; OrderType type; Price stop;
                  Price stopLimit; Price trail; };
    const Case bad[] = {
        {"sell Stop stopPrice 0",          20, Side::Sell, OrderType::Stop,      0,    0,    0},
        {"buy Stop stopPrice -1",          21, Side::Buy,  OrderType::Stop,     -1,    0,    0},
        {"StopLimit stopLimitPrice 0",     22, Side::Sell, OrderType::StopLimit, 4900, 0,    0},
        {"StopLimit stopPrice 0",          23, Side::Sell, OrderType::StopLimit, 0,    4900, 0},
        {"sell MIT stopPrice 0",           24, Side::Sell, OrderType::MIT,       0,    0,    0},
        {"sell trail == reference",        25, Side::Sell, OrderType::TrailingStop, 0, 0, 5100},
        {"sell trail > reference",         26, Side::Sell, OrderType::TrailingStop, 0, 0, 9000},
        {"buy trail negative",             27, Side::Buy,  OrderType::TrailingStop, 0, 0, -6000},
        {"buy trail 0",                    28, Side::Buy,  OrderType::TrailingStop, 0, 0, 0},
    };
    for (const Case& c : bad) {
        // price 4000 so no case is refused for its (unused) limit price.
        const auto r = book.addOrder(c.id, 2, c.side, 4000, 5, c.type, c.stop, 0,
                                     TimeInForce::GTC, 0, c.stopLimit, PegType::None, 0,
                                     c.trail);
        if (!rejectedWith(r, RejectReason::InvalidPrice)) {
            std::fprintf(stderr, "%s: %s\n", c.what,
                         accepted(r) ? "ACCEPTED" : "rejected with another reason");
            assert(false && "a stop with an unusable trigger field was admitted");
        }
    }

    // Controls: the same shapes with usable fields are admitted.
    assert(accepted(book.addOrder(30, 2, Side::Sell, 0, 5, OrderType::Stop, 4900)));
    assert(accepted(book.addOrder(31, 2, Side::Sell, 0, 5, OrderType::StopLimit, 4900, 0,
                                  TimeInForce::GTC, 0, 4800)));
    assert(accepted(book.addOrder(32, 2, Side::Sell, 0, 5, OrderType::MIT, 5200)));
    assert(accepted(book.addOrder(33, 2, Side::Sell, 0, 5, OrderType::TrailingStop, 0, 0,
                                  TimeInForce::GTC, 0, 0, PegType::None, 0, 100)));
    assert(book.getBestBid() == 5000 && book.getBestAsk() == 5100);
    std::puts("  stopPrice / stopLimitPrice <= 0 and trails outside (0, reference) rejected");
}

}  // namespace

int main() {
    electedStopTradesAsMarketAndCancelsTheRest(/*price=*/0);
    electedStopTradesAsMarketAndCancelsTheRest(/*price=*/4950);  // price is not a limit
    stopLimitRestsAtItsLimit();
    triggerFieldsMustBePositive();
    std::puts("StopMarketTest passed");
    return 0;
}
