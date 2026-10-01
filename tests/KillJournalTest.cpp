// KillJournalTest — roadmap 1.4-D6 (THR-2). A per-participant kill must leave
// the same trace as the engine-wide one: every cancel journaled, every working
// reservation released.
//
// killSwitch(pid) cancelled through OrderBook::cancelAllForParticipant, which
// neither journals nor reports exposure. So a restart replayed the killed
// orders back onto the book, live and tradeable, and until then the
// participant's position limit still counted them as working.
//
// Both engine modes: the sync sweep and the async worker's KillSwitch request
// were separate copies of the same omission.

#include "MatchingEngine.h"

#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>
#include <unistd.h>

using namespace OrderMatcher;

namespace {

constexpr ParticipantId kVictim    = 7;
constexpr ParticipantId kBystander = 8;

size_t resting(const MatchingEngine& engine, ParticipantId pid) {
    size_t n = 0;
    for (SymbolId sym : {SymbolId{0}, SymbolId{1}}) {
        if (const OrderBook* book = engine.getOrderBook(sym))
            book->forEachOrder([&](const Order& o) { n += o.participantId == pid; });
    }
    return n;
}

void runKill(bool async) {
    const char* mode = async ? "async" : "sync";
    const char* tmp = std::getenv("TMPDIR");
    const std::string path = std::string(tmp ? tmp : "/tmp") + "/kill_journal_" +
                             std::to_string(::getpid()) + "_" + mode + ".wal";
    std::remove(path.c_str());

    {
        MatchingEngine engine;
        engine.addSymbol(0);
        engine.addSymbol(1);
        assert(engine.enableJournal(path, Journal::SyncPolicy::Immediate));
        engine.setPositionLimit(kVictim, 1'000'000);
        if (async) engine.startAsync(2, 1024); else engine.start();

        engine.submitOrder(0, 1, kVictim, Side::Buy, 10000, 10, OrderType::Limit);
        engine.submitOrder(1, 2, kVictim, Side::Buy, 9900, 5, OrderType::Limit);
        engine.submitOrder(0, 3, kBystander, Side::Sell, 10100, 20, OrderType::Limit);
        if (async) engine.waitForDrain();
        assert(resting(engine, kVictim) == 2);
        assert(engine.getPosition(kVictim) == 15);

        engine.killSwitch(kVictim);

        assert(resting(engine, kVictim) == 0);
        const int64_t position = engine.getPosition(kVictim);
        if (position != 0)
            std::cout << "  " << mode << ": position after kill = " << position << std::endl;
        assert(position == 0 && "a killed order's working exposure must be released");
        engine.stop();
    }

    MatchingEngine restarted;
    restarted.addSymbol(0);
    restarted.addSymbol(1);
    assert(restarted.enableJournal(path));
    restarted.start();
    restarted.replayJournal();
    const size_t back = resting(restarted, kVictim);
    if (back != 0)
        std::cout << "  " << mode << ": " << back << " killed order(s) back after replay"
                  << std::endl;
    assert(back == 0 && "a killed order must not come back on replay");
    assert(resting(restarted, kBystander) == 1);
    restarted.stop();

    std::remove(path.c_str());
    std::cout << "  " << mode << " kill journaled and released" << std::endl;
}

}  // namespace

int main() {
    runKill(false);
    runKill(true);
    std::cout << "KillJournalTest PASSED" << std::endl;
    return 0;
}
