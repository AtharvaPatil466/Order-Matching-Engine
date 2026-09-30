// OrderTypeAdmissionTest — three order shapes that corrupt the book are refused
// at admission (roadmap 0.12). Each is a stop-gap until the real semantics land.
//
// WHAT WAS WRONG.
//
//   Stop, price <= 0 (OTM-4). validateOrderRequest exempted Stop from the
//   price > 0 check, but an elected Stop becomes a Limit at `price`. A sell
//   Stop at price 0 swept every bid down to zero and rested its remainder as
//   an ask at 0 — a price admission refuses for any Limit. Fix in 1.6-F2
//   (Stop becomes stop-market); until then a Stop needs a positive limit.
//
//   Market with minQty > 0 (OTM-2). screenMinQty treats the market order's
//   price (0) as a limit, so a buy sees no liquidity at any price, fails the
//   minimum, does not "cross" at 0, and is rested — a MARKET order sitting in
//   the book as a bid at 0, with deep asks it never traded against. Fix in
//   1.6-F2 (Market+minQty behaves as IOC+minQty).
//
//   Pegged (STP-13). A peg rests at its computed price without matching, and
//   reprices the same way. An offset through the touch leaves bid > ask at
//   once; a PrimaryPeg counts itself as the best bid, so offset +1 ratchets it
//   up a tick on every later order, straight through the ask. Fix in 1.6-F4.
//
// WHAT IS PINNED. Each is rejected with a reason, and the book is left as it
// was: no zero-priced rest, no resting market order, no crossed book. Positive
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

void stopNeedsAPositivePrice() {
    OrderBook book(1);
    relaxed(book);
    book.addOrder(1, 1, Side::Buy, 9900, 1, OrderType::Limit);
    book.addOrder(2, 1, Side::Buy, 5000, 10, OrderType::Limit);

    // Sell Stop, trigger 9900, price 0.
    assert(isReject(book.addOrder(3, 2, Side::Sell, 0, 50, OrderType::Stop, 9900),
                    RejectReason::InvalidPrice) &&
           "a Stop with price 0 was admitted; elected, it rests as a Limit at 0");
    assert(isReject(book.addOrder(4, 2, Side::Sell, -1, 50, OrderType::Stop, 9900),
                    RejectReason::InvalidPrice));

    // Print 9900. Had the stop been admitted it would now sell through the
    // 5000 bid and rest 40 as an ask at 0.
    book.addOrder(5, 3, Side::Sell, 9900, 1, OrderType::Limit);
    assert(book.getBestBid() == 5000 && "the 5000 bid must be untouched");
    assert(book.getBestAsk() > 0 && "an ask rests at price 0");

    // Control: a Stop with a positive price is still admitted.
    assert(std::holds_alternative<OrderId>(
        book.addOrder(6, 2, Side::Sell, 4000, 5, OrderType::Stop, 4500)));
    std::puts("  Stop: price <= 0 rejected (InvalidPrice), positive price admitted");
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
    assert(isReject(through, RejectReason::OrderTypeNotAllowedInState) &&
           "a Pegged order was admitted; it rests through the touch without trading");

    // MidPeg, offset 0 — the benign-looking form — is refused too.
    assert(isReject(book.addOrder(4, 3, Side::Buy, 10000, 10, OrderType::Pegged, 0, 0,
                                  TimeInForce::GTC, 0, 0, PegType::MidPeg, 0),
                    RejectReason::OrderTypeNotAllowedInState));

    assert(book.getBestBid() == 10000 && book.getBestAsk() == 10200 &&
           "the book must be exactly as it was, and not crossed");
    std::puts("  Pegged: rejected (OrderTypeNotAllowedInState), book not crossed");
}

}  // namespace

int main() {
    stopNeedsAPositivePrice();
    marketMinQtyIsRejected();
    peggedIsRejected();
    std::puts("OrderTypeAdmissionTest passed");
    return 0;
}
