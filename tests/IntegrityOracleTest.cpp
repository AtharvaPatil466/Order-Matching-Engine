// IntegrityOracleTest — validateIntegrity() refuses the quantity states the
// audit found (roadmap 1.3-C2, finding MATCH-14; decision 3 of lane I).
//
// WHAT WAS WRONG. validateIntegrity checked only that the price levels and
// orderLookup_ agree. It returned true on an order resting at zero quantity
// (MATCH-1/2), on an iceberg showing more than it holds (MATCH-3) and on an
// order whose fills and leaves do not add up to its size (MATCH-4), so the
// differential fuzzer and every other caller that leans on it could not see
// those defects.
//
// WHAT IS PINNED. Each bad state is injected directly — the routes that used
// to produce them are fixed, and a test that needs a live bug to fail stops
// testing anything once the bug is gone. A healthy book, partially filled and
// replaced orders included, still passes.
//
// NOT YET: a crossed book in Continuous. That check exposes a live defect
// (an elected trailing stop rests as a limit opposite an iceberg at the same
// price, MATCH-5) that roadmap 1.6-F3 fixes; it lands with that fix.

#include "OrderBook.h"

#include <cstdio>
#include <string>

using namespace OrderMatcher;

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

bool valid(const OrderBook& book) {
    std::string err;
    const bool ok = book.validateIntegrity(&err);
    if (!ok) std::printf("     validateIntegrity: %s\n", err.c_str());
    return ok;
}

// Fault injection: the oracle is what is under test, so the book is put into
// the state by hand.
Order& mutate(OrderBook& book, OrderId id) {
    return const_cast<Order&>(*book.getOrder(id));
}

void healthyBookPasses() {
    OrderBook book(1);
    book.setCircuitBreakerThreshold(1e9);
    book.addOrder(1, 1, Side::Buy, 9900, 100, OrderType::Limit);
    book.addOrder(2, 2, Side::Sell, 10100, 100, OrderType::Iceberg, 0, /*displayQty=*/10);
    book.addOrder(3, 3, Side::Sell, 9900, 60, OrderType::Limit);   // 60 of #1 fills
    book.addOrder(4, 4, Side::Buy, 10100, 25, OrderType::IOC);     // 25 of the iceberg
    book.cancelReplace(1, 9800, 150);                              // total 150, 90 left
    book.modifyOrder(2, 50);                                       // leaves 75 -> 50
    book.addOrder(5, 5, Side::Buy, 9000, 10, OrderType::Stop, 11000);  // parked
    expect(valid(book), "a healthy book (partial fills, replace, modify, iceberg, stop) passes");
}

void zeroQuantityRestingOrder() {
    OrderBook book(1);
    book.addOrder(1, 1, Side::Sell, 10100, 10, OrderType::Limit);
    mutate(book, 1).remainingQty = 0;
    mutate(book, 1).cumQty = 10;      // keep the accounting balanced: only zero is wrong
    expect(!valid(book), "an order resting with remainingQty == 0 is refused");
}

void icebergShowsMoreThanItHolds() {
    OrderBook book(1);
    book.addOrder(1, 1, Side::Sell, 10100, 100, OrderType::Iceberg, 0, /*displayQty=*/10);
    mutate(book, 1).remainingQty = 5;  // slice stays 10: the MATCH-3 state
    mutate(book, 1).initialQty = 5;
    expect(!valid(book), "an iceberg with visibleQty > remainingQty is refused");
}

void fillsAndLeavesMustAddUp() {
    OrderBook book(1);
    book.addOrder(1, 1, Side::Sell, 10100, 100, OrderType::Limit);
    mutate(book, 1).remainingQty = 160;  // the MATCH-4 overfill: leaves past the size
    expect(!valid(book), "a resting order with cumQty + remainingQty != initialQty is refused");

    OrderBook parked(1);
    parked.addOrder(1, 1, Side::Buy, 9000, 10, OrderType::Stop, 11000);
    mutate(parked, 1).cumQty = 3;
    expect(!valid(parked), "a parked order whose accounting does not add up is refused");
}

}  // namespace

int main() {
    healthyBookPasses();
    zeroQuantityRestingOrder();
    icebergShowsMoreThanItHolds();
    fillsAndLeavesMustAddUp();
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
