// EventOrderTest — the event-order rule (docs/MatchingRules.md Rule 2.5,
// audit GTDIFF-7): a fill's Trade reaches the listener before either side's
// OrderUpdate, so no OrderUpdate's filledQty ever exceeds the trades already
// delivered for that order.
//
// WHAT WAS WRONG. match() buffers its trades and dispatches them after the
// sweep (P1-3), but it notified a fully filled maker's Filled inline, so the
// maker's owner heard "Filled" before the trade that filled it. Every path
// that runs match() inherited it: a plain sweep, an iceberg's last slice, an
// elected stop or MIT, a replace that trades, a sweep cut short by STP or the
// market-protection collar. Pro-rata and the auction uncross dispatch each
// trade inline and were already in order; they are here so they stay that way.
//
// The listener below checks every OrderUpdate as it arrives, which the
// differential fuzzers' end-of-operation tally could not see.

#include "OrderBook.h"

#include <cstdio>
#include <map>
#include <string>

using namespace OrderMatcher;

namespace {

int failures = 0;

void expect(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

// Holds every OrderUpdate to the trades delivered before it.
struct SpecListener : EventListener {
    std::map<OrderId, Quantity> traded;   // trades delivered so far, per order
    std::map<OrderId, OrderStatus> last;  // last status seen, per order
    std::string violation;                // every breach, empty if none

    void onTrade(const Trade& t) override {
        traded[t.buyOrderId] += t.quantity;
        traded[t.sellOrderId] += t.quantity;
    }
    void onOrderUpdate(const OrderUpdate& u) override {
        last[u.orderId] = u.status;
        const Quantity seen = traded[u.orderId];
        if (u.filledQty <= seen) return;
        if (!violation.empty()) violation += "; ";
        violation += "order " + std::to_string(u.orderId) + " status " +
                     std::to_string(static_cast<int>(u.status)) + " filledQty " +
                     std::to_string(u.filledQty) + " before its trades (" +
                     std::to_string(seen) + " delivered)";
    }
};

// One scenario: the listener saw no breach, and `maker` really was filled
// (so the scenario exercised the path it names).
void check(const char* path, const SpecListener& l, OrderId maker) {
    auto it = l.last.find(maker);
    const bool filled = it != l.last.end() && it->second == OrderStatus::Filled;
    expect(filled, std::string(path) + ": maker " + std::to_string(maker) + " reported Filled");
    expect(l.violation.empty(), std::string(path) + ": trade precedes every OrderUpdate" +
                                    (l.violation.empty() ? "" : " — " + l.violation));
}

struct Fixture {
    OrderBook book;
    SpecListener spec;
    explicit Fixture(MatchAlgorithm algo = MatchAlgorithm::PriceTime) : book(1, algo) {
        book.setCircuitBreakerThreshold(1e9);
        book.setEventListener(&spec);
    }
};

void priceTimeOneFill() {
    Fixture f;
    f.book.addOrder(1, 1, Side::Sell, 100, 10, OrderType::Limit);
    f.book.addOrder(2, 2, Side::Buy, 100, 10, OrderType::Limit);
    check("price-time, one fill", f.spec, 1);
}

void priceTimeSweep() {
    Fixture f;
    f.book.addOrder(1, 1, Side::Sell, 100, 5, OrderType::Limit);
    f.book.addOrder(2, 3, Side::Sell, 101, 5, OrderType::Limit);
    f.book.addOrder(3, 4, Side::Sell, 102, 5, OrderType::Limit);
    f.book.addOrder(4, 2, Side::Buy, 102, 12, OrderType::Limit);
    check("price-time, three-level sweep", f.spec, 2);
}

void priceTimeIceberg() {
    Fixture f;
    f.book.addOrder(1, 1, Side::Sell, 100, 30, OrderType::Iceberg, 0, /*display=*/10);
    f.book.addOrder(2, 2, Side::Buy, 100, 30, OrderType::Limit);
    check("price-time, iceberg's last slice", f.spec, 1);
}

void proRata() {
    Fixture f(MatchAlgorithm::ProRata);
    f.book.addOrder(1, 1, Side::Sell, 100, 10, OrderType::Limit);
    f.book.addOrder(2, 3, Side::Sell, 100, 30, OrderType::Limit);
    f.book.addOrder(3, 2, Side::Buy, 100, 40, OrderType::Limit);
    check("pro-rata", f.spec, 1);
}

void proRataIceberg() {
    Fixture f(MatchAlgorithm::ProRata);
    f.book.addOrder(1, 1, Side::Sell, 100, 20, OrderType::Iceberg, 0, /*display=*/10);
    f.book.addOrder(2, 2, Side::Buy, 100, 20, OrderType::Limit);
    check("pro-rata, iceberg", f.spec, 1);
}

// A print at 100 elects a sell stop, which sweeps the resting buy at 99.
void stopElection() {
    Fixture f;
    f.book.addOrder(1, 4, Side::Buy, 99, 5, OrderType::Limit);
    f.book.addOrder(2, 3, Side::Sell, 0, 5, OrderType::Stop, /*stop=*/100);
    f.book.addOrder(3, 1, Side::Sell, 100, 1, OrderType::Limit);
    f.book.addOrder(4, 2, Side::Buy, 100, 1, OrderType::Limit);
    check("elected stop", f.spec, 1);
}

// A print at 100 touches a buy MIT, which sweeps the resting sell at 101.
void mitElection() {
    Fixture f;
    f.book.addOrder(1, 4, Side::Sell, 101, 5, OrderType::Limit);
    f.book.addOrder(2, 3, Side::Buy, 0, 5, OrderType::MIT, /*stop=*/100);
    f.book.addOrder(3, 1, Side::Sell, 100, 1, OrderType::Limit);
    f.book.addOrder(4, 2, Side::Buy, 100, 1, OrderType::Limit);
    check("elected MIT", f.spec, 1);
}

void replaceThatTrades() {
    Fixture f;
    f.book.addOrder(1, 1, Side::Sell, 101, 5, OrderType::Limit);
    f.book.addOrder(2, 2, Side::Buy, 99, 5, OrderType::Limit);
    f.book.cancelReplace(2, 101, 5);
    check("replace that trades", f.spec, 1);
}

void auctionUncross() {
    Fixture f;
    f.book.setTradingState(TradingState::PreOpen);
    f.book.addOrder(1, 1, Side::Sell, 100, 10, OrderType::Limit);
    f.book.addOrder(2, 2, Side::Buy, 100, 10, OrderType::Limit);
    f.book.setTradingState(TradingState::Continuous);  // the opening cross
    check("auction uncross", f.spec, 1);
}

// The buy fills participant 1's sell, then meets its own sell and STP cuts it.
void stpCut() {
    Fixture f;
    f.book.addOrder(1, 1, Side::Sell, 100, 5, OrderType::Limit);
    f.book.addOrder(2, 2, Side::Sell, 101, 5, OrderType::Limit);
    f.book.addOrder(3, 2, Side::Buy, 101, 10, OrderType::Limit);
    check("STP cut", f.spec, 1);
    expect(f.spec.last[3] == OrderStatus::CancelledBySTP, "STP cut: the buy ends CancelledBySTP");
}

// A 5% collar off the 100 touch stops the market buy before the ask at 110.
void marketCollar() {
    Fixture f;
    f.book.setMarketProtectionPct(0.05);
    f.book.addOrder(1, 1, Side::Sell, 100, 10, OrderType::Limit);
    f.book.addOrder(2, 3, Side::Sell, 104, 10, OrderType::Limit);
    f.book.addOrder(3, 4, Side::Sell, 110, 10, OrderType::Limit);
    f.book.addOrder(4, 2, Side::Buy, 0, 30, OrderType::Market);
    check("market-protection collar", f.spec, 2);
    expect(f.spec.last[4] == OrderStatus::Cancelled, "collar: the market remainder is cancelled");
}

}  // namespace

int main() {
    priceTimeOneFill();
    priceTimeSweep();
    priceTimeIceberg();
    proRata();
    proRataIceberg();
    stopElection();
    mitElection();
    replaceThatTrades();
    auctionUncross();
    stpCut();
    marketCollar();
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
