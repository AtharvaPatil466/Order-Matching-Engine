// SessionEndTest — the end of a trading session retires DAY orders.
//
// WHAT WAS WRONG (roadmap 1.8-H8, with 1.4-D9). Nothing ended a session.
// No binary ran SessionScheduler, and its PostClose phase only set the state,
// so a DAY order without an expiryTime behaved exactly like GTC: it rested
// into tomorrow. The DAY sweep existed only inside gracefulShutdown, and a
// process stop is not a session end (roadmap 0.8).
//
// WHAT IS PINNED. MatchingEngine::endTradingSession is the explicit
// session-end command: PostClose (running the closing cross) and then a
// journaled cancel of every DAY order on those symbols — and only those.
// SessionScheduler's close calls it. parseSessionSchedule reads the
// `HH:MM,HH:MM,HH:MM,HH:MM` form OrderEngine's --session-schedule takes, and
// refuses anything else.

#include "EventListener.h"
#include "SessionScheduler.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <unistd.h>

using namespace OrderMatcher;
namespace fs = std::filesystem;

namespace {

void submit(MatchingEngine& e, SymbolId s, OrderId id, Price px, TimeInForce tif) {
    e.submitOrder(s, id, 10, Side::Buy, px, 10, OrderType::Limit, 0, 0, tif);
}

void schedulerCloseCancelsDayOrders() {
    MatchingEngine engine;
    engine.addSymbol(0);
    engine.start();
    uint64_t now = 0;
    SessionSchedule sched;
    SessionScheduler scheduler(engine, {0}, sched);
    scheduler.setClock([&] { return now; });

    now = sched.openMs;
    scheduler.tick();
    submit(engine, 0, 1, 100, TimeInForce::DAY);
    submit(engine, 0, 2, 100, TimeInForce::GTC);
    now = sched.closeMs;
    scheduler.tick();

    const OrderBook* book = engine.getOrderBook(0);
    assert(book->getTradingState() == TradingState::PostClose);
    assert(book->getOrder(1) == nullptr && "a DAY order outlived the session's close");
    assert(book->getOrder(2) != nullptr && "the close cancelled a GTC order");
    engine.stop();
    std::puts("  the scheduler's close cancels DAY orders and keeps GTC");
}

void endTradingSessionIsJournaledAndScoped() {
    const fs::path path = fs::temp_directory_path() /
        ("session_end_" + std::to_string(::getpid()) + ".bin");
    fs::remove(path);
    {
        MatchingEngine engine;
        engine.addSymbol(0);
        engine.addSymbol(1);
        assert(engine.enableJournal(path.string()));
        engine.start();
        submit(engine, 0, 1, 100, TimeInForce::DAY);
        submit(engine, 0, 2, 100, TimeInForce::GTC);
        submit(engine, 1, 3, 100, TimeInForce::DAY);   // another session's symbol
        engine.setTradingStateBatch({0}, TradingState::AuctionClose);
        assert(engine.endTradingSession({0}) == 1);
        assert(engine.getOrderBook(0)->getTradingState() == TradingState::PostClose);
        assert(engine.getOrderBook(0)->getOrder(1) == nullptr);
        assert(engine.getOrderBook(1)->getOrder(3) != nullptr &&
               "ending one symbol's session cancelled another symbol's DAY order");
        engine.stop();
    }
    MatchingEngine replay;
    replay.addSymbol(0);
    replay.addSymbol(1);
    assert(replay.enableJournal(path.string()));
    replay.replayJournal();
    assert(replay.getOrderBook(0)->getOrder(1) == nullptr && "the DAY cancel was not journaled");
    assert(replay.getOrderBook(0)->getOrder(2) != nullptr);
    assert(replay.getOrderBook(0)->getTradingState() == TradingState::PostClose);
    fs::remove(path);
    std::puts("  endTradingSession is journaled and touches only its symbols");
}

void parsesTheScheduleFlag() {
    const auto s = parseSessionSchedule("09:00,09:30,15:55,16:00");
    assert(s && s->preOpenMs == 32400000 && s->openMs == 34200000 &&
           s->closeAuctionMs == 57300000 && s->closeMs == 57600000 &&
           s->postCloseMs == 57600000);
    assert(!parseSessionSchedule(""));
    assert(!parseSessionSchedule("09:00,09:30,15:55"));          // three times
    assert(!parseSessionSchedule("9:00,09:30,15:55,16:00"));     // not HH:MM
    assert(!parseSessionSchedule("09:00,09:30,15:55,24:00"));    // no such time
    assert(!parseSessionSchedule("09:00,09:30,15:55,16:60"));
    assert(!parseSessionSchedule("09:30,09:00,15:55,16:00"));    // out of order
    assert(!parseSessionSchedule("09:00,09:30,15:55,16:00,"));
    std::puts("  --session-schedule parses HH:MM x4 in order, refuses anything else");
}

}  // namespace

int main() {
    schedulerCloseCancelsDayOrders();
    endTradingSessionIsJournaledAndScoped();
    parsesTheScheduleFlag();
    std::puts("SessionEndTest passed");
    return 0;
}
