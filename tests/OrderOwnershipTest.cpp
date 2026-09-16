// OrderOwnershipTest — the half of H7 the login frame could not reach.
//
// Authenticating a session proves who is asking. It says nothing about which
// orders they may touch, and Cancel/Modify/CancelReplace name an order by id
// alone:
//
//     submitCancel(symbolId, orderId)
//
// The engine never checked who owned that id. So a session that had logged in
// perfectly legitimately could cancel a competitor's resting order by guessing
// its id — and order ids run sequentially in most client stacks, so "guessing"
// overstates the difficulty. GatewayLoginTest's permits() check cannot see
// this: the identity is valid and the participant it claims is its own; it is
// the ORDER that belongs to someone else.
//
// Two layers are covered here because they fail differently:
//   1. MatchingEngine/OrderBook — the check itself, under the book lock.
//   2. TcpGateway — that a real authenticated session is actually held to it.

#include "GatewayTestClient.h"
#include "ParticipantAuth.h"

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

constexpr SymbolId      kSym   = 0;
constexpr ParticipantId kFirmA = 100;
constexpr ParticipantId kFirmB = 200;
constexpr Price         kPrice = 1012500;
constexpr Quantity      kQty   = 10;

Quantity restingQty(MatchingEngine& engine, Price price) {
    engine.waitForDrain();
    const auto snap =
        engine.getOrderBook(kSym)->getSnapshot(MarketDataSnapshot::MAX_DEPTH);
    for (size_t i = 0; i < snap.bidCount; ++i) {
        if (snap.bids[i].price == price) return snap.bids[i].totalQuantity;
    }
    return 0;
}

void restOrderForFirmA(MatchingEngine& engine, OrderId id) {
    const auto r = engine.submitOrder(kSym, id, kFirmA, Side::Buy, kPrice, kQty,
                                      OrderType::Limit);
    assert(r.isAccepted());
}

// ─── 1: another participant cannot cancel your resting order ────────────────
void test_ForeignCancelIsRefused() {
    TEST(ForeignCancelIsRefused);
    MatchingEngine engine;
    engine.start();
    engine.addSymbol(kSym);
    restOrderForFirmA(engine, 1);

    const auto r = engine.submitCancel(kSym, 1, kFirmB);
    assert(!r.isAccepted());
    assert(r.rejectReason == RejectReason::NotOrderOwner);
    assert(restingQty(engine, kPrice) == kQty && "the order must still be resting");

    // And the owner still can.
    assert(engine.submitCancel(kSym, 1, kFirmA).isAccepted());
    assert(restingQty(engine, kPrice) == 0);

    engine.stop();
    PASS();
}

// ─── 2: same for modify and cancel/replace ──────────────────────────────────
void test_ForeignModifyAndReplaceAreRefused() {
    TEST(ForeignModifyAndReplaceAreRefused);
    MatchingEngine engine;
    engine.start();
    engine.addSymbol(kSym);
    restOrderForFirmA(engine, 2);

    const auto mod = engine.submitModify(kSym, 2, kQty - 1, kFirmB);
    assert(!mod.isAccepted());
    assert(mod.rejectReason == RejectReason::NotOrderOwner);

    const auto rep = engine.submitCancelReplace(kSym, 2, kPrice + 100, kQty, kFirmB);
    assert(!rep.isAccepted());
    assert(rep.rejectReason == RejectReason::NotOrderOwner);

    // Untouched: same price, same size.
    assert(restingQty(engine, kPrice) == kQty);

    // The owner's own modify still works.
    assert(engine.submitModify(kSym, 2, kQty - 1, kFirmA).isAccepted());
    assert(restingQty(engine, kPrice) == kQty - 1);

    engine.stop();
    PASS();
}

// ─── 3: "not yours" must be indistinguishable from "not there" on the wire ──
//
// A distinct wire code would turn cancel into an order-id oracle: walk the id
// space, keep the ids that answer "not yours", and you have mapped a
// competitor's resting book without ever reading the market data.
void test_DenialIsIndistinguishableFromNotFound() {
    TEST(DenialIsIndistinguishableFromNotFound);
    assert(clientVisibleReason(RejectReason::NotOrderOwner) ==
           RejectReason::OrderNotFound);
    // Every other reason passes through untouched.
    assert(clientVisibleReason(RejectReason::RateLimitExceeded) ==
           RejectReason::RateLimitExceeded);
    assert(clientVisibleReason(RejectReason::None) == RejectReason::None);
    PASS();
}

// ─── 4: internal sweeps are not participant-scoped ──────────────────────────
//
// Expiry, auction cleanup and the kill switch act for the venue. If the
// ownership check caught them too, the engine would lose the ability to clean
// up its own book — which is why the default is kAnyParticipant rather than
// something a forgotten argument can produce by accident.
void test_InternalCallersAreNotScoped() {
    TEST(InternalCallersAreNotScoped);
    MatchingEngine engine;
    engine.start();
    engine.addSymbol(kSym);
    restOrderForFirmA(engine, 3);

    // No requester supplied — the venue acting on its own book.
    assert(engine.submitCancel(kSym, 3).isAccepted());
    assert(restingQty(engine, kPrice) == 0);

    // The kill switch is participant-scoped by its own argument, not by the
    // ownership check, and must still clear the target's orders.
    restOrderForFirmA(engine, 4);
    engine.killSwitch(kFirmA);
    engine.waitForDrain();
    assert(restingQty(engine, kPrice) == 0);

    engine.stop();
    PASS();
}

// ─── 5: a missing order still reads as missing, not as an ownership failure ─
void test_MissingOrderStillReportsNotFound() {
    TEST(MissingOrderStillReportsNotFound);
    MatchingEngine engine;
    engine.start();
    engine.addSymbol(kSym);

    const auto r = engine.submitCancel(kSym, 999, kFirmB);
    assert(!r.isAccepted());
    assert(r.rejectReason == RejectReason::OrderNotFound &&
           "an absent order is not an ownership failure");

    engine.stop();
    PASS();
}

// ─── 6: end to end through an authenticated binary session ──────────────────
//
// The layer that matters: a real logged-in client, holding a real credential,
// trying to cancel an order that is not its own.
void test_AuthenticatedSessionCannotCancelAnotherFirmsOrder() {
    TEST(AuthenticatedSessionCannotCancelAnotherFirmsOrder);
    MatchingEngine engine;
    engine.start();
    engine.addSymbol(kSym);

    ParticipantAuth auth;
    auth.addCredential("firm-a", "s3cret", {kFirmA});
    auth.addCredential("firm-b", "0th3r",  {kFirmB});

    TcpGateway gateway(engine);
    gateway.setParticipantAuth(&auth);
    assert(gateway.start(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    restOrderForFirmA(engine, 5);

    TestClient client;
    assert(client.connect(gateway.port()));
    client.setRecvTimeout(10);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    assert(client.sendLogin("firm-b", "0th3r"));
    GatewayResponseV2 ack{};
    assert(client.recvResponseV2(ack));
    assert(ack.response.type == GatewayResponse::Type::Ack);

    // firm-b is who it says it is, and 200 is its own participant id. The
    // order, however, is firm-a's.
    OrderRequest cancel{};
    cancel.type          = OrderRequest::Type::Cancel;
    cancel.symbolId      = kSym;
    cancel.orderId       = 5;
    cancel.participantId = kFirmB;
    assert(client.sendOrderV2(cancel, 0));

    GatewayResponseV2 resp{};
    assert(client.recvResponseV2(resp));
    assert(resp.response.type == GatewayResponse::Type::Error);
    assert(resp.response.rejectReason == RejectReason::OrderNotFound &&
           "the wire must not distinguish 'not yours' from 'not there'");
    assert(restingQty(engine, kPrice) == kQty && "firm-a's order was cancelled");

    gateway.stop();
    engine.stop();
    PASS();
}

// ─── 7: the same session can still cancel what it does own ──────────────────
//
// The check has to be narrow. A guard that also blocked legitimate cancels
// would be found in production, by a client unable to pull its own quotes.
void test_AuthenticatedSessionCanCancelItsOwn() {
    TEST(AuthenticatedSessionCanCancelItsOwn);
    MatchingEngine engine;
    engine.start();
    engine.addSymbol(kSym);

    ParticipantAuth auth;
    auth.addCredential("firm-b", "0th3r", {kFirmB});

    TcpGateway gateway(engine);
    gateway.setParticipantAuth(&auth);
    assert(gateway.start(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TestClient client;
    assert(client.connect(gateway.port()));
    client.setRecvTimeout(10);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    assert(client.sendLogin("firm-b", "0th3r"));
    GatewayResponseV2 ack{};
    assert(client.recvResponseV2(ack));
    assert(ack.response.type == GatewayResponse::Type::Ack);

    OrderRequest add{};
    add.type          = OrderRequest::Type::NewOrder;
    add.symbolId      = kSym;
    add.orderId       = 6;
    add.participantId = kFirmB;
    add.side          = Side::Buy;
    add.price         = kPrice;
    add.qty           = kQty;
    add.orderType     = OrderType::Limit;
    assert(client.sendOrderV2(add, 0));
    GatewayResponseV2 addResp{};
    assert(client.recvResponseV2(addResp));
    assert(addResp.response.type == GatewayResponse::Type::Ack);
    assert(restingQty(engine, kPrice) == kQty);

    OrderRequest cancel{};
    cancel.type          = OrderRequest::Type::Cancel;
    cancel.symbolId      = kSym;
    cancel.orderId       = 6;
    cancel.participantId = kFirmB;
    assert(client.sendOrderV2(cancel, 0));
    GatewayResponseV2 cancelResp{};
    assert(client.recvResponseV2(cancelResp));
    assert(cancelResp.response.type == GatewayResponse::Type::Ack);
    assert(restingQty(engine, kPrice) == 0);

    gateway.stop();
    engine.stop();
    PASS();
}

// ─── 8: with auth off, nothing changed ──────────────────────────────────────
//
// An unverified participantId is not an identity, so enforcing ownership
// against it would buy no security while breaking clients that never filled
// the field in. Enforcement follows verification.
void test_UnauthenticatedGatewayDoesNotEnforceOwnership() {
    TEST(UnauthenticatedGatewayDoesNotEnforceOwnership);
    MatchingEngine engine;
    engine.start();
    engine.addSymbol(kSym);
    restOrderForFirmA(engine, 7);

    TcpGateway gateway(engine);   // no credential store
    assert(gateway.start(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TestClient client;
    assert(client.connect(gateway.port()));
    client.setRecvTimeout(10);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    OrderRequest cancel{};
    cancel.type          = OrderRequest::Type::Cancel;
    cancel.symbolId      = kSym;
    cancel.orderId       = 7;
    cancel.participantId = 0;     // never set, as an older client would leave it
    assert(client.sendOrderV2(cancel, 0));

    GatewayResponseV2 resp{};
    assert(client.recvResponseV2(resp));
    assert(resp.response.type == GatewayResponse::Type::Ack);
    assert(restingQty(engine, kPrice) == 0);

    gateway.stop();
    engine.stop();
    PASS();
}

}  // namespace

int main() {
    std::cout << "\n=== Order Ownership Tests (H7) ===\n\n";

    test_ForeignCancelIsRefused();
    test_ForeignModifyAndReplaceAreRefused();
    test_DenialIsIndistinguishableFromNotFound();
    test_InternalCallersAreNotScoped();
    test_MissingOrderStillReportsNotFound();
    test_AuthenticatedSessionCannotCancelAnotherFirmsOrder();
    test_AuthenticatedSessionCanCancelItsOwn();
    test_UnauthenticatedGatewayDoesNotEnforceOwnership();

    std::cout << "\n" << passed << " passed\n\n";
    return 0;
}
