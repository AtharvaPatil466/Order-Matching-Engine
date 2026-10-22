// JournalIdCountersTest — client-visible ids never repeat across a restart
// (roadmap 1.4-D1; JRN-1, AUCT-16 sequence part).
//
// Two counters restarted at 1 in every process:
//   - the engine sequence id every submit returns to its client (a
//     process-global allocator nothing persisted);
//   - each book's trade id. Replay of a whole journal re-runs the matches and
//     re-derives it, but a checkpoint keeps only resting orders, so after any
//     checkpoint a restart issued trade 1 again.
// Now the sequence ids are leased from the journal (a durable record names the
// highest id that may be handed out before the next one is written), and a
// checkpoint records each book's next trade id along with the lease.
//
// Each life of the engine is its own process: the allocator is process-global,
// so a second engine in the same process would continue it and hide the bug.
// The first life is killed with SIGKILL, after a checkpoint.

#include "MatchingEngine.h"
#include "OrderBook.h"
#include "TempPath.h"

#include <cassert>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace OrderMatcher;

namespace {

constexpr SymbolId kSym = 1;
constexpr Price kPx = 10000;

struct Ids {
    uint64_t maxSequenceId = 0;
    uint64_t lastTradeId = 0;
};

struct TradeIds : EventListener {
    uint64_t last = 0;
    void onTrade(const Trade& t) override { last = t.tradeId; }
    void onOrderUpdate(const OrderUpdate&) override {}
    void onMarketData(const MarketDataUpdate&) override {}
    void onBookVisible(const BookVisibleUpdate&) override {}
};

// Runs `life` in a child process, which writes its Ids to the pipe and then
// waits to be killed. Returns what it reported.
Ids inChild(const std::function<Ids()>& life) {
    int fds[2];
    assert(pipe(fds) == 0);
    std::fflush(stdout);
    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(fds[0]);
        const Ids ids = life();
        (void)!write(fds[1], &ids, sizeof ids);
        for (;;) pause();
    }
    close(fds[1]);
    Ids ids{};
    assert(read(fds[0], &ids, sizeof ids) == static_cast<ssize_t>(sizeof ids) &&
           "the child died before reporting");
    close(fds[0]);
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    return ids;
}

void submit(MatchingEngine& e, OrderId id, Side side, Price px, Ids& ids) {
    const SubmitResult r = e.submitOrder(kSym, id, static_cast<ParticipantId>(id), side, px, 10,
                                          OrderType::Limit);   // no self-trade
    assert(r.isAccepted());
    if (r.sequenceId > ids.maxSequenceId) ids.maxSequenceId = r.sequenceId;
}

}  // namespace

int main() {
    const std::string path = uniqueTempPath("journal_id_counters.wal");
    std::filesystem::remove(path);

    // Life 1: one trade, one resting order, a checkpoint, then kill -9.
    const Ids first = inChild([&] {
        MatchingEngine engine;
        TradeIds trades;
        engine.addSymbol(kSym);
        engine.getOrderBook(kSym)->setEventListener(&trades);
        engine.enableJournal(path);
        engine.start();
        Ids ids;
        submit(engine, 1, Side::Sell, kPx, ids);
        submit(engine, 2, Side::Buy, kPx, ids);        // trade
        submit(engine, 3, Side::Buy, kPx - 10, ids);   // rests
        engine.checkpoint();
        ids.lastTradeId = trades.last;
        return ids;
    });

    // Life 2: replay, then trade against the order the checkpoint kept.
    const Ids second = inChild([&] {
        MatchingEngine engine;
        TradeIds trades;
        engine.addSymbol(kSym);
        engine.getOrderBook(kSym)->setEventListener(&trades);
        engine.enableJournal(path);
        engine.replayJournal();
        engine.start();
        assert(engine.getOrderBook(kSym)->getOrder(3) && "the checkpoint lost order 3");
        Ids ids;
        submit(engine, 4, Side::Sell, kPx - 10, ids);   // trades with order 3
        ids.lastTradeId = trades.last;
        return ids;
    });
    std::filesystem::remove(path);

    std::printf("  before kill -9: sequence ids up to %llu, trade %llu\n",
                (unsigned long long)first.maxSequenceId, (unsigned long long)first.lastTradeId);
    std::printf("  after restart:  sequence id %llu, trade %llu\n",
                (unsigned long long)second.maxSequenceId, (unsigned long long)second.lastTradeId);
    assert(first.lastTradeId == 1 && second.lastTradeId != 0);
    assert(second.maxSequenceId > first.maxSequenceId &&
           "a restarted engine handed out a sequence id it had already given a client");
    assert(second.lastTradeId > first.lastTradeId &&
           "a restarted engine reused a trade id the checkpoint did not keep");
    std::puts("JournalIdCountersTest passed");
    return 0;
}
