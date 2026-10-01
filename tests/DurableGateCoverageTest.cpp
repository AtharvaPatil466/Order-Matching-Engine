// DurableGateCoverageTest — roadmap 1.4-D7, the bracket part. With durable
// client acks on, NOTHING a client can see may precede the journal entry
// behind it, whichever request produced it.
//
// Only submitOrder opened a DurabilityGate group. A cancel, modify, replace,
// expiry or kill sweep dispatched its OrderUpdate and MarketData events
// straight through the gate and journaled afterwards, so under group commit a
// client — and the market-data feed — saw an order leave the book while the
// record of it leaving sat in a user-space batch. kill -9 then restored the
// order on restart, after everyone had been told it was gone.
//
// Each case rests one order and makes it durable, then runs the request and
// requires its events to be held until the journal is flushed, then released.

#include "MatchingEngine.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <functional>
#include <iostream>
#include <string>
#include <unistd.h>

using namespace OrderMatcher;

namespace {

constexpr SymbolId      kSym   = 0;
constexpr OrderId       kOrder = 1;
constexpr ParticipantId kOwner = 9;
constexpr Price         kPx    = 1'000'000;

struct Seen : EventListener {
    size_t events = 0;
    void onTrade(const Trade&) override { ++events; }
    void onOrderUpdate(const OrderUpdate&) override { ++events; }
    void onMarketData(const MarketDataUpdate&) override { ++events; }
    void onBookVisible(const BookVisibleUpdate&) override { ++events; }
};

const uint64_t kExpiry = static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count()) +
    3'600'000'000'000ull;

bool held(const char* what, const std::function<void(MatchingEngine&)>& request) {
    const char* tmp = std::getenv("TMPDIR");
    const std::string path = std::string(tmp ? tmp : "/tmp") + "/durable_gate_" +
                             std::to_string(::getpid()) + "_" + what + ".wal";
    std::remove(path.c_str());

    Seen seen;
    size_t early = 0, late = 0;
    {
        MatchingEngine engine;
        engine.addSymbol(kSym);
        engine.getOrderBook(kSym)->setEventListener(&seen);
        assert(engine.enableJournal(path));          // group commit: batches of 64
        engine.start();
        assert(engine.enableDurableClientAcks(true));

        engine.submitOrder(kSym, kOrder, kOwner, Side::Buy, kPx, 10, OrderType::Limit,
                           0, 0, TimeInForce::GTD, kExpiry);
        assert(engine.getOrderBook(kSym)->getOrder(kOrder) && "setup order must rest");
        engine.getJournal()->flush();
        seen.events = 0;

        request(engine);
        assert(!engine.getOrderBook(kSym)->getOrder(kOrder) ||
               std::string(what) == "modify" || std::string(what) == "replace");
        early = seen.events;
        engine.getJournal()->flush();
        late = seen.events;
        engine.stop();
    }
    std::remove(path.c_str());

    std::cout << "  " << what << ": " << early << " event(s) before the commit, " << late
              << " after" << std::endl;
    return early == 0 && late > 0;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= held("cancel",  [](MatchingEngine& e) { e.cancelOrder(kSym, kOrder); });
    ok &= held("modify",  [](MatchingEngine& e) { assert(e.modifyOrder(kSym, kOrder, 5)); });
    ok &= held("replace", [](MatchingEngine& e) {
        assert(e.cancelReplace(kSym, kOrder, kPx - 100, 10));
    });
    ok &= held("expiry",  [](MatchingEngine& e) { e.expireOrders(kExpiry + 1); });
    ok &= held("kill",    [](MatchingEngine& e) { e.killSwitch(kOwner); });
    assert(ok && "every journaled request must hold its events until the entry is durable");
    std::cout << "DurableGateCoverageTest PASSED" << std::endl;
    return 0;
}
