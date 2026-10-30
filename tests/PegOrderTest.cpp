// PegOrderTest — a pegged order never rests at or through the opposite touch,
// and its reference is the book without pegs (roadmap 1.6-F4, STP-13).
//
// WHAT WAS WRONG. restPeggedOrder and updatePeggedOrders put a peg at
// reference + offset and called addToBook: no cross check, no match. A
// PrimaryPeg buy offset +30000 rested above the ask; a PrimaryPeg used the
// best bid, which after placement was itself, so every later order moved it
// one more offset; a MidPeg counted itself in the mid and drifted. Each left a
// crossed book that validateIntegrity passed, so roadmap 0.12 refused Pegged.
//
// WHAT IS PINNED. A peg whose price at entry is at or through the opposite
// touch is rejected (InvalidPrice). Its reference ignores pegged orders, its
// own included, so it does not ratchet. A reprice that would reach the
// opposite touch is clamped one unit behind it. A sell MidPeg on a one-unit
// spread rounds up, onto the ask, rather than down onto the bid.

#include "OrderBook.h"

#include <cstdio>
#include <limits>
#include <string>
#include <variant>

using namespace OrderMatcher;

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

bool sane(const OrderBook& b) {
    const Price bid = b.getBestBid(), ask = b.getBestAsk();
    if (bid != 0 && ask != std::numeric_limits<Price>::max() && bid >= ask) {
        std::printf("     bid %lld >= ask %lld\n", (long long)bid, (long long)ask);
        return false;
    }
    std::string err;
    if (!b.validateIntegrity(&err)) {
        std::printf("     validateIntegrity: %s\n", err.c_str());
        return false;
    }
    return true;
}

AddOrderResult peg(OrderBook& b, OrderId id, ParticipantId p, Side side, Price limit,
                   PegType type, Price offset) {
    return b.addOrder(id, p, side, limit, 10, OrderType::Pegged, 0, 0, TimeInForce::GTC,
                      0, 0, type, offset);
}

Price priceOf(const OrderBook& b, OrderId id) {
    const Order* o = b.getOrder(id);
    return o ? o->price : -1;
}

constexpr Price P = 1000000;

OrderBook& twoSided(OrderBook& b) {
    b.setCircuitBreakerThreshold(1e9);
    b.addOrder(1, 2, Side::Buy, P - 10000, 10, OrderType::Limit);
    b.addOrder(2, 3, Side::Sell, P + 10000, 10, OrderType::Limit);
    return b;
}

// The audit's W/stp/h4.cpp: bid 990000, ask 1010000, PrimaryPeg buy +30000.
void offsetThroughTheTouchIsRejected() {
    OrderBook b(1);
    twoSided(b);
    const auto r = peg(b, 3, 4, Side::Buy, P - 10000, PegType::PrimaryPeg, 30000);
    expect(std::holds_alternative<RejectReason>(r) &&
           std::get<RejectReason>(r) == RejectReason::InvalidPrice,
           "h4: a PrimaryPeg buy whose offset reaches through the ask is rejected (InvalidPrice)");
    b.addOrder(4, 5, Side::Buy, P - 5000, 1, OrderType::Limit);
    expect(sane(b), "h4: the book is not crossed and validateIntegrity agrees");
}

// The audit's W/match/h1.cpp S7a: PrimaryPeg buy +1 used to count itself as
// the best bid and climb one unit per unrelated order.
void primaryPegDoesNotRatchet() {
    OrderBook b(1);
    twoSided(b);
    const auto r = peg(b, 3, 4, Side::Buy, P - 10000, PegType::PrimaryPeg, 1);
    expect(std::holds_alternative<OrderId>(r) && priceOf(b, 3) == P - 9999,
           "S7a: a PrimaryPeg buy +1 is admitted one unit above the best bid");
    for (OrderId i = 0; i < 12; ++i)
        b.addOrder(100 + i, 5, Side::Sell, P + 20000 + static_cast<Price>(i), 1, OrderType::Limit);
    expect(priceOf(b, 3) == P - 9999, "S7a: twelve unrelated orders later it has not moved");
    b.addOrder(4, 5, Side::Buy, P - 5000, 1, OrderType::Limit);
    b.addOrder(5, 5, Side::Sell, P + 30000, 1, OrderType::Limit);
    expect(priceOf(b, 3) == P - 4999, "S7a: it follows a better non-peg bid, and only that");
    expect(sane(b), "S7a: the book is not crossed and validateIntegrity agrees");
}

// S7b: a MidPeg buy alone at the bid used to include itself in the mid.
void midPegDoesNotDrift() {
    OrderBook b(1);
    twoSided(b);
    peg(b, 3, 4, Side::Buy, P - 10000, PegType::MidPeg, 0);
    expect(priceOf(b, 3) == P, "S7b: a MidPeg buy rests at the mid of the other orders");
    for (OrderId i = 0; i < 10; ++i)
        b.addOrder(100 + i, 5, Side::Sell, P + 20000 + static_cast<Price>(i), 1, OrderType::Limit);
    expect(priceOf(b, 3) == P, "S7b: ten unrelated orders later it is still at that mid");
}

// The reference moves until reference + offset reaches the ask: clamped.
void repriceIsClampedBehindTheTouch() {
    OrderBook b(1);
    twoSided(b);
    peg(b, 3, 4, Side::Buy, P - 10000, PegType::PrimaryPeg, 15000);
    expect(priceOf(b, 3) == P + 5000, "clamp: a PrimaryPeg buy +15000 is admitted at 1005000");
    b.addOrder(4, 5, Side::Buy, P, 1, OrderType::Limit);  // reference 1000000 -> 1015000
    expect(priceOf(b, 3) == P + 9999, "clamp: the reprice stops one unit below the ask");
    expect(sane(b), "clamp: the book is not crossed and validateIntegrity agrees");
}

void sellMidPegRoundsAwayFromTheBid() {
    OrderBook b(1);
    b.setCircuitBreakerThreshold(1e9);
    b.addOrder(1, 2, Side::Buy, 100, 10, OrderType::Limit);
    b.addOrder(2, 3, Side::Sell, 101, 10, OrderType::Limit);
    const auto r = peg(b, 3, 4, Side::Sell, 101, PegType::MidPeg, 0);
    expect(std::holds_alternative<OrderId>(r) && priceOf(b, 3) == 101,
           "round: a sell MidPeg on a 100/101 market rests at 101, not on the bid");
    expect(sane(b), "round: the book is not crossed and validateIntegrity agrees");
}

}  // namespace

int main() {
    offsetThroughTheTouchIsRejected();
    primaryPegDoesNotRatchet();
    midPegDoesNotDrift();
    repriceIsClampedBehindTheTouch();
    sellMidPegRoundsAwayFromTheBid();
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
