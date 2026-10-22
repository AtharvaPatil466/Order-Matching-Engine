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
// A halt is a pause: Halted resumes only to the state it was entered from
// (or the session ends, PostClose), so a halt is no detour around the table
// (Auction.tla NoReopenAfterClose); the pre-halt state survives recovery.

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

// Every detour through Halted that the table would otherwise allow.
void haltIsAPause() {
    struct Detour { TradingState from; const char* what; };
    const Detour detours[] = {
        {TradingState::AuctionClose,
         "Continuous -> AuctionClose -> Halted -> Continuous abandoned the closing auction"},
        {TradingState::PostClose,
         "Continuous -> PostClose -> Halted -> Continuous reopened a closed market"},
        {TradingState::PreOpen,
         "PreOpen -> Halted -> Continuous opened without the opening cross"},
        {TradingState::AuctionOpen,
         "AuctionOpen -> Halted -> Continuous skipped the auction's uncross"},
        {TradingState::VolatilityAuction,
         "VolatilityAuction -> Halted -> Continuous skipped the reopening cross"},
    };
    for (const Detour& d : detours) {
        OrderBook book(1);
        book.setCircuitBreakerThreshold(1e9);
        if (d.from == TradingState::AuctionOpen) book.setTradingState(TradingState::PreOpen);
        book.setTradingState(d.from);
        expect(book.getTradingState() == d.from, "could not reach the detour's start");
        book.setTradingState(TradingState::Halted);
        book.setTradingState(TradingState::Continuous);
        expect(book.getTradingState() == TradingState::Halted, d.what);
        book.setTradingState(d.from);  // resuming the paused phase is fine
        expect(book.getTradingState() == d.from, "a halt did not resume to its own phase");
    }
    OrderBook book(1);
    book.setTradingState(TradingState::Halted);
    book.setTradingState(TradingState::Continuous);
    expect(book.getTradingState() == TradingState::Continuous,
           "a halt from Continuous did not resume to Continuous");
    book.setTradingState(TradingState::Halted);
    book.setTradingState(TradingState::PostClose);
    expect(book.getTradingState() == TradingState::PostClose,
           "the session could not end while halted");
    std::puts("  a halt resumes only to the phase it paused");
}

// A book halted in its closing auction resumes that auction, and only that,
// across a restart, from the journal and from a checkpoint.
void thePreHaltStateSurvivesRecovery(bool viaCheckpoint) {
    const fs::path path = uniqueTempPath(viaCheckpoint ? "prehalt_ckpt.bin" : "prehalt.bin");
    fs::remove(path);
    {
        MatchingEngine e;
        e.addSymbol(1);
        expect(e.enableJournal(path.string()), "journal did not open");
        e.start();
        e.beginTradingSession({1}, 20'004);
        e.setTradingStateBatch({1}, TradingState::Continuous);
        e.setTradingStateBatch({1}, TradingState::AuctionClose);
        e.setTradingStateBatch({1}, TradingState::Halted);
        if (viaCheckpoint) e.checkpoint();
        e.stop();
    }
    MatchingEngine r;
    r.addSymbol(1);
    expect(r.enableJournal(path.string()), "journal did not reopen");
    r.replayJournal();
    r.start();
    expect(stateOf(r, 1) == TradingState::Halted, "recovery lost the halt");
    expect(r.setTradingStateBatch({1}, TradingState::Continuous) == 0,
           "after recovery a halted closing auction went back to Continuous");
    expect(r.setTradingStateBatch({1}, TradingState::AuctionClose) == 1,
           "after recovery the halt did not resume its closing auction");
    r.stop();
    fs::remove(path);
    std::printf("  the pre-halt state survives recovery%s\n",
                viaCheckpoint ? " through a checkpoint" : "");
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
    haltIsAPause();
    thePreHaltStateSurvivesRecovery(/*viaCheckpoint=*/false);
    thePreHaltStateSurvivesRecovery(/*viaCheckpoint=*/true);
    if (failures) std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
