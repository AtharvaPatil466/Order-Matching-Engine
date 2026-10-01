// StopLatchCheckpointTest — roadmap 1.4-D8 (PRIOR-9). An elected stop that is
// waiting for execution budget must still be elected after checkpoint + replay.
//
// One print elects every stop it reaches, but a sweep executes at most
// kMaxStopExecutionsPerSweep of them; the rest are latched (isStopTriggered)
// and fire on the next sweep wherever the price has gone. A checkpoint
// rewrites the journal as Snapshot records, which did not carry the latch, so
// after a restart those stops were plain parked stops again — and a price that
// had come back below their trigger never fired them: elected orders silently
// un-elected.
//
// Plain journal replay is not the problem: it re-runs the print and latches
// them again. Only the snapshot loses it, so the test checkpoints first.

#include "MatchingEngine.h"

#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>
#include <unistd.h>

using namespace OrderMatcher;

namespace {

constexpr SymbolId kSym = 0;
constexpr OrderId kFirstStop = 1000;
constexpr size_t kStops = OrderBook::kMaxStopExecutionsPerSweep + 6;  // 6 latched

void boot(MatchingEngine& engine, const std::string& path) {
    engine.addSymbol(kSym);
    assert(engine.enableJournal(path, Journal::SyncPolicy::Immediate));
    engine.start();
    engine.getOrderBook(kSym)->setCircuitBreakerThreshold(0.99);
}

// Stops still parked, and how many of those are latched.
std::pair<size_t, size_t> parkedStops(const MatchingEngine& engine) {
    size_t parked = 0, latched = 0;
    for (OrderId id = kFirstStop; id < kFirstStop + kStops; ++id) {
        const Order* o = engine.getOrderBook(kSym)->getOrder(id);
        if (o && o->type == OrderType::Stop) {
            ++parked;
            latched += o->isStopTriggered;
        }
    }
    return {parked, latched};
}

}  // namespace

int main() {
    const char* tmp = std::getenv("TMPDIR");
    const std::string path = std::string(tmp ? tmp : "/tmp") + "/stop_latch_" +
                             std::to_string(::getpid()) + ".wal";
    std::remove(path.c_str());

    {
        MatchingEngine engine;
        boot(engine, path);
        engine.submitOrder(kSym, 1, 1, Side::Sell, 10000, 1, OrderType::Limit);
        // Buy stops triggering at 100.00, each becoming a buy at 99.00 that rests.
        for (OrderId id = kFirstStop; id < kFirstStop + kStops; ++id)
            engine.submitOrder(kSym, id, 3, Side::Buy, 9900, 1, OrderType::Stop, 10000);
        // The print at 100.00 elects all of them; the budget executes 64.
        engine.submitOrder(kSym, 2, 2, Side::Buy, 10000, 1, OrderType::Limit);
        const auto [parked, latched] = parkedStops(engine);
        assert(parked == 6 && latched == 6);

        // A joining backup is built from the same Snapshot records, streamed.
        MatchingEngine backup;
        backup.addSymbol(kSym);
        backup.start();
        backup.setReplayModeAllBooks(true);
        engine.streamSnapshot([&](const JournalEntry& e) { backup.applyReplicatedEntry(e); });
        assert(parkedStops(backup).second == 6 && "a backup must receive the latch too");
        backup.stop();

        engine.checkpoint();
        engine.stop();
    }

    MatchingEngine restarted;
    boot(restarted, path);
    restarted.replayJournal();
    assert(parkedStops(restarted).first == 6);
    // The price comes back: a print at 99.00, below the stops' trigger. Latched
    // stops fire on this sweep anyway; un-latched ones would not.
    restarted.submitOrder(kSym, 3, 4, Side::Sell, 9900, 1, OrderType::Limit);
    const auto [parked, latched] = parkedStops(restarted);
    if (parked != 0)
        std::cout << "  " << parked << " elected stop(s) still parked after checkpoint + "
                  << "replay (" << latched << " latched)" << std::endl;
    assert(parked == 0 && "an elected stop must stay elected across checkpoint + replay");
    restarted.stop();

    std::remove(path.c_str());
    std::cout << "StopLatchCheckpointTest PASSED" << std::endl;
    return 0;
}
