// OrderLifecycleTest — every way an order ends, or comes to rest, is reported
// (roadmap 1.5-E3).
//
// The rest-or-finish tail was hand-copied at each place an order leaves the
// matcher, and the copies drifted. Each case below is one copy that broke.

#include "OrderBook.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace OrderMatcher;

namespace {

constexpr Price PX = 10000;

struct Capture : EventListener {
    std::vector<OrderUpdate> updates;
    std::vector<MarketDataUpdate> md;
    void onOrderUpdate(const OrderUpdate& u) override { updates.push_back(u); }
    void onMarketData(const MarketDataUpdate& m) override { md.push_back(m); }

    const OrderUpdate* lastFor(OrderId id) const {
        const OrderUpdate* found = nullptr;
        for (const auto& u : updates) if (u.orderId == id) found = &u;
        return found;
    }
};

void checkIntegrity(const OrderBook& book) {
    std::string err;
    if (!book.validateIntegrity(&err)) std::printf("  integrity: %s\n", err.c_str());
    assert(book.validateIntegrity());
}

// MATCH-11: screenMinQty rested a non-crossing minQty order with the return of
// addToBook ignored. When the book had no room for a new level, the order
// stayed acked and in the lookup, but in no price level: live to its owner,
// invisible to every aggressor, never told.
void minQtyRestThatDoesNotFitIsCancelled() {
    OrderBook book(1);
    book.setCircuitBreakerThreshold(1e9);
    book.setMaxDepth(1);
    Capture cap;
    book.setEventListener(&cap);

    book.addOrder(1, 1, Side::Buy, PX - 100, 10, OrderType::Limit);   // the one bid level
    book.addOrder(2, 2, Side::Sell, PX + 100, 10, OrderType::Limit);
    // Non-crossing, minQty unmet: it rests — at a second bid level, no room.
    book.addOrder(3, 3, Side::Buy, PX, 10, OrderType::Limit, 0, 0, TimeInForce::GTC,
                  0, 0, PegType::None, 0, 0, /*minQty=*/5);

    if (book.getOrder(3))
        std::printf("  FAIL order 3 is live with %s, but rests in no level\n",
                    cap.lastFor(3) ? "an update" : "no terminal update");
    assert(book.getOrder(3) == nullptr && "an order that cannot rest must not stay live");
    const OrderUpdate* u = cap.lastFor(3);
    assert(u && u->status == OrderStatus::Cancelled && "and its owner is told");
    checkIntegrity(book);
    std::puts("  minQty rest without room: Cancelled, not left live outside the book");
}

// MATCH-8: the stop and trailing-stop sweeps carried their own copy of the
// post-match tail. An elected stop that filled completely was freed with no
// Filled update, and one that rested was never announced to the L2 feed.
void electedStopThatFillsIsReportedFilled() {
    OrderBook book(1);
    book.setCircuitBreakerThreshold(1e9);
    Capture cap;
    book.setEventListener(&cap);

    book.addOrder(1, 1, Side::Sell, PX, 10, OrderType::Limit);
    book.addOrder(2, 2, Side::Buy, PX, 5, OrderType::Stop, /*stopPrice=*/PX);
    book.addOrder(3, 3, Side::Buy, PX, 1, OrderType::Limit);   // prints PX: elects 2

    const OrderUpdate* u = cap.lastFor(2);
    if (!u || u->status != OrderStatus::Filled)
        std::printf("  FAIL elected stop 2 filled 5 and is gone (%s), last update %s\n",
                    book.getOrder(2) ? "live" : "freed",
                    u ? std::to_string(static_cast<int>(u->status)).c_str() : "none");
    assert(book.getOrder(2) == nullptr);
    assert(u && u->status == OrderStatus::Filled && u->filledQty == 5 &&
           "an elected stop that fills completely gets its Filled update");
    checkIntegrity(book);
    std::puts("  elected stop, fully filled: Filled update");
}

void electedStopThatRestsIsPublished() {
    OrderBook book(1);
    book.setCircuitBreakerThreshold(1e9);
    Capture cap;
    book.setEventListener(&cap);

    book.addOrder(1, 1, Side::Sell, PX, 2, OrderType::Limit);
    book.addOrder(2, 2, Side::Buy, PX, 5, OrderType::Stop, /*stopPrice=*/PX);
    cap.md.clear();
    book.addOrder(3, 3, Side::Buy, PX, 1, OrderType::Limit);   // prints PX: elects 2
    // Stop 2 takes the 1 left and rests 4 @ PX as a limit.
    assert(book.getOrder(2) && book.getOrder(2)->remainingQty == 4);

    bool announced = false;
    for (const auto& m : cap.md)
        announced |= m.action == MarketDataUpdate::Action::Add && m.side == Side::Buy &&
                     m.level.price == PX;
    if (!announced) std::puts("  FAIL elected stop 2 rests 4 @ PX with no market-data Add");
    assert(announced && "an elected stop that rests is announced like any rest");
    checkIntegrity(book);
    std::puts("  elected stop, rests: market-data Add");
}

}  // namespace

int main() {
    minQtyRestThatDoesNotFitIsCancelled();
    electedStopThatFillsIsReportedFilled();
    electedStopThatRestsIsPublished();
    std::puts("OrderLifecycleTest passed");
    return 0;
}
