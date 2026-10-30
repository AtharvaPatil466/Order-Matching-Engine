// OrderTypeAdmissionTest — order shapes that corrupt the book are refused at
// admission (roadmap 0.12). Each is a stop-gap until the real semantics land.
// (Stop with price <= 0, OTM-4, was the first: 1.6-F2 made Stop a stop-market,
// pinned by StopMarketTest.)
//
// WHAT WAS WRONG.
//
//   Market with minQty > 0 (OTM-2). screenMinQty treats the market order's
//   price (0) as a limit, so a buy sees no liquidity at any price, fails the
//   minimum, does not "cross" at 0, and is rested — a MARKET order sitting in
//   the book as a bid at 0, with deep asks it never traded against. Fix in
//   1.6-F2 (Market+minQty behaves as IOC+minQty).
//
//   Pegged (STP-13). A peg rests at its computed price without matching, and
//   reprices the same way. An offset through the touch left bid > ask at once.
//   1.6-F4 lifted the blanket reject: only a peg priced at or through the
//   opposite touch is refused now (tests/PegOrderTest.cpp has the rest).
//
// WHAT IS PINNED. Each is rejected with a reason, and the book is left as it
// was: no resting market order, no crossed book. Positive
// controls confirm the ordinary forms still work.

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

void marketMinQtyIsRejected() {
    OrderBook book(1);
    relaxed(book);
    book.addOrder(1, 1, Side::Sell, 10000, 1000, OrderType::Limit);

    const auto r = book.addOrder(2, 2, Side::Buy, 0, 100, OrderType::Market,
                                 /*stopPrice=*/0, /*displayQty=*/0, TimeInForce::GTC,
                                 /*expiryTime=*/0, /*stopLimitPrice=*/0, PegType::None,
                                 /*pegOffset=*/0, /*trailAmount=*/0, /*minQty=*/1);
    assert(isReject(r, RejectReason::InvalidQuantity) &&
           "a Market order with minQty was admitted; it rests in the book as a bid at 0");
    assert(book.getOrder(2) == nullptr);
    assert(book.getBestBid() == 0 && book.getBidLevelsCount() == 0);

    // Control: without minQty the same market order trades.
    const uint64_t before = book.getTradeCount();
    assert(std::holds_alternative<OrderId>(
        book.addOrder(3, 2, Side::Buy, 0, 100, OrderType::Market)));
    assert(book.getTradeCount() == before + 1);
    std::puts("  Market: minQty > 0 rejected (InvalidQuantity), plain market trades");
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
    marketMinQtyIsRejected();
    peggedIsRejected();
    std::puts("OrderTypeAdmissionTest passed");
    return 0;
}
