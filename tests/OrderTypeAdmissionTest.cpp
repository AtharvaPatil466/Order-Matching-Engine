// OrderTypeAdmissionTest — order shapes that corrupt the book are refused at
// admission (roadmap 0.12). Each is a stop-gap until the real semantics land.
// (Stop with price <= 0, OTM-4, and Market with minQty > 0, OTM-2, were the
// first two: 1.6-F2 made Stop a stop-market and Market+minQty an IOC+minQty,
// pinned by StopMarketTest and MarketMinQtyTest.)
//
// WHAT WAS WRONG.
//
//   Pegged (STP-13). A peg rests at its computed price without matching, and
//   reprices the same way. An offset through the touch left bid > ask at once.
//   1.6-F4 lifted the blanket reject: only a peg priced at or through the
//   opposite touch is refused now (tests/PegOrderTest.cpp has the rest).
//
// WHAT IS PINNED. Each is rejected with a reason, and the book is left as it
// was: no crossed book. Positive controls confirm the ordinary forms still
// work.

#include "OrderBook.h"

#include <cassert>
#include <cstdio>
#include <variant>

using namespace OrderMatcher;

namespace {

bool isReject(const AddOrderResult& r, RejectReason want) {
    return std::holds_alternative<RejectReason>(r) && std::get<RejectReason>(r) == want;
}

OrderBook& relaxed(OrderBook& b) {
    b.setCircuitBreakerThreshold(1e9);  // orthogonal to what is under test
    return b;
}

void peggedIsRejected() {
    OrderBook book(1);
    relaxed(book);
    book.addOrder(1, 1, Side::Buy, 10000, 10, OrderType::Limit);
    book.addOrder(2, 2, Side::Sell, 10200, 10, OrderType::Limit);

    // PrimaryPeg buy, offset +500: best bid + 500 = 10500, through the 10200 ask.
    const auto through = book.addOrder(3, 3, Side::Buy, 10000, 10, OrderType::Pegged,
                                       0, 0, TimeInForce::GTC, 0, 0,
                                       PegType::PrimaryPeg, 500);
    assert(isReject(through, RejectReason::InvalidPrice) &&
           "a Pegged order priced through the touch was admitted");

    // Control: MidPeg, offset 0, rests at the mid.
    assert(std::holds_alternative<OrderId>(
        book.addOrder(4, 3, Side::Buy, 10000, 10, OrderType::Pegged, 0, 0,
                      TimeInForce::GTC, 0, 0, PegType::MidPeg, 0)));
    assert(book.getOrder(4) && book.getOrder(4)->price == 10100);

    assert(book.getBestBid() == 10100 && book.getBestAsk() == 10200 &&
           "the book must not be crossed");
    std::puts("  Pegged: through the touch rejected (InvalidPrice), a MidPeg rests at the mid");
}

}  // namespace

int main() {
    peggedIsRejected();
    std::puts("OrderTypeAdmissionTest passed");
    return 0;
}
