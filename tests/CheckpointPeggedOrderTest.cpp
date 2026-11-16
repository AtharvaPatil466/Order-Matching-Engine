// CheckpointPeggedOrderTest — pegged orders come back from a checkpoint where
// they rested, in the queue order they had, and reprice the next time the
// touch moves as they would have without the restart.
//
// A checkpoint re-adds the resting orders level by level, bids then asks, and
// a Pegged record's re-add re-pegged: restPeggedOrder recomputed the price
// from the reference against the half-restored book. A buy peg clamped one
// tick behind the ask came back on the ask, because no ask was restored yet;
// the restored ask then traded with it. A sell peg clamped behind the bid was
// refused (InvalidPrice) because that buy peg now sat on its price. Each later
// re-add ran a reprice pass against the same partial book.
//
// An auction's call period may cross the book around a peg already resting,
// and a sell peg, re-added after the bids, was refused for being through a bid
// that had arrived after it: the check is for a peg entering, not for one
// coming back.
//
// peggedOrders_ is the order a reprice pass moves the pegs in, and so the
// queue order they land in at a new price. A cancel reorders it (the list
// erases by swapping the last entry in). A checkpoint rebuilt it in level
// order, so two pegs that moved together came back in the other order.
//
// Each scenario runs on the book as it is, on one restarted from the plain
// journal and on one restarted from a checkpoint; the restored book and the
// book after a touch move must read the same in all three.

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

enum class Life { Uninterrupted, Replayed, Checkpointed };
const char* name(Life l) {
    switch (l) {
    case Life::Uninterrupted: return "uninterrupted";
    case Life::Replayed:      return "replayed";
    case Life::Checkpointed:  return "checkpointed";
    }
    return "";
}

void boot(MatchingEngine& engine, const std::string& path) {
    engine.addSymbol(kSym);
    assert(engine.enableJournal(path, Journal::SyncPolicy::Immediate));
    engine.start();
}

void limit(MatchingEngine& e, OrderId id, Side side, Price px) {
    assert(e.submitOrder(kSym, id, id, side, px, 10, OrderType::Limit).isAccepted());
}
void peg(MatchingEngine& e, OrderId id, Side side, Price offset) {
    assert(e.submitOrder(kSym, id, id, side, kPx, 10, OrderType::Pegged, 0, 0,
                         TimeInForce::GTC, 0, 0, PegType::PrimaryPeg, offset).isAccepted());
}

// Every order, level by level and front to back: "b5@10008x10" is order 5
// bidding 10 at 100.08.
std::string dump(const OrderBook& b) {
    std::string s;
    b.forEachOrderLocked([&](const Order& o) {
        s += (o.side == Side::Buy ? " b" : " s") + std::to_string(o.id) + "@" +
             std::to_string(o.price) + "x" + std::to_string(o.remainingQty);
    });
    return s;
}

using Setup = std::function<void(MatchingEngine&)>;
using Probe = std::function<void(OrderBook&)>;

// Pegs clamped behind the touch on both sides, two pegs at one bid with a
// limit ahead of them. Bid 99.90, ask 100.10; a buy peg +1 and a sell peg -1;
// a bid at 100.08 lifts the buy peg to 100.09, clamped to 100.08 behind the
// sell peg's 100.09; a second buy peg (offset 0) joins at 100.08; an offer at
// 100.09 queues behind the sell peg, which a re-peg would put at 100.08 and
// clamp back to 100.09.
void clamped(MatchingEngine& e) {
    limit(e, 1, Side::Buy, kPx - 10);
    limit(e, 2, Side::Sell, kPx + 10);
    peg(e, 3, Side::Buy, 1);
    peg(e, 4, Side::Sell, -1);
    limit(e, 5, Side::Buy, kPx + 8);
    peg(e, 6, Side::Buy, 0);
    limit(e, 7, Side::Sell, kPx + 9);
}
// The touch moves: an offer at 100.08 takes order 5, the only limit at the
// bid, and the buy pegs fall to the next one.
void clampedProbe(OrderBook& b) {
    b.addOrder(20, 20, Side::Sell, kPx + 8, 10, OrderType::Limit);
}

// Three buy pegs at the bid behind a limit; the first is cancelled, which
// swaps the last into its slot, so the reprice list reads 5, 4.
void swapped(MatchingEngine& e) {
    limit(e, 1, Side::Buy, kPx - 10);
    limit(e, 2, Side::Sell, kPx + 10);
    peg(e, 3, Side::Buy, 0);
    peg(e, 4, Side::Buy, 0);
    peg(e, 5, Side::Buy, 0);
    assert(e.submitCancel(kSym, 3).isAccepted());
}
// The touch moves up to 99.95 and both pegs follow it, in reprice-list order.
void swappedProbe(OrderBook& b) {
    b.addOrder(20, 20, Side::Buy, kPx - 5, 10, OrderType::Limit);
}

// Pegs resting when an auction starts; in the call period a bid above the
// sell peg and an offer below the buy peg cross the book, which an auction
// allows. The opening print moves the touch.
void auction(MatchingEngine& e) {
    limit(e, 1, Side::Buy, kPx - 10);
    limit(e, 2, Side::Sell, kPx + 10);
    peg(e, 3, Side::Sell, -1);
    peg(e, 4, Side::Buy, 1);
    assert(e.setTradingStateBatch({kSym}, TradingState::AuctionOpen) == 1);
    limit(e, 5, Side::Buy, kPx + 20);
    limit(e, 6, Side::Sell, kPx - 20);
}
void auctionProbe(OrderBook& b) {
    b.setTradingState(TradingState::Continuous);
}

struct Outcome { std::string restored, moved; };

Outcome after(Life life, const Setup& setup, const Probe& probe) {
    const std::string path = uniqueTempPath("checkpoint_pegged.wal");
    std::remove(path.c_str());
    Outcome out;
    const auto run = [&](OrderBook& b) {
        out.restored = dump(b);
        probe(b);
        out.moved = dump(b);
    };
    {
        MatchingEngine engine;
        boot(engine, path);
        setup(engine);
        if (life == Life::Uninterrupted) run(*engine.getOrderBook(kSym));
        if (life == Life::Checkpointed) engine.checkpoint();
        engine.stop();
    }
    if (life != Life::Uninterrupted) {
        MatchingEngine restarted;
        boot(restarted, path);
        restarted.replayJournal();
        run(*restarted.getOrderBook(kSym));
        restarted.stop();
    }
    std::remove(path.c_str());
    return out;
}

int check(const char* what, const Setup& setup, const Probe& probe) {
    int failures = 0;
    std::printf("  %s\n", what);
    const Outcome want = after(Life::Uninterrupted, setup, probe);
    for (Life life : {Life::Uninterrupted, Life::Replayed, Life::Checkpointed}) {
        const Outcome got = life == Life::Uninterrupted ? want : after(life, setup, probe);
        const bool restoredOk = got.restored == want.restored, movedOk = got.moved == want.moved;
        std::printf("    %-13s restored  %s%s\n", name(life), got.restored.c_str(),
                    restoredOk ? "" : "  <- differs");
        std::printf("    %-13s moved     %s%s\n", name(life), got.moved.c_str(),
                    movedOk ? "" : "  <- differs");
        failures += !restoredOk + !movedOk;
    }
    return failures;
}

}  // namespace

int main() {
    int failures = 0;
    failures += check("pegs clamped behind the touch, two at one bid", clamped, clampedProbe);
    failures += check("pegs whose reprice order a cancel swapped", swapped, swappedProbe);
    failures += check("pegs in an auction whose call period crossed the book", auction, auctionProbe);
    if (failures != 0) {
        std::printf("CheckpointPeggedOrderTest FAILED: %d restarted book state(s) differ\n",
                    failures);
        return 1;
    }
    std::puts("CheckpointPeggedOrderTest passed");
    return 0;
}
