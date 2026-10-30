// StopSweepOrderTest — the stop, trailing-stop and peg sweeps run after the
// incoming order is final (roadmap 1.6-F3, MATCH-5).
//
// WHAT WAS WRONG. addOrder ran the sweeps between match() and
// finalizeRemainingQty(), so a stop elected by the incoming order's own print
// matched against a book that did not yet hold that order's remainder. The
// stop missed the best contra price and rested; the remainder then rested
// through it, and nothing ever matched two resting orders: a crossed book in
// Continuous, which validateIntegrity passed. cancelReplace ran no sweep at
// all, so a replace that printed through a stop's trigger never elected it.

#include "OrderBook.h"

#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace OrderMatcher;

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

struct Tape : EventListener {
    std::vector<Trade> trades;
    void onTrade(const Trade& t) override { trades.push_back(t); }
    void onOrderUpdate(const OrderUpdate&) override {}
};

bool notCrossed(const OrderBook& b) {
    const Price bid = b.getBestBid(), ask = b.getBestAsk();
    const bool ok = bid == 0 || ask == std::numeric_limits<Price>::max() || bid < ask;
    if (!ok) std::printf("     bid %lld >= ask %lld\n", (long long)bid, (long long)ask);
    return ok;
}

bool valid(const OrderBook& b) {
    std::string err;
    const bool ok = b.validateIntegrity(&err);
    if (!ok) std::printf("     validateIntegrity: %s\n", err.c_str());
    return ok;
}

// The audit's W/match/h1.cpp S1: the buy's print elects a sell stop-limit at
// 99, and the buy's own remainder at 101 is the best bid it should hit.
void stopLimitElectedByIncomingPrint() {
    OrderBook b(1);
    b.setCircuitBreakerThreshold(1e9);
    Tape tape;
    b.setEventListener(&tape);
    b.addOrder(1, 1, Side::Sell, 100, 5, OrderType::Limit);
    b.addOrder(2, 3, Side::Sell, 0, 10, OrderType::StopLimit, /*stop=*/100, 0,
               TimeInForce::GTC, 0, /*stopLimitPrice=*/99);
    b.addOrder(3, 2, Side::Buy, 101, 10, OrderType::Limit);
    expect(notCrossed(b), "S1: a stop-limit elected by the incoming print leaves the book uncrossed");
    expect(valid(b), "S1: validateIntegrity agrees");
    bool stopHitRemainder = false;
    for (const Trade& t : tape.trades)
        if (t.sellOrderId == 2 && t.buyOrderId == 3 && t.price == 101 && t.quantity == 5)
            stopHitRemainder = true;
    expect(stopHitRemainder, "S1: the elected stop trades 5 @ 101 against the buy's remainder");
}

// The ItchReconstructionTest case behind the held crossed-book check: an
// elected trailing stop becomes a limit at its stop price and must meet the
// incoming sell that elected it, not rest opposite it at the same price.
void trailingStopElectedByIncomingPrint() {
    OrderBook b(1);
    b.setCircuitBreakerThreshold(1e9);
    Tape tape;
    b.setEventListener(&tape);
    b.addOrder(1, 1, Side::Sell, 995, 1, OrderType::Limit);
    b.addOrder(2, 2, Side::Buy, 995, 1, OrderType::Limit);          // last = 995
    b.addOrder(3, 3, Side::Buy, 0, 10, OrderType::TrailingStop, 0, 0,
               TimeInForce::GTC, 0, 0, PegType::None, 0, /*trailAmount=*/5);  // stop 1000
    b.addOrder(4, 5, Side::Buy, 1000, 2, OrderType::Limit);
    b.addOrder(5, 4, Side::Sell, 1000, 10, OrderType::Limit);       // prints 2 @ 1000
    bool stopHitRemainder = false;
    for (const Trade& t : tape.trades)
        if (t.buyOrderId == 3 && t.sellOrderId == 5 && t.price == 1000 && t.quantity == 8)
            stopHitRemainder = true;
    expect(stopHitRemainder, "trailing: the elected stop trades 8 @ 1000 against the sell's remainder");
    expect(notCrossed(b), "trailing: the book is neither crossed nor locked");
    expect(valid(b), "trailing: validateIntegrity agrees");
}

// A replace that prints through a stop's trigger elects it, as an add does.
void replacePrintElectsStop() {
    OrderBook b(1);
    b.setCircuitBreakerThreshold(1e9);
    Tape tape;
    b.setEventListener(&tape);
    b.addOrder(1, 1, Side::Sell, 100, 5, OrderType::Limit);
    b.addOrder(2, 3, Side::Buy, 0, 1, OrderType::StopLimit, /*stop=*/100, 0,
               TimeInForce::GTC, 0, /*stopLimitPrice=*/100);
    b.addOrder(3, 2, Side::Buy, 90, 1, OrderType::Limit);
    const bool replaced = b.cancelReplace(3, 100, 1);
    expect(replaced && tape.trades.size() == 2 && b.getOrder(2) == nullptr,
           "replace: a replace printing 1 @ 100 elects the buy stop at 100, which fills");
    expect(valid(b), "replace: validateIntegrity agrees");
}

}  // namespace

int main() {
    stopLimitElectedByIncomingPrint();
    trailingStopElectedByIncomingPrint();
    replacePrintElectsStop();
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
