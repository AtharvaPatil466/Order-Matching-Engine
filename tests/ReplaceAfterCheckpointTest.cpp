// ReplaceAfterCheckpointTest — a replace journaled after a checkpoint replays
// against the quantity the order had already executed (roadmap 1.5-E1).
//
// cancelReplace's newQty is the order's new total, so applying it needs the
// order's cumQty: total 80 with 60 executed leaves 20. A checkpoint rewrites
// the journal as Snapshot records, which carried only remainingQty, so every
// reader restored a partially filled order as an unfilled one — and the same
// replace then left 80 working on the restored book against 20 on the live
// one. Plain journal replay is not the problem (it re-runs the fills); only
// the snapshot loses them, so the restart replays from a checkpoint.

#include "MatchingEngine.h"
#include "TempPath.h"

#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>

using namespace OrderMatcher;

namespace {

constexpr SymbolId kSym = 0;
constexpr Price kPx = 10000;

void boot(MatchingEngine& engine, const std::string& path) {
    engine.addSymbol(kSym);
    assert(engine.enableJournal(path, Journal::SyncPolicy::Immediate));
    engine.start();
    engine.getOrderBook(kSym)->setCircuitBreakerThreshold(0.99);
}

// Order 1 after "buy 100, 60 filled, replaced to a total of 80".
void expectReplaced(const OrderBook& book, const char* who) {
    const Order* o = book.getOrder(1);
    assert(o && o->price == kPx - 1);
    if (o->remainingQty != 20)
        std::cout << "  " << who << ": order 1 works " << o->remainingQty
                  << ", the live book works 20" << std::endl;
    assert(o->remainingQty == 20 && "a replace after a checkpoint lost the executed 60");
    assert(o->cumQty == 60 && o->initialQty == 80);
}

}  // namespace

int main() {
    const std::string path = uniqueTempPath("replace_ckpt.wal");
    std::remove(path.c_str());

    {
        MatchingEngine engine;
        boot(engine, path);
        engine.submitOrder(kSym, 1, 1, Side::Buy, kPx, 100, OrderType::Limit);
        engine.submitOrder(kSym, 2, 2, Side::Sell, kPx, 60, OrderType::Limit);
        assert(engine.getOrderBook(kSym)->getOrder(1)->remainingQty == 40);
        engine.checkpoint();

        assert(engine.cancelReplace(kSym, 1, kPx - 1, 80));
        expectReplaced(*engine.getOrderBook(kSym), "live");
        engine.stop();
    }

    MatchingEngine restarted;
    boot(restarted, path);
    restarted.replayJournal();
    expectReplaced(*restarted.getOrderBook(kSym), "restart replay");
    restarted.stop();

    std::remove(path.c_str());
    std::cout << "ReplaceAfterCheckpointTest PASSED" << std::endl;
    return 0;
}
