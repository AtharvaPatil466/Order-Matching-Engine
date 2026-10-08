// VolatilityBreakerTest — the volatility breaker trips on a print, not a quote,
// and the auction it starts reopens on a timer.
//
// WHAT WAS WRONG (roadmap 1.8-H3, findings PRIOR-5 / AUCT-8). The breaker
// tested each order's LIMIT price. A passive bid far below the market can
// never execute, yet it switched the whole symbol into VolatilityAuction, and
// nothing in the shipped binary ever reopened it. The 0.14 stop-gap made the
// breach a per-order reject: no market stop, but a far passive order was
// refused for being far, and an order that would really print out of band
// was refused instead of being auctioned.
//
// WHAT IS PINNED.
//   1. A far passive order is admitted and rests; the book stays Continuous.
//   2. An order whose FIRST fill would print outside the threshold trips the
//      breaker: the book enters VolatilityAuction, nothing prints, the order
//      takes part in the auction (a limit rests, a market order is parked, an
//      IOC is cancelled). The breaker_trip event is still logged.
//   3. Only the first fill counts: a sweep that starts in band is not stopped.
//   4. The breaker never trips while the book is already in an auction state.
//   5. The auction reopens on the engine's own clock after the dwell (default
//      5 minutes), sync and async, with the reopening cross.
//   6. Replay reproduces a trip exactly, whatever the replaying engine's
//      breaker configuration: the trip is journaled.

#include "EventListener.h"
#include "MatchingEngine.h"
#include "OrderBook.h"
#include "StructuredLog.h"
#include "TempPath.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <variant>
#include <vector>

using namespace OrderMatcher;
namespace fs = std::filesystem;

namespace {

constexpr Price kRef = 10000;
constexpr SymbolId kSym = 0;
constexpr uint64_t kMinute = 60'000'000'000ULL;

struct Recorder : EventListener {
    std::map<OrderId, OrderUpdate> last;
    std::vector<Trade> trades;
    void onOrderUpdate(const OrderUpdate& u) override { last[u.orderId] = u; }
    void onTrade(const Trade& t) override { trades.push_back(t); }
};

bool accepted(const AddOrderResult& r) { return std::holds_alternative<OrderId>(r); }

// Reference 10000 (the first priced order), threshold 5%.
void seed(OrderBook& book) {
    book.setCircuitBreakerThreshold(0.05);
    assert(accepted(book.addOrder(1, 1, Side::Buy, kRef, 100, OrderType::Limit)));
}

void farPassiveOrderIsAdmitted() {
    OrderBook book(1);
    Recorder rec;
    book.setEventListener(&rec);
    seed(book);
    assert(accepted(book.addOrder(2, 2, Side::Sell, kRef + 10, 100, OrderType::Limit)));

    // 50% below the reference, but it cannot execute: the ask is at 10010.
    assert(accepted(book.addOrder(3, 3, Side::Buy, kRef / 2, 100, OrderType::Limit)) &&
           "a far passive order was refused although it could not print");
    assert(book.getOrder(3) != nullptr);
    assert(book.getTradingState() == TradingState::Continuous);

    assert(accepted(book.addOrder(4, 4, Side::Buy, kRef + 10, 40, OrderType::IOC)));
    assert(rec.trades.size() == 1 && "continuous trading stopped");
    std::puts("  a far passive order rests and the book stays Continuous");
}

void outOfBandFirstFillTrips() {
    CapturingSink cap;
    setObSink(&cap);
    OrderBook book(1);
    Recorder rec;
    book.setEventListener(&rec);
    seed(book);
    // A thin ask 10% up: passive, so admitted.
    assert(accepted(book.addOrder(2, 2, Side::Sell, kRef + kRef / 10, 100, OrderType::Limit)));
    assert(book.getTradingState() == TradingState::Continuous);

    // A buy that would lift it prints 10% away from the reference.
    assert(accepted(book.addOrder(3, 3, Side::Buy, kRef + kRef / 10, 100, OrderType::Limit)));
    assert(book.getTradingState() == TradingState::VolatilityAuction &&
           "an out-of-band print did not trip the breaker");
    assert(rec.trades.empty() && "the out-of-band trade printed");
    assert(book.getOrder(3) && book.getOrder(3)->remainingQty == 100 &&
           "the order that tripped the breaker must wait for the reopening cross");
    bool logged = false;
    for (const auto& e : cap.events)
        if (e.name == "breaker_trip") logged = true;
    assert(logged && "breaker_trip not logged");
    setObSink(nullptr);

    // Nothing matches in the auction.
    assert(accepted(book.addOrder(4, 4, Side::Sell, kRef, 10, OrderType::Limit)));
    assert(rec.trades.empty());
    std::puts("  an out-of-band first fill enters VolatilityAuction without printing");
}

void trippingMarketOrderIsParkedAndIocCancelled() {
    {
        OrderBook book(1);
        Recorder rec;
        book.setEventListener(&rec);
        seed(book);
        assert(accepted(book.addOrder(2, 2, Side::Sell, kRef * 2, 100, OrderType::Limit)));
        assert(accepted(book.addOrder(3, 3, Side::Buy, 0, 50, OrderType::Market)));
        assert(book.getTradingState() == TradingState::VolatilityAuction);
        assert(book.getOrder(3) != nullptr && "the market order must be parked for the reopen");
        assert(book.resumeVolatilityAuction());
        assert(rec.trades.size() == 1 && rec.trades[0].quantity == 50 &&
               "the parked market order did not take part in the reopening cross");
    }
    {
        OrderBook book(1);
        Recorder rec;
        book.setEventListener(&rec);
        seed(book);
        assert(accepted(book.addOrder(2, 2, Side::Sell, kRef * 2, 100, OrderType::Limit)));
        book.addOrder(3, 3, Side::Buy, kRef * 2, 50, OrderType::IOC);
        assert(book.getTradingState() == TradingState::VolatilityAuction);
        assert(book.getOrder(3) == nullptr);
        assert(rec.last[3].status == OrderStatus::Cancelled && rec.last[3].filledQty == 0);
    }
    std::puts("  a tripping market order is parked; a tripping IOC is cancelled");
}

void onlyTheFirstFillCounts() {
    OrderBook book(1);
    Recorder rec;
    book.setEventListener(&rec);
    seed(book);
    assert(accepted(book.addOrder(2, 2, Side::Sell, kRef + 100, 10, OrderType::Limit)));  // 1%
    assert(accepted(book.addOrder(3, 3, Side::Sell, kRef * 2, 10, OrderType::Limit)));    // 100%
    assert(accepted(book.addOrder(4, 4, Side::Buy, kRef * 2, 20, OrderType::Limit)));
    assert(book.getTradingState() == TradingState::Continuous);
    assert(rec.trades.size() == 2 && "a sweep whose first fill is in band was stopped");
    std::puts("  a sweep that starts in band is not stopped");
}

void neverTripsInAnAuction() {
    OrderBook book(1);
    Recorder rec;
    book.setEventListener(&rec);
    seed(book);
    book.setTradingState(TradingState::PreOpen);
    assert(accepted(book.addOrder(2, 2, Side::Sell, kRef * 2, 100, OrderType::Limit)));
    assert(accepted(book.addOrder(3, 3, Side::Buy, kRef * 2, 100, OrderType::Limit)));
    assert(book.getTradingState() == TradingState::PreOpen &&
           "the breaker tripped inside an auction");
    std::puts("  the breaker never trips in an auction state");
}

// A book that trips on order 3; `extra` runs while it is in the auction.
void tripThroughEngine(MatchingEngine& e) {
    e.getOrderBook(kSym)->setCircuitBreakerThreshold(0.05);
    e.submitOrder(kSym, 1, 1, Side::Buy, kRef, 100, OrderType::Limit);
    e.submitOrder(kSym, 2, 2, Side::Sell, kRef * 2, 100, OrderType::Limit);
    e.submitOrder(kSym, 3, 3, Side::Buy, kRef * 2, 60, OrderType::Limit);
    e.waitForDrain();
    assert(e.getOrderBook(kSym)->getTradingState() == TradingState::VolatilityAuction);
}

void reopensOnTheEngineClock(bool async) {
    MatchingEngine e;
    e.addSymbol(kSym);
    OrderBook* book = e.getOrderBook(kSym);
    assert(book->getVolatilityAuctionDwellNs() == 5 * kMinute && "default dwell is 5 minutes");
    uint64_t now = 1'000;
    e.setExpiryClock([&] { return now; });
    Recorder rec;
    book->setEventListener(&rec);
    if (async) e.startAsync(1, 1024); else e.start();
    tripThroughEngine(e);

    auto tick = [&](uint64_t t) { now = t; e.expireOrdersFromClock(); e.waitForDrain(); };
    tick(1'000);                                  // the dwell clock starts here
    tick(1'000 + 5 * kMinute - 1);
    assert(book->getTradingState() == TradingState::VolatilityAuction && "reopened early");
    assert(rec.trades.empty());
    tick(1'000 + 5 * kMinute);
    assert(book->getTradingState() == TradingState::Continuous &&
           "the volatility auction never reopened");
    assert(rec.trades.size() == 1 && rec.trades[0].quantity == 60 &&
           "the reopen did not run the reopening cross");
    if (async) e.stopAsync(); else e.stop();
    std::printf("  %s: the auction reopens after the dwell, with the cross\n",
                async ? "async" : "sync");
}

void replayReproducesTheTrip() {
    const fs::path path = uniqueTempPath("volatility_breaker.journal");
    fs::remove(path);
    fs::remove(fs::path(path.string() + ".tmp"));
    {
        MatchingEngine e;
        e.addSymbol(kSym);
        assert(e.enableJournal(path.string()));
        e.start();
        tripThroughEngine(e);
        e.stop();
    }
    // A replaying engine with no breaker at all must still land in the auction
    // with order 3 resting, not matched.
    MatchingEngine r;
    r.addSymbol(kSym);
    r.getOrderBook(kSym)->setCircuitBreakerThreshold(1e9);
    assert(r.enableJournal(path.string()));
    r.replayJournal();
    OrderBook* book = r.getOrderBook(kSym);
    assert(book->getTradingState() == TradingState::VolatilityAuction &&
           "replay did not reproduce the breaker trip");
    assert(book->getOrder(3) && book->getOrder(3)->remainingQty == 60 &&
           "replay matched the order that tripped the breaker");
    assert(book->getTradeCount() == 0);
    fs::remove(path);
    std::puts("  replay reproduces a trip whatever its own breaker setting");
}

}  // namespace

int main() {
    farPassiveOrderIsAdmitted();
    outOfBandFirstFillTrips();
    trippingMarketOrderIsParkedAndIocCancelled();
    onlyTheFirstFillCounts();
    neverTripsInAnAuction();
    reopensOnTheEngineClock(false);
    reopensOnTheEngineClock(true);
    replayReproducesTheTrip();
    std::puts("VolatilityBreakerTest passed");
    return 0;
}
