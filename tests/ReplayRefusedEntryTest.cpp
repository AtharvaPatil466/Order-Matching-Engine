// ReplayRefusedEntryTest — a journal entry the live engine applied and replay
// refuses stops the boot, naming the entry.
//
// The live engine journals an order only once the book admitted it, and a
// modify or replace only once it applied. So a refusal on replay means the
// recovered book is not the one that was journaled. replayJournal discarded
// what each entry returned and counted it as replayed, and bootJournal
// reported a clean recovery (Overview row 1; the checkpoint half was fixed in
// round 7, CheckpointRestoreRefusedTest).
//
// Here the restart's configured band is narrower than the one that wrote the
// journal: an admitted order and a replace no longer fit.

#include "JournalBoot.h"
#include "MatchingEngine.h"
#include "TempPath.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace OrderMatcher;

namespace {

int boot(MatchingEngine& engine, const std::string& path) {
    std::string program = "OrderEngine", flag = "--journal", file = path;
    char* argv[] = {program.data(), flag.data(), file.data(), nullptr};
    return bootJournal(engine, 3, argv, Journal::SyncPolicy::Immediate);
}

}  // namespace

int main() {
    const std::string path = uniqueTempPath("replay_refused.wal");
    std::remove(path.c_str());
    {
        MatchingEngine engine;
        engine.addSymbol(0);
        engine.getOrderBook(0)->setPriceRange(5000, 25000);
        engine.getOrderBook(0)->setCircuitBreakerThreshold(1e9);
        assert(engine.enableJournal(path, Journal::SyncPolicy::Immediate));
        engine.start();
        assert(engine.submitOrder(0, 1, 1, Side::Buy, 10000, 10, OrderType::Limit).isAccepted());
        assert(engine.submitOrder(0, 2, 2, Side::Buy, 20000, 10, OrderType::Limit).isAccepted());
        assert(engine.submitOrder(0, 3, 3, Side::Sell, 24000, 10, OrderType::Limit).isAccepted());
        assert(engine.cancelReplace(0, 1, 10100, 10, 1));   // in both bands
        assert(engine.cancelReplace(0, 3, 24100, 10, 3));   // only in the live one
        engine.commitJournal();
        engine.stop();
    }

    bool ok = true;
    {
        // The band that wrote it: everything applies, and the boot succeeds.
        MatchingEngine same;
        same.addSymbol(0);
        same.getOrderBook(0)->setPriceRange(5000, 25000);
        same.getOrderBook(0)->setCircuitBreakerThreshold(1e9);
        const int rc = boot(same, path);
        std::printf("  same band: bootJournal %d, replay refusals %zu\n", rc,
                    same.replayRefusals());
        if (rc != 0 || same.replayRefusals() != 0) ok = false;
    }
    {
        MatchingEngine narrowed;
        narrowed.addSymbol(0);
        narrowed.getOrderBook(0)->setPriceRange(5000, 15000);
        narrowed.getOrderBook(0)->setCircuitBreakerThreshold(1e9);
        const int rc = boot(narrowed, path);
        // Order 2 (20000) and order 3 (24000) are refused, and so is the
        // replace of 3, which replay can no longer find.
        std::printf("  narrowed band: bootJournal %d, replay refusals %zu\n", rc,
                    narrowed.replayRefusals());
        if (rc == 0 || narrowed.replayRefusals() != 3) ok = false;
    }
    std::remove(path.c_str());

    if (!ok) {
        std::puts("ReplayRefusedEntryTest FAILED: a journal entry replay refused did not stop "
                  "the boot");
        return 1;
    }
    std::puts("ReplayRefusedEntryTest passed");
    return 0;
}
