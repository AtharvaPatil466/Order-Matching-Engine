// GatewayExpiresGtdTest — a GTD order the order-entry port accepted must be gone
// once its expiry time has passed.
//
// WHAT WAS WRONG. MatchingEngine has an expiry sweep and a timer to run it, and
// neither binary started either. GatewayServer — the binary that takes client
// orders — acknowledged a GTD order and let it rest, display and trade forever:
// the expiry field on the wire was stored and never read.
//
// WHY NOT THE ENGINE'S TIMER. This engine is synchronous; orders are processed on
// the gateway's event thread. The timer sweeps from a thread of its own, and a
// sweep emits events through the durability gate and into the market-data
// publisher, neither of which takes a lock — it would race the order path. The
// sweep runs on the event thread instead.
//
// HOW IT IS OBSERVED. Black-box, through the order port; GatewayServer has no
// endpoint to read the book with. Three resting orders — a GTD due in a second,
// a GTD due in an hour, a GTC — then a wait past the first expiry and a sweep
// interval, then a cancel for each. OrderNotFound for the first means it
// expired. An Ack for the other two means the sweep compared times rather than
// clearing every GTD, and that the probe can say yes at all.
//
// Usage: GatewayExpiresGtdTest <path-to-GatewayServer>

#include "GatewayProcess.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace OrderMatcher;
using namespace gwtest;

namespace {

// Unix-epoch nanoseconds, the unit of expiryTime on the wire.
uint64_t wallNsIn(std::chrono::nanoseconds fromNow) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        (std::chrono::system_clock::now() + fromNow).time_since_epoch()).count());
}

struct Resting {
    OrderId id;
    Price price;
    TimeInForce tif;
    uint64_t expiryTime;
};

OrderRequest request(OrderRequest::Type type, const Resting& o) {
    OrderRequest r{};
    r.type          = type;
    r.symbolId      = 0;
    r.orderId       = o.id;
    r.participantId = 7;
    r.side          = Side::Buy;
    r.price         = o.price;
    r.qty           = 10;
    r.orderType     = OrderType::Limit;
    r.tif           = o.tif;
    r.expiryTime    = o.expiryTime;
    return r;
}

// Cancel the order; true if it was there to cancel.
bool stillResting(Client& c, const Resting& o) {
    assert(c.send(request(OrderRequest::Type::Cancel, o)));
    GatewayResponse r{};
    assert(c.answerFor(o.id, r) && "no answer to the cancel");
    if (r.type == GatewayResponse::Type::Ack) return true;
    assert(r.rejectReason == RejectReason::OrderNotFound &&
           "the cancel was refused for a reason other than the order being gone");
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    assert(argc == 2 && "usage: GatewayExpiresGtdTest <path-to-GatewayServer>");
    const char* bin = argv[1];
    uint16_t port = 48230;
    const std::string journal = tempJournalPath("ob_gateway_gtd");
    std::remove(journal.c_str());

    const pid_t pid = startOnFreePort(bin, port, journal);
    assert(pid > 0 && "GatewayServer never started listening");

    const Resting soon {7001, 9900, TimeInForce::GTD, wallNsIn(std::chrono::seconds(1))};
    const Resting later{7002, 9800, TimeInForce::GTD, wallNsIn(std::chrono::hours(1))};
    const Resting gtc  {7003, 9700, TimeInForce::GTC, 0};

    Client c;
    assert(c.connect(port));
    for (const Resting* o : {&soon, &later, &gtc}) {
        assert(c.send(request(OrderRequest::Type::NewOrder, *o)));
        GatewayResponse r{};
        assert(c.answerFor(o->id, r) && r.type == GatewayResponse::Type::Ack &&
               "the gateway did not acknowledge an order this test depends on");
    }

    // Past the first expiry (1 s) and a full sweep interval (1 s), with margin.
    std::this_thread::sleep_for(std::chrono::seconds(4));

    const bool soonResting  = stillResting(c, soon);
    const bool laterResting = stillResting(c, later);
    const bool gtcResting   = stillResting(c, gtc);

    // Stop the process before asserting, so a failure does not leak it.
    const bool stopped = stopCleanly(pid);
    std::remove(journal.c_str());

    std::printf("  GTD due in 1 s: %s\n", soonResting ? "STILL RESTING" : "expired");
    std::printf("  GTD due in 1 h: %s\n", laterResting ? "resting" : "GONE");
    std::printf("  GTC:            %s\n", gtcResting ? "resting" : "GONE");

    assert(!soonResting && "a GTD order is still resting 3 s after its expiry time");
    assert(laterResting && "a GTD order expired an hour early — the sweep ignored its time");
    assert(gtcResting && "a GTC order was swept");
    assert(stopped && "GatewayServer did not exit cleanly on SIGTERM");

    std::puts("GatewayExpiresGtdTest passed");
    return 0;
}
