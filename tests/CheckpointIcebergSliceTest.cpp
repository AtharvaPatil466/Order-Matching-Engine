// CheckpointIcebergSliceTest — an iceberg's partly consumed displayed slice
// survives a checkpoint.
//
// The Snapshot record carried the iceberg's leaves and display size but not
// what was left of the slice on show, so the re-add sliced it afresh: depth
// showed a full slice where the book had less, and the next aggressor took
// the whole fresh slice from the iceberg instead of the remainder and then the
// order queued behind it.
//
// Each probe runs on the book as it is, on one restarted from the plain
// journal and on one restarted from a checkpoint; all three must agree.

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

using Probe = std::function<long long(OrderBook&)>;

// An iceberg of 100 showing 10 has had 4 taken from its slice; a plain sell
// of 10 queues behind it.
void consumed(MatchingEngine& e) {
    assert(e.submitOrder(kSym, 1, 1, Side::Sell, kPx, 100, OrderType::Iceberg, 0, 10).isAccepted());
    assert(e.submitOrder(kSym, 2, 2, Side::Buy, kPx, 4, OrderType::Limit).isAccepted());
    assert(e.submitOrder(kSym, 3, 3, Side::Sell, kPx, 10, OrderType::Limit).isAccepted());
    assert(e.getOrderBook(kSym)->getOrder(1)->visibleQty == 6);
}

long long after(Life life, const Probe& probe) {
    const std::string path = uniqueTempPath("checkpoint_iceberg.wal");
    std::remove(path.c_str());
    long long result = 0;
    {
        MatchingEngine engine;
        boot(engine, path);
        consumed(engine);
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
    std::remove(path.c_str());
    return result;
}

long long sliceProbe(OrderBook& b) { return static_cast<long long>(b.getOrder(1)->visibleQty); }

// A buy of 8 takes the 6 left on show, then 2 from the order behind.
long long queueProbe(OrderBook& b) {
    b.addOrder(40, 4, Side::Buy, kPx, 8, OrderType::Limit);
    return static_cast<long long>(b.getOrder(3)->remainingQty);
}

int check(const char* what, const Probe& probe, long long want) {
    int failures = 0;
    for (Life life : {Life::Uninterrupted, Life::Replayed, Life::Checkpointed}) {
        const long long got = after(life, probe);
        std::printf("  %-13s %-38s %lld%s\n", name(life), what, got, got == want ? "" : "  <- differs");
        failures += got != want;
    }
    return failures;
}

}  // namespace

int main() {
    int failures = 0;
    failures += check("iceberg's slice on show", sliceProbe, 6);
    failures += check("order behind it after a buy of 8", queueProbe, 8);
    if (failures != 0) {
        std::printf("CheckpointIcebergSliceTest FAILED: %d restarted book(s) lost the slice\n", failures);
        return 1;
    }
    std::puts("CheckpointIcebergSliceTest passed");
    return 0;
}
