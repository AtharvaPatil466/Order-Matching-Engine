// SessionClockStepTest — a backward clock step is not a new trading day.
//
// WHAT WAS WRONG (roadmap 1.8-H7, finding AUCT-7). SessionScheduler::tick()
// reset the session to Idle whenever the clock read earlier than the previous
// tick, on the theory that only a midnight wrap does that. A routine 1 ms NTP
// step during the closing auction therefore replayed the whole day in one
// tick: PreOpen, the OPENING uncross (crossing the closing-auction book three
// minutes early), Continuous, and back into the closing auction, with a
// second closing cross at 16:00.
//
// WHAT IS PINNED. The session clock never runs backwards within a trading
// date; a new day is a change of trading date, not a smaller ms-of-day.

#include "EventListener.h"
#include "SessionScheduler.h"

#include <cassert>
#include <cstdio>
#include <map>

using namespace OrderMatcher;

namespace {

struct Recorder : EventListener {
    size_t trades = 0;
    std::map<OrderId, OrderUpdate> last;
    void onTrade(const Trade&) override { ++trades; }
    void onOrderUpdate(const OrderUpdate& u) override { last[u.orderId] = u; }
};

void backwardStepInTheCloseIsIgnored() {
    MatchingEngine engine;
    engine.addSymbol(0);
    engine.start();
    OrderBook* book = engine.getOrderBook(0);
    book->setCircuitBreakerThreshold(1e9);
    Recorder rec;
    book->setEventListener(&rec);

    uint64_t now = 0;
    SessionSchedule sched;
    SessionScheduler scheduler(engine, {0}, sched);
    scheduler.setClock([&] { return now; });
    auto tick = [&](uint64_t t) { now = t; scheduler.tick(); };

    tick(sched.preOpenMs);
    tick(sched.openMs);
    engine.submitOrder(0, 1, 1, Side::Buy, 100, 100, OrderType::Limit);
    engine.submitOrder(0, 2, 2, Side::Sell, 101, 100, OrderType::Limit);
    tick(sched.closeAuctionMs);
    engine.submitOrder(0, 3, 3, Side::Buy, 101, 50, OrderType::LOC);
    engine.submitOrder(0, 4, 4, Side::Sell, 0, 30, OrderType::MOC);

    tick(sched.closeAuctionMs + 120000);       // 15:57:00.000
    tick(sched.closeAuctionMs + 119999);       // the wall clock steps back 1 ms
    assert(scheduler.currentPhase() == SessionPhase::CloseAuction &&
           "a 1 ms backward step restarted the trading day");
    assert(book->getTradingState() == TradingState::AuctionClose);
    assert(rec.trades == 0 && "the closing-auction book was crossed early");

    engine.submitOrder(0, 5, 5, Side::Sell, 0, 40, OrderType::MOC);  // still the close
    tick(sched.closeMs);
    assert(scheduler.currentPhase() == SessionPhase::PostClose);
    assert(rec.last[5].status == OrderStatus::Filled &&
           "an MOC sent after the step missed today's close");
    engine.stop();
    std::puts("  a backward clock step in the closing auction changes nothing");
}

}  // namespace

int main() {
    backwardStepInTheCloseIsIgnored();
    std::puts("SessionClockStepTest passed");
    return 0;
}
