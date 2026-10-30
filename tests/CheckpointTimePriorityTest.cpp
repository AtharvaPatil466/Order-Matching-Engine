// CheckpointTimePriorityTest — a price level keeps its time priority across a
// checkpoint.
//
// A checkpoint wrote the resting orders in order-id order (the id map's), and
// replay re-adds them in that order, so a restored level queued by id. Ids are
// the client's and say nothing about arrival: an order that lost its place (a
// replace that grows it) went back to the front of the level.

#include "MatchingEngine.h"
#include "TempPath.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace OrderMatcher;

namespace {

constexpr SymbolId kSym = 0;
constexpr Price kPx = 10000;

void boot(MatchingEngine& engine, const std::string& path) {
    engine.addSymbol(kSym);
    assert(engine.enableJournal(path, Journal::SyncPolicy::Immediate));
    engine.start();
}

// Order 1 then 2 bid at 100.00; 1 grows to 20, which sends it behind 2.
void queue(MatchingEngine& e) {
    assert(e.submitOrder(kSym, 1, 1, Side::Buy, kPx, 10, OrderType::Limit).isAccepted());
    assert(e.submitOrder(kSym, 2, 2, Side::Buy, kPx, 10, OrderType::Limit).isAccepted());
    assert(e.cancelReplace(kSym, 1, kPx, 20));
}

// A sell for 10 fills whichever order is at the front of the level.
OrderId front(MatchingEngine& e) {
    assert(e.submitOrder(kSym, 3, 3, Side::Sell, kPx, 10, OrderType::Limit).isAccepted());
    const OrderBook& b = *e.getOrderBook(kSym);
    return b.getOrder(2) ? 1 : 2;
}

OrderId frontAfter(bool checkpoint) {
    const std::string path = uniqueTempPath("checkpoint_priority.wal");
    std::remove(path.c_str());
    {
        MatchingEngine engine;
        boot(engine, path);
        queue(engine);
        if (checkpoint) engine.checkpoint();
        engine.stop();
    }
    MatchingEngine restarted;
    boot(restarted, path);
    restarted.replayJournal();
    const OrderId id = front(restarted);
    restarted.stop();
    std::remove(path.c_str());
    return id;
}

}  // namespace

int main() {
    const std::string path = uniqueTempPath("checkpoint_priority_live.wal");
    std::remove(path.c_str());
    OrderId live = 0;
    {
        MatchingEngine engine;
        boot(engine, path);
        queue(engine);
        live = front(engine);
        engine.stop();
    }
    std::remove(path.c_str());
    const OrderId replayed = frontAfter(false), checkpointed = frontAfter(true);
    std::printf("  front of the level: live %llu, replayed %llu, checkpointed %llu\n",
                (unsigned long long)live, (unsigned long long)replayed,
                (unsigned long long)checkpointed);
    assert(live == 2 && "the grown order should have lost its place");
    assert(replayed == live && "a replayed level lost its time priority");
    assert(checkpointed == live && "a checkpointed level lost its time priority");
    std::puts("CheckpointTimePriorityTest passed");
    return 0;
}
