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

}  // namespace

int main() {
    minQtyRestThatDoesNotFitIsCancelled();
    std::puts("OrderLifecycleTest passed");
    return 0;
}
