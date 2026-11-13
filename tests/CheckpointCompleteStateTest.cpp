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
    bool operator==(const BookState&) const = default;
};

BookState capture(MatchingEngine& e) {
    BookState s;
    OrderBook* b = e.getOrderBook(S);
    b->forEachOrderLocked([&](const Order& o) { s.resting[o.id] = {o.price, o.remainingQty}; });
    s.trades = b->getTradeCount();
    s.lastTrade = b->getSnapshot().lastTradePrice;
    return s;
}

void print(const char* tag, const BookState& s) {
    std::printf("    %-10s trades=%llu last=%lld resting:", tag,
                static_cast<unsigned long long>(s.trades), static_cast<long long>(s.lastTrade));
    for (const auto& [id, pq] : s.resting)
        std::printf(" #%llu=%llu@%lld", static_cast<unsigned long long>(id),
                    static_cast<unsigned long long>(pq.second), static_cast<long long>(pq.first));
    std::printf("\n");
}

using Step = std::function<void(MatchingEngine&)>;
using Setup = std::function<void(MatchingEngine&)>;

// `before` runs, then a checkpoint, then `after`. Live: on one engine. Restarted:
// `before` and the checkpoint on one engine, `after` on a fresh one replayed
// from that checkpoint.
bool sameAfterRestart(const char* name, const Setup& setup, const Step& before,
                      const Step& after) {
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
        e.checkpoint();
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

}  // namespace

int main() {
    bool ok = true;
    ok &= auctionSelfCrossKeepsAge();
    if (!ok) {
        std::puts("CheckpointCompleteStateTest FAILED: a restart from a checkpoint decided "
                  "differently from the engine that wrote it");
        return 1;
    }
    std::puts("CheckpointCompleteStateTest passed");
    return 0;
}
