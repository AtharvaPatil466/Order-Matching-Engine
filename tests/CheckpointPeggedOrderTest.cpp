// CheckpointPeggedOrderTest — pegged orders come back from a checkpoint where
// they rested and in the queue order they had.
//
// A checkpoint re-adds the resting orders level by level, bids then asks, and
// a Pegged record's re-add re-pegged: restPeggedOrder recomputed the price
// from the reference against the half-restored book. A buy peg clamped one
// tick behind the ask came back on the ask, because no ask was restored yet;
// the restored ask then traded with it. A sell peg clamped behind the bid was
// refused (InvalidPrice) because that buy peg now sat on its price. Each later
// re-add ran a reprice pass against the same partial book.
//
// Each scenario runs on the book as it is, on one restarted from the plain
// journal and on one restarted from a checkpoint; the restored book must read
// the same in all three.

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

std::string after(Life life, const Setup& setup) {
    const std::string path = uniqueTempPath("checkpoint_pegged.wal");
    std::remove(path.c_str());
    std::string restored;
    {
        MatchingEngine engine;
        boot(engine, path);
        setup(engine);
        if (life == Life::Uninterrupted) restored = dump(*engine.getOrderBook(kSym));
        if (life == Life::Checkpointed) engine.checkpoint();
        engine.stop();
    }
    if (life != Life::Uninterrupted) {
        MatchingEngine restarted;
        boot(restarted, path);
        restarted.replayJournal();
        restored = dump(*restarted.getOrderBook(kSym));
        restarted.stop();
    }
    std::remove(path.c_str());
    return restored;
}

int check(const char* what, const Setup& setup) {
    int failures = 0;
    std::printf("  %s\n", what);
    const std::string want = after(Life::Uninterrupted, setup);
    for (Life life : {Life::Uninterrupted, Life::Replayed, Life::Checkpointed}) {
        const std::string got = life == Life::Uninterrupted ? want : after(life, setup);
        std::printf("    %-13s restored  %s%s\n", name(life), got.c_str(),
                    got == want ? "" : "  <- differs");
        failures += got != want;
    }
    return failures;
}

}  // namespace

int main() {
    int failures = 0;
    failures += check("pegs clamped behind the touch, two at one bid", clamped);
    if (failures != 0) {
        std::printf("CheckpointPeggedOrderTest FAILED: %d restarted book(s) differ\n", failures);
        return 1;
    }
    std::puts("CheckpointPeggedOrderTest passed");
    return 0;
}
