// SessionTransitionTest — the session transition table is enforced
// (decision 7, roadmap 1.8-H4).
//
// WHAT WAS WRONG. setTradingState applied any transition it was handed:
// PostClose -> Continuous reopened a closed market, AuctionClose -> Continuous
// abandoned a closing auction, and a scheduler reset on the same trading date
// ran a second session after the close.
//
// WHAT IS PINNED. A transition off the table (docs/MatchingRules.md Rule 7.7)
// is refused and leaves the book where it was. PostClose -> PreOpen happens
// only for a later trading date, and that date survives journal recovery.

#include "MatchingEngine.h"
#include "SessionScheduler.h"
#include "TempPath.h"

#include <cstdio>
#include <filesystem>
#include <string>

using namespace OrderMatcher;
namespace fs = std::filesystem;

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
    if (ok) return;
    std::printf("  FAIL: %s\n", what);
    ++failures;
}

TradingState stateOf(MatchingEngine& e, SymbolId s) {
    return e.getOrderBook(s)->getTradingState();
}

void offTableTransitionsAreRefused() {
    MatchingEngine engine;
    for (SymbolId s : {1, 2, 3}) engine.addSymbol(s);
    engine.start();
    OrderBook* book = engine.getOrderBook(1);

    book->setTradingState(TradingState::AuctionClose);
    book->setTradingState(TradingState::Continuous);
    expect(book->getTradingState() == TradingState::AuctionClose,
           "AuctionClose -> Continuous was applied");
    book->setTradingState(TradingState::PostClose);
    book->setTradingState(TradingState::Continuous);
    expect(book->getTradingState() == TradingState::PostClose,
           "PostClose -> Continuous was applied");

    book = engine.getOrderBook(2);
    book->setTradingState(TradingState::PreOpen);
    book->setTradingState(TradingState::PostClose);
    expect(book->getTradingState() == TradingState::PreOpen,
           "PreOpen -> PostClose was applied");

    // Every edge on the table still works.
    book = engine.getOrderBook(3);
    const TradingState path[] = {
        TradingState::PreOpen,      TradingState::AuctionOpen, TradingState::Continuous,
        TradingState::VolatilityAuction, TradingState::Continuous, TradingState::Halted,
        TradingState::Continuous,   TradingState::AuctionClose, TradingState::PostClose};
    for (TradingState s : path) {
        book->setTradingState(s);
        expect(book->getTradingState() == s, "a transition on the table was refused");
    }
    engine.stop();
    std::puts("  transitions off the table are refused; the table's edges apply");
}

void sameDateResetDoesNotReopen() {
    MatchingEngine engine;
    engine.addSymbol(1);
    engine.start();
    uint64_t now = 0;
    SessionSchedule sched;
    SessionScheduler scheduler(engine, {1}, sched);
    scheduler.setClock([&] { return now; });
    scheduler.setTradingDateFn([] { return uint32_t{4}; });  // Monday 1970-01-05

    now = sched.closeMs;
    scheduler.tick();
    expect(stateOf(engine, 1) == TradingState::PostClose, "the session did not close");
    scheduler.resetSession();
    now = sched.preOpenMs;
    scheduler.tick();
    expect(stateOf(engine, 1) == TradingState::PostClose,
           "a second session ran on the same trading date");
    engine.getOrderBook(1)->setTradingState(TradingState::PreOpen);
    expect(stateOf(engine, 1) == TradingState::PostClose,
           "an undated PreOpen reopened a closed book");
    engine.stop();
    std::puts("  a closed book does not reopen on the same date");
}

// The date travels in the journal, and a checkpoint keeps it, so a restarted
// engine still refuses a second session that date.
void theSessionDateSurvivesRecovery(bool viaCheckpoint) {
    const fs::path path = uniqueTempPath(viaCheckpoint ? "session_date_ckpt.bin"
                                                       : "session_date.bin");
    fs::remove(path);
    constexpr uint32_t kMonday = 20'000 + 4;  // any weekday will do
    {
        MatchingEngine e;
        e.addSymbol(1);
        expect(e.enableJournal(path.string()), "journal did not open");
        e.start();
        expect(e.beginTradingSession({1}, kMonday) == 1, "the first session did not begin");
        e.setTradingStateBatch({1}, TradingState::Continuous);
        e.endTradingSession({1});
        expect(e.beginTradingSession({1}, kMonday) == 0, "a second session began the same date");
        expect(e.beginTradingSession({1}, kMonday - 1) == 0, "a session began on an earlier date");
        expect(e.setTradingStateBatch({1}, TradingState::PreOpen) == 0,
               "an undated PreOpen reopened a closed book");
        if (viaCheckpoint) e.checkpoint();
        e.stop();
    }
    MatchingEngine r;
    r.addSymbol(1);
    expect(r.enableJournal(path.string()), "journal did not reopen");
    r.replayJournal();
    r.start();
    expect(stateOf(r, 1) == TradingState::PostClose, "recovery lost the close");
    expect(r.beginTradingSession({1}, kMonday) == 0,
           "after recovery a second session began the same date");
    expect(r.beginTradingSession({1}, kMonday + 1) == 1, "the next date's session did not begin");
    r.stop();
    fs::remove(path);
    std::printf("  the session date survives recovery%s\n",
                viaCheckpoint ? " through a checkpoint" : "");
}

}  // namespace

int main() {
    std::puts("SessionTransitionTest");
    offTableTransitionsAreRefused();
    sameDateResetDoesNotReopen();
    theSessionDateSurvivesRecovery(/*viaCheckpoint=*/false);
    theSessionDateSurvivesRecovery(/*viaCheckpoint=*/true);
    if (failures) std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
