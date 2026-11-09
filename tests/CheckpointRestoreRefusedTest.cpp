// CheckpointRestoreRefusedTest — a checkpoint order the restart cannot put
// back stops the boot instead of vanishing.
//
// A checkpoint replaces the journal, so its Snapshot record is the only copy
// of a resting order. replayJournal discarded what each re-add returned, and
// bootJournal reported a clean recovery over a book missing the orders it had
// refused. A restore can still fail — a price outside an instrument band
// narrowed in the config since, an order pool sized smaller than the one that
// wrote the checkpoint — and that must be loud: logged, counted, and the boot
// refused, so the journal is not checkpointed again without them.
//
// Here the restart's band leaves out one of two resting bids.

#include "JournalBoot.h"
#include "MatchingEngine.h"
#include "TempPath.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace OrderMatcher;

int main() {
    const std::string path = uniqueTempPath("checkpoint_refused.wal");
    std::remove(path.c_str());
    {
        MatchingEngine engine;
        engine.addSymbol(0);
        assert(engine.enableJournal(path, Journal::SyncPolicy::Immediate));
        engine.start();
        assert(engine.submitOrder(0, 1, 1, Side::Buy, 10000, 10, OrderType::Limit).isAccepted());
        assert(engine.submitOrder(0, 2, 2, Side::Buy, 20000, 10, OrderType::Limit).isAccepted());
        engine.gracefulShutdown();
        engine.stop();
    }

    MatchingEngine restarted;
    restarted.addSymbol(0);
    restarted.getOrderBook(0)->setPriceRange(5000, 15000);
    std::string program = "OrderEngine", flag = "--journal", file = path;
    char* argv[] = {program.data(), flag.data(), file.data(), nullptr};
    const int rc = bootJournal(restarted, 3, argv, Journal::SyncPolicy::Immediate);
    const OrderBook& book = *restarted.getOrderBook(0);
    const bool kept = book.getOrder(1) != nullptr, refusedOne = book.getOrder(2) == nullptr;
    std::printf("  bootJournal returned %d, restore refusals %zu, order 1 %s, order 2 %s\n", rc,
                restarted.restoreRefusals(), kept ? "restored" : "MISSING",
                refusedOne ? "refused" : "restored");
    std::remove(path.c_str());

    if (rc == 0 || restarted.restoreRefusals() != 1 || !kept || !refusedOne) {
        std::puts("CheckpointRestoreRefusedTest FAILED: a checkpoint order the restart could "
                  "not restore did not stop the boot");
        return 1;
    }
    std::puts("CheckpointRestoreRefusedTest passed");
    return 0;
}
