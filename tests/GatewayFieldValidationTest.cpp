// GatewayFieldValidationTest — audit PROTO-1, roadmap 0.13.
//
// TcpGateway::decodeFrame memcpy's the client's OrderRequest straight off the
// wire, enums and bool included. Nothing downstream range-checked them, so a
// side byte of 2 entered the book as a SELL and traded, an orderType one past
// the last enumerator rested, and a `hidden` byte of 2 was loaded as a bool —
// undefined behaviour (UBSan: "load of value 2 ... not a valid value for type
// 'bool'"). The SBE path already rejects these with InvalidFieldValue; this is
// the native gateway, the only order-entry protocol a shipped binary exposes.

#include "GatewayTestClient.h"

#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

using namespace OrderMatcher;

namespace {

int passed = 0;
#define TEST(name) std::cout << "  " << #name << "..." << std::flush
#define PASS() do { std::cout << " ok" << std::endl; ++passed; } while (0)

constexpr Price kPrice = 1012500;

// One past the last enumerator: the first value the schema does not define.
template <typename E, E Last>
constexpr E pastEnd() { return static_cast<E>(static_cast<uint8_t>(Last) + 1); }

struct Fixture {
    MatchingEngine engine;
    TcpGateway     gateway{engine};
    TestClient     client;

    Fixture() {
        engine.start();
        engine.addSymbol(0);
        assert(gateway.start(0));
        assert(client.connect(gateway.port()));
        client.setRecvTimeout(10);
    }
    ~Fixture() { gateway.stop(); engine.stop(); }

    // The Ack/Error answering `orderId`, skipping trade/update notifications.
    GatewayResponse answerFor(OrderId orderId) {
        for (int frames = 0; frames < 64; ++frames) {
            GatewayResponseV2 r{};
            assert(client.recvResponseV2(r));
            const auto t = r.response.type;
            if ((t == GatewayResponse::Type::Ack || t == GatewayResponse::Type::Error) &&
                r.response.orderId == orderId)
                return r.response;
        }
        assert(false && "no answer for order");
        return {};
    }

    GatewayResponse submit(const OrderRequest& req) {
        assert(client.sendOrderV2(req, req.orderId));
        return answerFor(req.orderId);
    }

    Quantity visibleBidQty() {
        engine.waitForDrain();
        const auto snap =
            engine.getOrderBook(0)->getSnapshot(MarketDataSnapshot::MAX_DEPTH);
        Quantity q = 0;
        for (size_t i = 0; i < snap.bidCount; ++i) q += snap.bids[i].totalQuantity;
        return q;
    }
    // getOrder() takes no lock; after waitForDrain() the worker is idle, which
    // is the read-only inspection its comment allows.
    bool resting(OrderId id) {
        engine.waitForDrain();
        return engine.getOrderBook(0)->getOrder(id) != nullptr;
    }
};

OrderRequest order(OrderId id, Side side) {
    OrderRequest req{};
    req.type          = OrderRequest::Type::NewOrder;
    req.symbolId      = 0;
    req.orderId       = id;
    req.participantId = 1;
    req.side          = side;
    req.price         = kPrice;
    req.qty           = 10;
    req.orderType     = OrderType::Limit;
    return req;
}

void expectInvalidField(const GatewayResponse& r) {
    assert(r.type == GatewayResponse::Type::Error);
    assert(r.rejectReason == RejectReason::InvalidFieldValue);
}

// ─── 1: an undefined side must not trade as a Sell ──────────────────────────
void test_OutOfRangeSideIsRejectedNotTraded() {
    TEST(OutOfRangeSideIsRejectedNotTraded);
    Fixture fx;
    assert(fx.submit(order(1, Side::Buy)).type == GatewayResponse::Type::Ack);

    expectInvalidField(fx.submit(order(2, pastEnd<Side, Side::Sell>())));

    assert(fx.visibleBidQty() == 10 && "the resting buy must not have been hit");
    assert(!fx.resting(2));
    PASS();
}

// ─── 2: orderType / tif / pegType one past the end are rejected ─────────────
void test_OutOfRangeEnumsAreRejected() {
    TEST(OutOfRangeEnumsAreRejected);
    Fixture fx;

    OrderRequest badType = order(10, Side::Buy);
    badType.orderType = pastEnd<OrderType, OrderType::LOC>();
    expectInvalidField(fx.submit(badType));

    OrderRequest badTif = order(11, Side::Buy);
    badTif.tif = pastEnd<TimeInForce, TimeInForce::DAY>();
    expectInvalidField(fx.submit(badTif));

    OrderRequest badPeg = order(12, Side::Buy);
    badPeg.pegType = pastEnd<PegType, PegType::PrimaryPeg>();
    expectInvalidField(fx.submit(badPeg));

    for (OrderId id : {10, 11, 12}) assert(!fx.resting(id) && "no undefined order may rest");

    // The last defined values are still legal — the bound is not off by one.
    OrderRequest last = order(13, Side::Sell);
    last.tif = TimeInForce::DAY;
    assert(fx.submit(last).type == GatewayResponse::Type::Ack);
    assert(fx.resting(13));
    PASS();
}

// ─── 3: the bare V1 frame (the other memcpy site) is checked too ────────────
void test_V1FrameOutOfRangeSideIsRejected() {
    TEST(V1FrameOutOfRangeSideIsRejected);
    Fixture fx;
    assert(fx.client.sendOrder(order(20, pastEnd<Side, Side::Sell>())));
    GatewayResponse r{};
    assert(fx.client.recvResponse(r));
    assert(r.orderId == 20);
    expectInvalidField(r);
    assert(!fx.resting(20));
    PASS();
}

// ─── 4: hidden is a byte on the wire; any non-zero value means hidden ───────
//
// Written with memcpy so this test never loads the invalid bool itself.
void test_HiddenByteTwoDecodesAsHidden() {
    TEST(HiddenByteTwoDecodesAsHidden);
    Fixture fx;
    OrderRequest req = order(30, Side::Buy);
    const uint8_t two = 2;
    std::memcpy(&req.hidden, &two, sizeof(two));

    assert(fx.submit(req).type == GatewayResponse::Type::Ack);
    assert(fx.resting(30));
    assert(fx.visibleBidQty() == 0 && "a hidden order must not show in the book");
    PASS();
}

}  // namespace

int main() {
    std::cout << "GatewayFieldValidationTest\n";
    test_OutOfRangeSideIsRejectedNotTraded();
    test_OutOfRangeEnumsAreRejected();
    test_V1FrameOutOfRangeSideIsRejected();
    test_HiddenByteTwoDecodesAsHidden();
    std::cout << passed << " passed\n";
    return 0;
}
