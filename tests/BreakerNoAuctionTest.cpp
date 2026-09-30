// BreakerNoAuctionTest — an out-of-band order is rejected; the MARKET is not.
//
// WHAT WAS WRONG (roadmap 0.14, findings PRIOR-5 / AUCT-8 trigger). The
// volatility breaker tested each incoming priced order's LIMIT price against the
// reference and, on a breach, moved the whole book into VolatilityAuction. A
// limit price is not a print: a passive bid 50% below the market can never
// execute, yet it switched every participant on the symbol from continuous
// trading to an accumulate-only auction. Nothing in the shipped binary calls
// resumeVolatilityAuctions, so the symbol stayed there until restart. One
// participant, one order, a whole symbol stopped.
//
// WHAT IS PINNED. The offending order is still refused with
// VolatilityCircuitBreaker, and nothing else changes: the book stays
// Continuous, a later in-band order still trades on arrival, and IOC is still
// admitted. A breaker that trips on a would-be execution price and reopens on a
// timer is roadmap 1.8-H3; until then the breaker is a per-order reject.

#include "OrderBook.h"

#include <cassert>
#include <cstdio>
#include <variant>

using namespace OrderMatcher;

namespace {

constexpr Price kRef = 10000;

void farPassiveOrderDoesNotStopTheMarket() {
    OrderBook book(1);
    book.setCircuitBreakerThreshold(0.05);

    // Reference 10000 (first priced order), and an ask to trade against.
    assert(std::holds_alternative<OrderId>(
        book.addOrder(1, 1, Side::Buy, kRef, 100, OrderType::Limit)));
    assert(std::holds_alternative<OrderId>(
        book.addOrder(2, 2, Side::Sell, kRef + 10, 100, OrderType::Limit)));

    // A passive bid 50% below the reference. It can never execute — the ask is
    // at 10010 — but it is outside the breaker threshold.
    const auto far = book.addOrder(3, 3, Side::Buy, kRef / 2, 100, OrderType::Limit);
    assert(std::holds_alternative<RejectReason>(far) &&
           std::get<RejectReason>(far) == RejectReason::VolatilityCircuitBreaker &&
           "the out-of-band order itself is still refused");

    assert(book.getTradingState() == TradingState::Continuous &&
           "one far-away passive order moved the whole symbol into a volatility "
           "auction — every other participant lost continuous trading");

    // Continuous trading carries on: an in-band IOC is admitted and trades now.
    const uint64_t tradesBefore = book.getTradeCount();
    const auto ioc = book.addOrder(4, 4, Side::Buy, kRef + 10, 40, OrderType::IOC);
    assert(std::holds_alternative<OrderId>(ioc) &&
           "IOC was refused after an unrelated order breached the breaker");
    assert(book.getTradeCount() == tradesBefore + 1 &&
           "an in-band crossing order did not trade on arrival");

    std::puts("  far passive order rejected, book still Continuous, IOC trades");
}

}  // namespace

int main() {
    farPassiveOrderDoesNotStopTheMarket();
    std::puts("BreakerNoAuctionTest passed");
    return 0;
}
