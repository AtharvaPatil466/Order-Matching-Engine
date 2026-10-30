// FokStpLiquidityTest — the minQty pre-check sees the fill the sweep will make
// (roadmap 1.6-F5, OTM-11).
//
// WHAT WAS WRONG. checkMinQty counted an iceberg's slice where the sweep takes
// its whole reserve, so an IOC with a minQty the iceberg could meet was
// cancelled unfilled; and screenMinQty ran in auction states, cancelling a
// crossing minQty order that should have joined the auction. Case E is the
// audit's W/gtdiff/stpq/h.cpp C, case H its h1 S21.

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

constexpr Price P = 1000000;

void minQtyCountsIcebergReserve() {
    Book k;
    k.b.addOrder(1, 2, Side::Sell, P, 100, OrderType::Iceberg, 0, /*displayQty=*/5);
    k.buy(3, 1, P, 50, OrderType::IOC, /*minQty=*/10);
    expect(k.tape.filled() == 50, "E: IOC 50 minQty 10 against an iceberg 100 showing 5 fills 50");
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
    minQtyCountsIcebergReserve();
    minQtyJoinsTheAuction();
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
