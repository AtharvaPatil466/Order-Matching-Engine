// GatewayParticipantIsolationTest — one participant cannot reach another's
// orders through the order port (MIDS-3, MIDS-7).
//
// Order ids are chosen by the client. They used to go into the book as sent,
// so two participants shared one id space: B could probe which ids A had live,
// rest an id first so A's genuine order was refused, and, with authentication
// off, cancel A's order by naming its id. And a participant id is a claim when
// authentication is off, so a kill switch naming the engine's kill-all
// sentinel cancelled every participant's orders on every symbol.
//
// The engine here is OrderEngine's shape: async, behind TcpGateway, no
// credentials (the case where the participant id is only a claim).

#include "GatewayTestClient.h"
#include "MatchingEngine.h"
#include "OrderBook.h"

#include <cassert>
#include <cstdio>
#include <limits>
#include <string>

using namespace OrderMatcher;

namespace {

constexpr ParticipantId kA = 1;
constexpr ParticipantId kB = 2;

OrderRequest restingBuy(ParticipantId p, SymbolId sym, OrderId id, Price px) {
    OrderRequest r{};
    r.type = OrderRequest::Type::NewOrder;
    r.symbolId = sym;
    r.orderId = id;
    r.participantId = p;
    r.side = Side::Buy;
    r.price = px;
    r.qty = 10;
    r.orderType = OrderType::Limit;
    r.tif = TimeInForce::GTC;
    return r;
}

// The Ack or Error answering a request, skipping any report frames.
GatewayResponse answer(TestClient& c, const OrderRequest& r) {
    assert(c.sendOrder(r));
    GatewayResponse resp{};
    for (;;) {
        assert(c.recvResponse(resp));
        if (resp.type == GatewayResponse::Type::Ack || resp.type == GatewayResponse::Type::Error)
            return resp;
    }
}

// Resting orders on `sym` and how many of them `p` owns. The worker is idle
// after waitForDrain, so walking the book from here is safe.
struct Count { size_t all = 0; size_t owned = 0; };
Count resting(MatchingEngine& engine, SymbolId sym, ParticipantId p) {
    engine.waitForDrain();
    Count n;
    engine.getOrderBook(sym)->forEachOrder([&](const Order& o) {
        ++n.all;
        if (o.participantId == p) ++n.owned;
    });
    return n;
}

struct Venue {
    MatchingEngine engine;
    TcpGateway gateway{engine};
    Venue() {
        engine.addSymbol(0);
        engine.addSymbol(1);
        engine.startAsync(1);
        assert(gateway.start(0));
    }
    ~Venue() { gateway.stop(); engine.stopAsync(); }
};

void sameIdTwoParticipants() {
    Venue v;
    TestClient a, b;
    assert(a.connect(v.gateway.port()) && b.connect(v.gateway.port()));
    a.setRecvTimeout(10);
    b.setRecvTimeout(10);

    assert(answer(a, restingBuy(kA, 0, 5, 100)).type == GatewayResponse::Type::Ack);
    assert(answer(b, restingBuy(kB, 0, 5, 99)).type == GatewayResponse::Type::Ack);
    const Count both = resting(v.engine, 0, kA);
    std::printf("  same id, two participants: %zu resting\n", both.all);
    assert(both.all == 2 && "one participant's id refused another's order");

    // B cancels "its" 5: only its own order goes.
    OrderRequest cancel = restingBuy(kB, 0, 5, 0);
    cancel.type = OrderRequest::Type::Cancel;
    assert(answer(b, cancel).type == GatewayResponse::Type::Ack);
    const Count afterCancel = resting(v.engine, 0, kA);
    assert(afterCancel.all == 1 && afterCancel.owned == 1 && "B's cancel reached A's order");

    // And B's id 5 is free again for B, whatever A holds.
    assert(answer(b, restingBuy(kB, 0, 5, 98)).type == GatewayResponse::Type::Ack);
    assert(resting(v.engine, 0, kA).all == 2);
    std::puts("sameIdTwoParticipants passed");
}

void killAllSentinelRefused() {
    Venue v;
    TestClient c;
    assert(c.connect(v.gateway.port()));
    c.setRecvTimeout(10);
    assert(answer(c, restingBuy(kA, 0, 1, 100)).type == GatewayResponse::Type::Ack);
    assert(answer(c, restingBuy(kB, 1, 2, 100)).type == GatewayResponse::Type::Ack);

    for (ParticipantId sentinel : {ParticipantId{0xFFFFFFFF},
                                   std::numeric_limits<ParticipantId>::max(),
                                   kMaxParticipants}) {
        OrderRequest kill{};
        kill.type = OrderRequest::Type::KillSwitch;
        kill.participantId = sentinel;
        const GatewayResponse r = answer(c, kill);
        assert(r.type == GatewayResponse::Type::Error && "an out-of-range participant was served");
        assert(r.rejectReason == RejectReason::InvalidFieldValue);
    }
    const size_t left = resting(v.engine, 0, kA).all + resting(v.engine, 1, kB).all;
    std::printf("  after kill-all attempts: %zu resting\n", left);
    assert(left == 2 && "a wire kill switch cancelled every participant's orders");

    // Every request type, not only the kill switch.
    assert(answer(c, restingBuy(kMaxParticipants, 0, 3, 90)).type == GatewayResponse::Type::Error);
    assert(answer(c, restingBuy(kA, 0, kMaxClientOrderId + 1, 90)).type ==
           GatewayResponse::Type::Error);
    assert(resting(v.engine, 0, kA).all == 1);
    std::puts("killAllSentinelRefused passed");
}

}  // namespace

// An argument names one case, to run it alone.
int main(int argc, char** argv) {
    const std::string only = argc > 1 ? argv[1] : "";
    if (only.empty() || only == "ids") sameIdTwoParticipants();
    if (only.empty() || only == "kill") killAllSentinelRefused();
    std::puts("GatewayParticipantIsolationTest passed");
    return 0;
}
