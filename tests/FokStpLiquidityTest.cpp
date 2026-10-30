// FokStpLiquidityTest — the FOK and minQty pre-checks see the fill the sweep
// will make, self-trade prevention included (roadmap 1.6-F5, GTDIFF-2 and
// OTM-11).
//
// WHAT WAS WRONG. checkLiquidity (FOK) and checkMinQty added up every opposite
// order at a crossing price, the incoming participant's own included. The
// sweep then hit self-trade prevention and stopped, so a FOK filled part of
// its size and an IOC filled below its minQty. checkMinQty also counted an
// iceberg's slice where the sweep takes its whole reserve, and ran in auction
// states, cancelling a minQty order that should have joined the auction.
//
// WHAT IS PINNED. Each pre-check runs the sweep's own STP decision at each of
// the participant's resting orders: the default and CancelIncoming/CancelBoth
// end the sweep there, CancelResting passes it, DecreaseAndCancel shrinks the
// order without a fill. An iceberg ahead of a sweep-ending order counts only
// its slice; otherwise its reserve. The pro-rata pre-pass ends a level before
// it allocates. Cases A-C and E are the audit's W/gtdiff/rep/h.cpp and
// W/gtdiff/stpq/h.cpp.

#include "OrderBook.h"

#include <cstdio>
#include <variant>
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
    Quantity filled() const {
        Quantity q = 0;
        for (const Trade& t : trades) q += t.quantity;
        return q;
    }
};

struct Book {
    OrderBook b;
    Tape tape;
    explicit Book(MatchAlgorithm algo = MatchAlgorithm::PriceTime) : b(1, algo) {
        b.setCircuitBreakerThreshold(1e9);
        b.setEventListener(&tape);
    }
    void sell(OrderId id, ParticipantId p, Price px, Quantity q) {
        b.addOrder(id, p, Side::Sell, px, q, OrderType::Limit);
    }
    AddOrderResult buy(OrderId id, ParticipantId p, Price px, Quantity q, OrderType type,
                       Quantity minQty = 0) {
        tape.trades.clear();
        return b.addOrder(id, p, Side::Buy, px, q, type, 0, 0, TimeInForce::GTC, 0, 0,
                          PegType::None, 0, 0, minQty);
    }
};

bool fokRejected(const AddOrderResult& r) {
    return std::holds_alternative<RejectReason>(r) &&
           std::get<RejectReason>(r) == RejectReason::FOKInsufficientLiquidity;
}

constexpr Price P = 1000000;

void fokStopsAtOwnOrder() {
    Book k;
    k.sell(38, 3, P + 700, 1);   // own, behind
    k.sell(35, 4, P + 600, 9);
    const auto r = k.buy(47, 3, P + 700, 10, OrderType::FOK);
    expect(fokRejected(r) && k.tape.trades.empty(),
           "A: FOK 10 with 9 ahead of its own order is rejected, not filled 9");
}

void iocMinQtyStopsAtOwnOrder() {
    Book k;
    k.sell(1, 2, P, 5);
    k.sell(2, 1, P, 50);         // own
    k.buy(3, 1, P, 55, OrderType::IOC, /*minQty=*/20);
    expect(k.tape.trades.empty(), "B: IOC minQty 20 with 5 ahead of its own order trades nothing");
}

void fokUnderCancelResting() {
    Book k;
    k.b.setSTPMode(1, STPMode::CancelResting);
    k.sell(1, 2, P, 5);
    k.sell(2, 1, P, 50);         // own: the sweep would cancel it and go on
    const auto r = k.buy(3, 1, P, 10, OrderType::FOK);
    expect(fokRejected(r) && k.b.getOrder(2) && k.b.getOrder(2)->remainingQty == 50,
           "C: CancelResting FOK 10 with only 5 past its own order is rejected, book untouched");

    Book c;
    c.b.setSTPMode(1, STPMode::CancelResting);
    c.sell(1, 2, P, 5);
    c.sell(2, 1, P, 50);
    c.sell(4, 2, P, 10);
    const auto ok = c.buy(3, 1, P, 10, OrderType::FOK);
    expect(std::holds_alternative<OrderId>(ok) && c.tape.filled() == 10 && !c.b.getOrder(2),
           "C: control, 15 past its own order: the FOK fills 10 and cancels its own resting order");
}

void fokUnderDecreaseAndCancel() {
    Book k;
    k.b.setSTPMode(1, STPMode::DecreaseAndCancel);
    k.sell(1, 2, P, 5);
    k.sell(2, 1, P, 50);         // own: the sweep would decrement the FOK by it
    const auto r = k.buy(3, 1, P, 10, OrderType::FOK);
    expect(fokRejected(r) && k.tape.trades.empty(),
           "D: DecreaseAndCancel FOK 10 that would fill 5 and lose 5 to a decrement is rejected");
}

void minQtyCountsIcebergReserve() {
    Book k;
    k.b.addOrder(1, 2, Side::Sell, P, 100, OrderType::Iceberg, 0, /*displayQty=*/5);
    k.buy(3, 1, P, 50, OrderType::IOC, /*minQty=*/10);
    expect(k.tape.filled() == 50, "E: IOC 50 minQty 10 against an iceberg 100 showing 5 fills 50");
}

void icebergAheadOfOwnOrderCountsItsSlice() {
    Book k;
    k.b.addOrder(1, 2, Side::Sell, P, 100, OrderType::Iceberg, 0, /*displayQty=*/5);
    k.sell(2, 1, P, 10);         // own, behind the iceberg in the same level
    const auto r = k.buy(3, 1, P, 50, OrderType::FOK);
    expect(fokRejected(r) && k.tape.trades.empty(),
           "F: FOK 50 is rejected: the iceberg's refreshed slice rejoins behind its own order");

    Book m;
    m.b.addOrder(1, 2, Side::Sell, P, 100, OrderType::Iceberg, 0, /*displayQty=*/5);
    m.sell(2, 1, P, 10);
    m.buy(4, 1, P, 50, OrderType::IOC, /*minQty=*/6);
    expect(m.tape.trades.empty(), "F: IOC minQty 6 there trades nothing either (5 fillable)");
}

void proRataLevelEndsAtOwnOrder() {
    Book k(MatchAlgorithm::ProRata);
    k.sell(1, 2, P, 5);
    k.sell(2, 2, P + 100, 5);
    k.sell(3, 1, P + 100, 50);   // own, in the second level
    const auto r = k.buy(4, 1, P + 100, 10, OrderType::FOK);
    expect(fokRejected(r) && k.tape.trades.empty(),
           "G: pro-rata FOK 10: the STP pre-pass ends the second level, so only 5 is fillable");
}

void minQtyJoinsTheAuction() {
    OrderBook b(1);
    b.setTradingState(TradingState::PreOpen);
    b.addOrder(1, 1, Side::Sell, 1000, 10, OrderType::Limit);
    b.addOrder(2, 2, Side::Buy, 1000, 20, OrderType::Limit, 0, 0, TimeInForce::GTC, 0, 0,
               PegType::None, 0, 0, /*minQty=*/15);
    expect(b.getOrder(2) != nullptr, "H: in PreOpen a crossing minQty limit rests for the auction");
}

}  // namespace

int main() {
    fokStopsAtOwnOrder();
    iocMinQtyStopsAtOwnOrder();
    fokUnderCancelResting();
    fokUnderDecreaseAndCancel();
    minQtyCountsIcebergReserve();
    icebergAheadOfOwnOrderCountsItsSlice();
    proRataLevelEndsAtOwnOrder();
    minQtyJoinsTheAuction();
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
