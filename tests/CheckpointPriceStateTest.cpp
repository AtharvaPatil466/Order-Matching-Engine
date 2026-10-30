// CheckpointPriceStateTest — a book's reference price and last trade price
// survive a checkpoint.
//
// Both lived only in what the journal replays: the reference is seeded by the
// first priced order and re-anchored by a volatility auction's reopening
// print, the last trade price by every trade. A checkpoint keeps resting
// orders, the session state and the id counters, and drops the trades, so a
// book restarted from one had neither. The circuit breaker (5% from the
// reference) then re-seeded from whatever order came next, and an auction
// with only market orders on both sides, which crosses at the last trade
// price (decision 6), cancelled them instead. A book that only quoted has a
// reference too, and no trade id for the checkpoint to record it beside.
//
// A parked trailing stop's Snapshot record carries the trigger its trail had
// ratcheted to, and re-adding the order threw it away: the trail restarted
// from the order's own price, then ratcheted from the last trade, which is
// below the high it had trailed.
//
// Each probe runs on the book as it is, on one restarted from the plain
// journal, on one restarted from a checkpoint, and on a JournalFollower's
// standby promoted over that checkpoint; all four must agree.

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
constexpr Price kPx = 10000;              // 100.00: the reference and the last trade
constexpr Price kJump = kPx * 12 / 10;    // 120.00: 20% away, past the 5% breaker

enum class Life { Uninterrupted, Replayed, Checkpointed, Followed };
const char* name(Life l) {
    switch (l) {
    case Life::Uninterrupted: return "uninterrupted";
    case Life::Replayed:      return "replayed";
    case Life::Checkpointed:  return "checkpointed";
    case Life::Followed:      return "followed";   // a promoted JournalFollower
    }
    return "";
}

struct Trades : EventListener {
    int count = 0;
    void onTrade(const Trade&) override { ++count; }
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
using Probe = std::function<int(OrderBook&)>;

// One trade at 100.00, nothing left resting.
void traded(MatchingEngine& e) {
    assert(e.submitOrder(kSym, 1, 1, Side::Sell, kPx, 10, OrderType::Limit).isAccepted());
    assert(e.submitOrder(kSym, 2, 2, Side::Buy, kPx, 10, OrderType::Limit).isAccepted());
    assert(!e.getOrderBook(kSym)->getOrder(1) && !e.getOrderBook(kSym)->getOrder(2));
}

// No trade: an order at 100.00 seeded the reference, then was cancelled.
void quoted(MatchingEngine& e) {
    assert(e.submitOrder(kSym, 1, 1, Side::Sell, kPx, 10, OrderType::Limit).isAccepted());
    e.cancelOrder(kSym, 1);
    assert(!e.getOrderBook(kSym)->getOrder(1));
}

// A sell trailing 1.00 behind the trade at 100.00; a trade at 104.00 ratchets
// its trigger to 103.00, and one at 103.50 leaves it there.
constexpr Price kTrail = 100;
void trade(MatchingEngine& e, OrderId id, Price px) {
    assert(e.submitOrder(kSym, id, id, Side::Sell, px, 10, OrderType::Limit).isAccepted());
    assert(e.submitOrder(kSym, id + 1, id + 1, Side::Buy, px, 10, OrderType::Limit).isAccepted());
}
void trailed(MatchingEngine& e) {
    traded(e);
    assert(e.submitOrder(kSym, 3, 3, Side::Sell, kPx, 10, OrderType::TrailingStop, 0, 0,
                         TimeInForce::GTC, 0, 0, PegType::None, 0, kTrail).isAccepted());
    trade(e, 10, kPx + 400);
    trade(e, 12, kPx + 350);
    assert(e.getOrderBook(kSym)->getOrder(3)->stopPrice == kPx + 300);
}

// `setup`, then `probe` on that book, or on one rebuilt from its journal (the
// followed one after a checkpoint too).
int after(Life life, const Setup& setup, const Probe& probe) {
    const std::string path = uniqueTempPath("checkpoint_prices.wal");
    std::remove(path.c_str());
    int result = 0;
    {
        MatchingEngine engine;
        boot(engine, path);
        setup(engine);
        if (life == Life::Uninterrupted) result = probe(*engine.getOrderBook(kSym));
        if (life == Life::Checkpointed || life == Life::Followed) engine.checkpoint();
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
    std::remove(path.c_str());
    return result;
}

// A print 20% from the reference trips the breaker into a volatility auction.
int breakerProbe(OrderBook& b) {
    b.addOrder(3, 3, Side::Sell, kJump, 10, OrderType::Limit);
    b.addOrder(4, 4, Side::Buy, kJump, 10, OrderType::Limit);
    return b.getTradingState() == TradingState::VolatilityAuction;
}

// Market orders on both sides of an intraday auction cross at the last trade.
int marketCrossProbe(OrderBook& b) {
    Trades trades;
    b.setEventListener(&trades);
    b.setTradingState(TradingState::AuctionOpen);
    b.addOrder(5, 5, Side::Buy, 0, 10, OrderType::Market);
    b.addOrder(6, 6, Side::Sell, 0, 10, OrderType::Market);
    b.setTradingState(TradingState::Continuous);
    b.setEventListener(nullptr);
    return trades.count;
}

// A print at 102.80 is under the ratcheted trigger, so the stop fires.
int trailProbe(OrderBook& b) {
    b.addOrder(20, 20, Side::Sell, kPx + 280, 1, OrderType::Limit);
    b.addOrder(21, 21, Side::Buy, kPx + 280, 1, OrderType::Limit);
    const Order* stop = b.getOrder(3);
    return !(stop && stop->type == OrderType::TrailingStop);
}

int check(const char* what, const Setup& setup, const Probe& probe, int want) {
    int failures = 0;
    for (Life life : {Life::Uninterrupted, Life::Replayed, Life::Checkpointed, Life::Followed}) {
        const int got = after(life, setup, probe);
        std::printf("  %-13s %-38s %d%s\n", name(life), what, got, got == want ? "" : "  <- differs");
        failures += got != want;
    }
    return failures;
}

}  // namespace

int main() {
    int failures = 0;
    failures += check("traded: breaker tripped at 120.00", traded, breakerProbe, 1);
    failures += check("traded: trades from market vs market", traded, marketCrossProbe, 1);
    failures += check("quoted: breaker tripped at 120.00", quoted, breakerProbe, 1);
    failures += check("trailed: stop fired at 102.80", trailed, trailProbe, 1);
    if (failures != 0) {
        std::printf("CheckpointPriceStateTest FAILED: %d restarted book(s) lost price state\n", failures);
        return 1;
    }
    std::puts("CheckpointPriceStateTest passed");
    return 0;
}
