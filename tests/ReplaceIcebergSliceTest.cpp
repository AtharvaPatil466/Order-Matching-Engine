// ReplaceIcebergSliceTest — a repriced iceberg rests with a correct slice
// (roadmap 0.11, finding MATCH-3).
//
// WHAT WAS WRONG. cancelReplace's price-change branch never re-sliced an
// iceberg. Every other rest path sets visibleQty = min(remainingQty,
// displayQty); this one kept the old slice. A repriced iceberg that trades as
// an aggressor comes out with less remaining than that slice, and rested
// showing it: visibleQty > remainingQty. The next aggressor was filled for the
// slice, more than the order held, and remainingQty wrapped to ~2^64. At HEAD:
// iceberg 100/10 repriced onto a 95 bid kept visibleQty 10 over remainingQty 5,
// and an IOC for 10 received 10, leaving remainingQty 18446744073709551611.
//
// WHAT IS PINNED. After a crossing reprice the book advertises what remains,
// an aggressor receives no more than that, and a non-crossing reprice
// re-enters the book with a fresh slice.

#include "OrderBook.h"

#include <cassert>
#include <cstdio>
#include <vector>

using namespace OrderMatcher;

namespace {

constexpr Price kPx = 10000;

struct Tape : EventListener {
    std::vector<Trade> trades;
    void onTrade(const Trade& t) override { trades.push_back(t); }

    Quantity executedBy(OrderId id) const {
        Quantity q = 0;
        for (const auto& t : trades)
            if (t.buyOrderId == id || t.sellOrderId == id) q += t.quantity;
        return q;
    }
};

void crossingRepriceReslices() {
    OrderBook book(1);
    book.setCircuitBreakerThreshold(1e9);
    Tape tape;
    book.setEventListener(&tape);

    // Bid 95 @ kPx-100. Sell iceberg 100, display 10, resting away at kPx+100.
    book.addOrder(1, 1, Side::Buy, kPx - 100, 95, OrderType::Limit);
    book.addOrder(10, 2, Side::Sell, kPx + 100, 100, OrderType::Iceberg, 0, /*displayQty=*/10);

    // Reprice onto the bid, same quantity: trades 95 as aggressor, 5 remain.
    assert(book.cancelReplace(10, kPx - 100, 100));
    const Order* ice = book.getOrder(10);
    assert(ice && ice->remainingQty == 5);
    assert(ice->visibleQty <= ice->remainingQty &&
           "repriced iceberg rests showing more than it holds (stale slice)");
    const auto snap = book.getSnapshot();
    assert(snap.askCount == 1 && snap.asks[0].totalQuantity == 5 &&
           "the book advertises the pre-trade slice, not the 5 that remain");

    // An aggressor for 10 must receive 5, and the iceberg must not wrap.
    book.addOrder(20, 3, Side::Buy, kPx - 100, 10, OrderType::IOC);
    assert(tape.executedBy(20) == 5 && "the iceberg delivered more than it held");
    assert(book.getOrder(10) == nullptr && "iceberg fully executed and gone");
    std::puts("  crossing reprice: slice = remaining, delivers no more than it holds");
}

void nonCrossingRepriceTakesAFreshSlice() {
    OrderBook book(1);
    book.setCircuitBreakerThreshold(1e9);

    book.addOrder(30, 2, Side::Sell, kPx + 100, 100, OrderType::Iceberg, 0, /*displayQty=*/10);
    book.addOrder(31, 4, Side::Buy, kPx + 100, 7, OrderType::IOC);     // slice 10 -> 3
    assert(book.getOrder(30)->remainingQty == 93);
    assert(book.cancelReplace(30, kPx + 200, 93));
    assert(book.getOrder(30)->visibleQty == 10 &&
           "a repriced iceberg re-enters the book with min(remaining, displayQty)");
    std::puts("  non-crossing reprice: fresh slice of min(remaining, displayQty)");
}

}  // namespace

int main() {
    crossingRepriceReslices();
    nonCrossingRepriceTakesAFreshSlice();
    std::puts("ReplaceIcebergSliceTest passed");
    return 0;
}
