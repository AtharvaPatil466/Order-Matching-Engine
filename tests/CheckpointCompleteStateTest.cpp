// CheckpointCompleteStateTest — a restart from a checkpoint decides the way the
// engine that wrote it would have.
//
// Each case runs the same operations twice: once on a live engine that keeps
// going, once on an engine restarted from a checkpoint taken at the same point.
// The rest of the flow then runs on both, and the books must agree. A case
// names the piece of state the checkpoint used to lose.

#include "MatchingEngine.h"
#include "TempPath.h"

#include <cassert>
#include <cstdio>
#include <functional>
#include <map>
#include <string>

using namespace OrderMatcher;

namespace {

constexpr SymbolId S = 0;

struct BookState {
    std::map<OrderId, std::pair<Price, Quantity>> resting;
    uint64_t trades = 0;
    Price lastTrade = 0;
    TradingState state = TradingState::Continuous;
    bool operator==(const BookState&) const = default;
};

BookState capture(MatchingEngine& e) {
    BookState s;
    OrderBook* b = e.getOrderBook(S);
    b->forEachOrderLocked([&](const Order& o) { s.resting[o.id] = {o.price, o.remainingQty}; });
    s.trades = b->getTradeCount();
    s.lastTrade = b->getSnapshot().lastTradePrice;
    s.state = b->getTradingState();
    return s;
}

void print(const char* tag, const BookState& s) {
    std::printf("    %-10s state=%d trades=%llu last=%lld resting:", tag,
                static_cast<int>(s.state), static_cast<unsigned long long>(s.trades),
                static_cast<long long>(s.lastTrade));
    for (const auto& [id, pq] : s.resting)
        std::printf(" #%llu=%llu@%lld", static_cast<unsigned long long>(id),
                    static_cast<unsigned long long>(pq.second), static_cast<long long>(pq.first));
    std::printf("\n");
}

using Step = std::function<void(MatchingEngine&)>;
using Setup = std::function<void(MatchingEngine&)>;

// `before` runs, then a checkpoint, then `after`. Live: on one engine. Restarted:
// `before` and the checkpoint on one engine, `after` on a fresh one replayed
// from that checkpoint (or from the plain journal, without one).
bool sameAfterRestart(const char* name, const Setup& setup, const Step& before,
                      const Step& after, bool takeCheckpoint = true) {
    const std::string path = uniqueTempPath(std::string("ckpt_complete_") + name + ".wal");
    std::remove(path.c_str());
    BookState live, restarted;
    {
        MatchingEngine e;
        e.addSymbol(S);
        setup(e);
        e.start();
        before(e);
        after(e);
        live = capture(e);
        e.stop();
    }
    {
        MatchingEngine e;
        e.addSymbol(S);
        setup(e);
        assert(e.enableJournal(path, Journal::SyncPolicy::Immediate));
        e.start();
        before(e);
        if (takeCheckpoint) e.checkpoint();
        e.stop();
    }
    {
        MatchingEngine e;
        e.addSymbol(S);
        setup(e);
        assert(e.enableJournal(path, Journal::SyncPolicy::Immediate));
        e.replayJournal();
        const bool refused = e.restoreRefusals() != 0;
        e.start();
        after(e);
        restarted = capture(e);
        e.stop();
        if (refused) std::printf("    restore refused %zu order(s)\n", e.restoreRefusals());
    }
    std::remove(path.c_str());
    const bool same = live == restarted;
    std::printf("  %s: %s\n", name, same ? "same" : "DIVERGED");
    if (!same) {
        print("live", live);
        print("restarted", restarted);
    }
    return same;
}

const Setup kNoBreaker = [](MatchingEngine& e) {
    e.getOrderBook(S)->setCircuitBreakerThreshold(1e9);
};

// MIDS-1. An auction's self-cross resolution cancels the newer leg. A restore
// re-stamped every order in bids-then-asks order, so after a restart every bid
// was older than every ask and the other leg was cancelled.
bool auctionSelfCrossKeepsAge() {
    return sameAfterRestart(
        "auction_self_cross_age", kNoBreaker,
        [](MatchingEngine& e) {
            e.setTradingStateBatch({S}, TradingState::AuctionOpen);
            e.submitOrder(S, 1, 1, Side::Sell, 99, 100, OrderType::Limit);   // P, older
            e.submitOrder(S, 2, 1, Side::Buy, 101, 100, OrderType::Limit);   // P, newer
            e.submitOrder(S, 3, 2, Side::Buy, 100, 60, OrderType::Limit);    // Q
        },
        [](MatchingEngine& e) { e.uncrossBatch({S}); });
}

// JOURNAL-2. With no configured band, the book anchors a ±100,000-tick range
// on its first resting price (150000 here: [50000, 250000]). A restore
// re-anchored it on the first order put back, the best bid at 60000, so the
// ask at 240000 no longer fitted and the boot was refused; and an order the
// live range takes after the restart (245000) must still rest.
bool anchoredRangeSurvives() {
    return sameAfterRestart(
        "anchored_range", [](MatchingEngine&) {},
        [](MatchingEngine& e) {
            e.submitOrder(S, 1, 1, Side::Sell, 150000, 10, OrderType::Limit);  // anchors
            e.submitOrder(S, 2, 2, Side::Buy, 60000, 10, OrderType::Limit);
            e.submitOrder(S, 3, 1, Side::Sell, 240000, 10, OrderType::Limit);
        },
        [](MatchingEngine& e) {
            e.submitOrder(S, 4, 1, Side::Sell, 245000, 10, OrderType::Limit);
            e.submitOrder(S, 5, 2, Side::Buy, 55000, 10, OrderType::Limit);
        });
}

// The anchor outlives the orders that set it: an emptied book keeps its range
// live, so a restart must too, or the next order re-anchors somewhere else.
bool anchoredRangeOfEmptyBookSurvives() {
    return sameAfterRestart(
        "anchored_range_empty_book", [](MatchingEngine&) {},
        [](MatchingEngine& e) {
            e.submitOrder(S, 1, 1, Side::Sell, 150000, 10, OrderType::Limit);  // anchors
            e.cancelOrder(S, 1);
        },
        [](MatchingEngine& e) {
            e.submitOrder(S, 2, 2, Side::Buy, 60000, 10, OrderType::Limit);
            e.submitOrder(S, 3, 1, Side::Sell, 240000, 10, OrderType::Limit);
        });
}

// JOURNAL-7. A volatility auction reopens once it has run its dwell, timed
// from the first due-check in the auction. A restart started the dwell again.
bool volatilityDwellSurvives() {
    constexpr uint64_t kDwell = 1000, kStart = 5000;
    return sameAfterRestart(
        "volatility_dwell",
        [](MatchingEngine& e) { e.getOrderBook(S)->setVolatilityAuctionDwellNs(kDwell); },
        [](MatchingEngine& e) {
            e.setTradingStateBatch({S}, TradingState::VolatilityAuction);
            e.expireOrders(kStart);  // the dwell starts here
        },
        [](MatchingEngine& e) { e.expireOrders(kStart + kDwell); });
}

// JOURNAL-5. The engine-wide kill switch refuses every new order until it is
// released. A restart released it: nothing journaled it.
bool killSwitchSurvives(bool takeCheckpoint) {
    return sameAfterRestart(
        takeCheckpoint ? "kill_switch" : "kill_switch_plain_journal", [](MatchingEngine&) {},
        [](MatchingEngine& e) {
            e.submitOrder(S, 1, 1, Side::Buy, 100, 10, OrderType::Limit);
            e.setKillSwitch(true);
        },
        [](MatchingEngine& e) { e.submitOrder(S, 2, 1, Side::Buy, 100, 10, OrderType::Limit); },
        takeCheckpoint);
}

// Released before the restart, it stays released.
bool releasedKillSwitchStaysReleased() {
    return sameAfterRestart(
        "kill_switch_released", [](MatchingEngine&) {},
        [](MatchingEngine& e) {
            e.setKillSwitch(true);
            e.setKillSwitch(false);
        },
        [](MatchingEngine& e) { e.submitOrder(S, 2, 1, Side::Buy, 100, 10, OrderType::Limit); },
        false);
}

}  // namespace

int main() {
    bool ok = true;
    ok &= auctionSelfCrossKeepsAge();
    ok &= anchoredRangeSurvives();
    ok &= anchoredRangeOfEmptyBookSurvives();
    ok &= volatilityDwellSurvives();
    ok &= killSwitchSurvives(true);
    ok &= killSwitchSurvives(false);
    ok &= releasedKillSwitchStaysReleased();
    if (!ok) {
        std::puts("CheckpointCompleteStateTest FAILED: a restart from a checkpoint decided "
                  "differently from the engine that wrote it");
        return 1;
    }
    std::puts("CheckpointCompleteStateTest passed");
    return 0;
}
