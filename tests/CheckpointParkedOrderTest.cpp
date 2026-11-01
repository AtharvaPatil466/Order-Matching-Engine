// CheckpointParkedOrderTest — parked orders keep their queue order across a
// checkpoint.
//
// Stops, trailing stops, auction market orders and on-close orders wait in
// lists, not price levels, and the list order decides: which elected stops
// run first and take the liquidity, which market orders an uncross fills
// first. A checkpoint re-added them in the order its walk of the order-id map
// produced, close to id order, so an order queued second but numbered lower
// came back first.
//
// In each case order 20 queued before order 10 and there is liquidity for one
// of them; each probe returns the one that traded. It runs on the book as it
// is, on one restarted from the plain journal, on one restarted from a
// checkpoint, on a JournalFollower's standby promoted over that checkpoint,
// and on an engine that applied the checkpoint record by record as a backup
// does; all five must agree.

#include "JournalFollower.h"
#include "MatchingEngine.h"
#include "TempPath.h"

#include <cassert>
#include <cstdio>
#include <functional>
#include <string>

using namespace OrderMatcher;

namespace {

constexpr SymbolId kSym = 0;
constexpr Price kPx = 10000;

enum class Life { Uninterrupted, Replayed, Checkpointed, Followed, Applied };
const char* name(Life l) {
    switch (l) {
    case Life::Uninterrupted: return "uninterrupted";
    case Life::Replayed:      return "replayed";
    case Life::Checkpointed:  return "checkpointed";
    case Life::Followed:      return "followed";   // a promoted JournalFollower
    case Life::Applied:       return "applied";    // applyReplicatedEntry
    }
    return "";
}

// The order on the other side of the first trade against `against`.
struct FirstTrade : EventListener {
    OrderId against;
    OrderId other = 0;
    explicit FirstTrade(OrderId a) : against(a) {}
    void onTrade(const Trade& t) override {
        if (other != 0) return;
        if (t.buyOrderId == against) other = t.sellOrderId;
        if (t.sellOrderId == against) other = t.buyOrderId;
    }
    void onOrderUpdate(const OrderUpdate&) override {}
    void onMarketData(const MarketDataUpdate&) override {}
    void onBookVisible(const BookVisibleUpdate&) override {}
};

void boot(MatchingEngine& engine, const std::string& path) {
    engine.addSymbol(kSym);
    assert(engine.enableJournal(path, Journal::SyncPolicy::Immediate));
    engine.start();
}

using Setup = std::function<void(MatchingEngine&)>;
using Probe = std::function<long long(OrderBook&)>;

void sell(MatchingEngine& e, OrderId id, Price px, Quantity qty, OrderType type,
          Price stopPrice = 0, Price trail = 0) {
    assert(e.submitOrder(kSym, id, id, Side::Sell, px, qty, type, stopPrice, 0,
                         TimeInForce::GTC, 0, 0, PegType::None, 0, trail).isAccepted());
}
void buy(MatchingEngine& e, OrderId id, Price px, Quantity qty, OrderType type) {
    assert(e.submitOrder(kSym, id, id, Side::Buy, px, qty, type).isAccepted());
}

// A print at 99.50 that sweeps the stops.
void print(OrderBook& b) {
    b.addOrder(40, 40, Side::Buy, kPx - 50, 1, OrderType::Limit);
    b.addOrder(41, 41, Side::Sell, kPx - 50, 1, OrderType::Limit);
}

// Two sell stops at 99.50; a bid of 5 for the first to fire into.
void stops(MatchingEngine& e) {
    buy(e, 1, kPx - 100, 5, OrderType::Limit);
    sell(e, 20, 0, 5, OrderType::Stop, kPx - 50);
    sell(e, 10, 0, 5, OrderType::Stop, kPx - 50);
}
long long stopsProbe(OrderBook& b) {
    FirstTrade t(1);
    b.setEventListener(&t);
    print(b);
    b.setEventListener(nullptr);
    return static_cast<long long>(t.other);
}

// Two sells trailing 0.50 behind 100.00: elected at 99.50, they rest there as
// limits in the order they fired; a buy of 5 takes the first.
void trailing(MatchingEngine& e) {
    sell(e, 20, kPx, 5, OrderType::TrailingStop, 0, 50);
    sell(e, 10, kPx, 5, OrderType::TrailingStop, 0, 50);
}
long long trailingProbe(OrderBook& b) {
    print(b);
    FirstTrade t(42);
    b.setEventListener(&t);
    b.addOrder(42, 42, Side::Buy, kPx - 50, 5, OrderType::Limit);
    b.setEventListener(nullptr);
    return static_cast<long long>(t.other);
}

// Two market buys parked in an opening auction; an offer of 5 at the uncross.
void auctionMarket(MatchingEngine& e) {
    assert(e.setTradingStateBatch({kSym}, TradingState::AuctionOpen) == 1);
    buy(e, 20, 0, 5, OrderType::Market);
    buy(e, 10, 0, 5, OrderType::Market);
    sell(e, 1, kPx, 5, OrderType::Limit);
}
long long auctionMarketProbe(OrderBook& b) {
    FirstTrade t(1);
    b.setEventListener(&t);
    b.setTradingState(TradingState::Continuous);
    b.setEventListener(nullptr);
    return static_cast<long long>(t.other);
}

// Two market-on-close buys parked for the close; an offer of 5 at it.
void onClose(MatchingEngine& e) {
    sell(e, 1, kPx, 5, OrderType::Limit);
    buy(e, 20, 0, 5, OrderType::MOC);
    buy(e, 10, 0, 5, OrderType::MOC);
}
long long onCloseProbe(OrderBook& b) {
    FirstTrade t(1);
    b.setEventListener(&t);
    b.setTradingState(TradingState::AuctionClose);
    b.setTradingState(TradingState::PostClose);
    b.setEventListener(nullptr);
    return static_cast<long long>(t.other);
}

long long after(Life life, const Setup& setup, const Probe& probe) {
    const std::string path = uniqueTempPath("checkpoint_parked.wal");
    std::remove(path.c_str());
    long long result = 0;
    {
        MatchingEngine engine;
        boot(engine, path);
        setup(engine);
        if (life == Life::Uninterrupted) result = probe(*engine.getOrderBook(kSym));
        if (life != Life::Uninterrupted && life != Life::Replayed) engine.checkpoint();
        engine.stop();
    }
    if (life == Life::Replayed || life == Life::Checkpointed) {
        MatchingEngine restarted;
        boot(restarted, path);
        restarted.replayJournal();
        result = probe(*restarted.getOrderBook(kSym));
        restarted.stop();
    }
    if (life == Life::Followed) {
        OrderBook standby(kSym);
        JournalFollower follower(path, kSym, standby);
        follower.promote();
        result = probe(standby);
    }
    if (life == Life::Applied) {
        MatchingEngine backup;
        backup.addSymbol(kSym);
        backup.setReplayModeAllBooks(true);
        Journal journal(path);
        for (const JournalEntry& e : journal.readAll(true, true)) backup.applyReplicatedEntry(e);
        backup.setReplayModeAllBooks(false);
        result = probe(*backup.getOrderBook(kSym));
    }
    std::remove(path.c_str());
    return result;
}

int check(const char* what, const Setup& setup, const Probe& probe) {
    int failures = 0;
    for (Life life : {Life::Uninterrupted, Life::Replayed, Life::Checkpointed, Life::Followed,
                      Life::Applied}) {
        const long long got = after(life, setup, probe);
        std::printf("  %-13s %-34s %lld%s\n", name(life), what, got, got == 20 ? "" : "  <- differs");
        failures += got != 20;
    }
    return failures;
}

}  // namespace

int main() {
    int failures = 0;
    failures += check("stop that fired first", stops, stopsProbe);
    failures += check("trailing stop that fired first", trailing, trailingProbe);
    failures += check("auction market order filled", auctionMarket, auctionMarketProbe);
    failures += check("MOC filled at the close", onClose, onCloseProbe);
    if (failures != 0) {
        std::printf("CheckpointParkedOrderTest FAILED: %d restarted book(s) reordered parked orders\n",
                    failures);
        return 1;
    }
    std::puts("CheckpointParkedOrderTest passed");
    return 0;
}
