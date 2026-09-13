// GatewayLoginTest — audit H7, the binary gateway's half.
//
// ParticipantAuthTest covers the credential store in isolation. This covers
// what the gateway does with it.
//
// Before this, TcpGateway had no login frame at all, so with a credential
// store configured the only safe thing it could do was refuse every request.
// That made "authentication required by default" impossible to ship: the one
// configuration that actually served traffic was the one with auth switched
// off. A login frame is the prerequisite, not a nicety.
//
// The frame rides in the V2 header's reserved `flags` word. Zero still means
// "this payload is an OrderRequest", so an existing client is untouched.

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

constexpr ParticipantId kFirmA1 = 100;  // firm-a, first desk
constexpr ParticipantId kFirmA2 = 101;  // firm-a, second desk
constexpr ParticipantId kFirmB  = 200;  // a different firm entirely

constexpr Price kPrice = 1012500;  // toPrice(101.25), used by every order here

// Engine + gateway on an OS-assigned port, with or without credentials.
struct Fixture {
    MatchingEngine  engine;
    ParticipantAuth auth;
    TcpGateway      gateway{engine};

    explicit Fixture(bool withCredentials) {
        engine.start();
        engine.addSymbol(0);
        if (withCredentials) {
            auth.addCredential("firm-a", "s3cret", {kFirmA1, kFirmA2});
            auth.addCredential("firm-b", "0th3r",  {kFirmB});
            gateway.setParticipantAuth(&auth);
        }
        assert(gateway.start(0));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ~Fixture() { gateway.stop(); engine.stop(); }

    // Bounded recv, so a gateway that answers nothing fails the test instead
    // of hanging the whole suite.
    void attach(TestClient& c) {
        assert(c.connect(gateway.port()));
        c.setRecvTimeout(10);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Resting size at kPrice on the bid. Read through getSnapshot(), which
    // takes the book lock — getOrder() documents that it does not, and the
    // engine's worker thread is live here.
    Quantity restingQty() {
        engine.waitForDrain();
        const auto snap =
            engine.getOrderBook(0)->getSnapshot(MarketDataSnapshot::MAX_DEPTH);
        for (size_t i = 0; i < snap.bidCount; ++i) {
            if (snap.bids[i].price == kPrice) return snap.bids[i].totalQuantity;
        }
        return 0;
    }
};

OrderRequest newOrder(OrderId id, ParticipantId pid) {
    OrderRequest req{};
    req.type          = OrderRequest::Type::NewOrder;
    req.symbolId      = 0;
    req.orderId       = id;
    req.participantId = pid;
    req.side          = Side::Buy;
    req.price         = kPrice;
    req.qty           = 10;
    req.orderType     = OrderType::Limit;
    return req;
}

// ─── 1: an order before the login frame is refused ──────────────────────────
//
// The whole point of H7. Reaching the port is not the same as being entitled
// to trade on it.
void test_OrderBeforeLoginIsRefused() {
    TEST(OrderBeforeLoginIsRefused);
    Fixture fx(true);
    TestClient client;
    fx.attach(client);

    assert(client.sendOrderV2(newOrder(1, kFirmA1), 7));
    GatewayResponseV2 resp{};
    assert(client.recvResponseV2(resp));
    assert(resp.response.type == GatewayResponse::Type::Error);
    assert(std::strstr(resp.response.errorMessage, "not logged in") != nullptr);
    assert(resp.header.clientRequestId == 7 && "the reject must still correlate");

    assert(fx.restingQty() == 0 && "a refused order must not reach the engine");
    PASS();
}

// ─── 2: login, then trade as a participant the credential covers ────────────
void test_LoginThenTradeAsPermittedParticipant() {
    TEST(LoginThenTradeAsPermittedParticipant);
    Fixture fx(true);
    TestClient client;
    fx.attach(client);

    assert(client.sendLogin("firm-a", "s3cret", 1));
    GatewayResponseV2 ack{};
    assert(client.recvResponseV2(ack));
    assert(ack.response.type == GatewayResponse::Type::Ack);
    assert(ack.header.clientRequestId == 1);

    // Both of firm-a's desks, on one credential — a firm running two
    // strategies legitimately submits under more than one id.
    for (ParticipantId pid : {kFirmA1, kFirmA2}) {
        assert(client.sendOrderV2(newOrder(10 + pid, pid), pid));
        GatewayResponseV2 resp{};
        assert(client.recvResponseV2(resp));
        assert(resp.response.type == GatewayResponse::Type::Ack);
    }

    assert(fx.restingQty() == 20 && "both orders should be resting");
    PASS();
}

// ─── 3: a logged-in session cannot claim someone else's participant id ──────
//
// Authenticated is not the same as authorised. firm-a proved who it is; that
// says nothing about its right to trade on firm-b's account.
void test_LoggedInSessionCannotClaimAnotherParticipant() {
    TEST(LoggedInSessionCannotClaimAnotherParticipant);
    Fixture fx(true);
    TestClient client;
    fx.attach(client);

    assert(client.sendLogin("firm-a", "s3cret"));
    GatewayResponseV2 ack{};
    assert(client.recvResponseV2(ack));
    assert(ack.response.type == GatewayResponse::Type::Ack);

    assert(client.sendOrderV2(newOrder(2, kFirmB), 0));
    GatewayResponseV2 resp{};
    assert(client.recvResponseV2(resp));
    assert(resp.response.type == GatewayResponse::Type::Error);
    assert(std::strstr(resp.response.errorMessage, "does not cover") != nullptr);

    assert(fx.restingQty() == 0);
    PASS();
}

// ─── 4: the kill switch is covered by the same check ────────────────────────
//
// This is the sharpest edge of H7: KillSwitch acts directly on the participant
// id in the request, so an unchecked one lets any connection halt any firm's
// trading.
void test_KillSwitchRequiresMatchingCredential() {
    TEST(KillSwitchRequiresMatchingCredential);
    Fixture fx(true);
    TestClient client;
    fx.attach(client);

    assert(client.sendLogin("firm-a", "s3cret"));
    GatewayResponseV2 ack{};
    assert(client.recvResponseV2(ack));
    assert(ack.response.type == GatewayResponse::Type::Ack);

    OrderRequest kill{};
    kill.type          = OrderRequest::Type::KillSwitch;
    kill.symbolId      = 0;
    kill.participantId = kFirmB;
    assert(client.sendOrderV2(kill, 0));

    GatewayResponseV2 resp{};
    assert(client.recvResponseV2(resp));
    assert(resp.response.type == GatewayResponse::Type::Error);

    // firm-b must still be able to trade.
    fx.engine.waitForDrain();
    auto ok = fx.engine.submitOrder(0, 500, kFirmB, Side::Buy, kPrice, 5,
                                    OrderType::Limit);
    assert(ok.isAccepted() && "firm-b's trading was halted by another firm");
    PASS();
}

// ─── 5: a bad secret is rejected and the connection is closed ───────────────
//
// One attempt per connection, so an online guessing loop has to pay the accept
// path — and pass the IP allow-list — for every try.
void test_BadSecretRejectsAndClosesConnection() {
    TEST(BadSecretRejectsAndClosesConnection);
    Fixture fx(true);
    TestClient client;
    fx.attach(client);

    assert(client.sendLogin("firm-a", "wrong"));
    GatewayResponseV2 resp{};
    assert(client.recvResponseV2(resp));
    assert(resp.response.type == GatewayResponse::Type::Error);
    assert(std::strstr(resp.response.errorMessage, "login rejected") != nullptr);

    // Unknown user and wrong secret must be indistinguishable, so a probe
    // cannot enumerate who has an account here.
    assert(std::strstr(resp.response.errorMessage, "firm-a") == nullptr);

    // The gateway hangs up: the next read sees EOF rather than another frame.
    GatewayResponseV2 after{};
    assert(!client.recvResponseV2(after) && "connection should be closed");
    PASS();
}

// ─── 6: logging in to a gateway with no credential store is refused ─────────
//
// A client configured to authenticate, talking to a gateway that enforces
// nothing, is a misconfiguration. Surface it at the first connection rather
// than at the first incident.
void test_LoginWithoutCredentialStoreIsRefused() {
    TEST(LoginWithoutCredentialStoreIsRefused);
    Fixture fx(false);
    TestClient client;
    fx.attach(client);

    assert(client.sendLogin("firm-a", "s3cret"));
    GatewayResponseV2 resp{};
    assert(client.recvResponseV2(resp));
    assert(resp.response.type == GatewayResponse::Type::Error);
    assert(std::strstr(resp.response.errorMessage, "no credential store") != nullptr);
    PASS();
}

// ─── 7: an unknown flag bit is rejected, not ignored ────────────────────────
//
// Ignoring a flag it does not understand is how a gateway silently downgrades
// a future authenticated frame into an unauthenticated one.
void test_UnknownFrameFlagsAreRejected() {
    TEST(UnknownFrameFlagsAreRejected);
    Fixture fx(true);
    TestClient client;
    fx.attach(client);

    assert(client.sendOrderWithFlags(newOrder(3, kFirmA1), 1u << 15));

    // decodeFrame() fails before it records a protocol version, so the reject
    // comes back in the V1 shape.
    GatewayResponse resp{};
    assert(client.recvResponse(resp));
    assert(resp.type == GatewayResponse::Type::Error);
    assert(std::strstr(resp.errorMessage, "Unsupported gateway frame flags") != nullptr);
    PASS();
}

// ─── 8: with no credentials configured, nothing changed ─────────────────────
//
// The pre-H7 path. An operator who has not opted in gets exactly the old
// behaviour, including for clients that never send a login frame.
void test_UnauthenticatedGatewayStillServesOrders() {
    TEST(UnauthenticatedGatewayStillServesOrders);
    Fixture fx(false);
    TestClient client;
    fx.attach(client);

    assert(client.sendOrderV2(newOrder(4, kFirmB), 0));
    GatewayResponseV2 resp{};
    assert(client.recvResponseV2(resp));
    assert(resp.response.type == GatewayResponse::Type::Ack);
    assert(fx.restingQty() == 10);
    PASS();
}

}  // namespace

int main() {
    std::cout << "\n=== Gateway Login Tests (H7) ===\n\n";

    test_OrderBeforeLoginIsRefused();
    test_LoginThenTradeAsPermittedParticipant();
    test_LoggedInSessionCannotClaimAnotherParticipant();
    test_KillSwitchRequiresMatchingCredential();
    test_BadSecretRejectsAndClosesConnection();
    test_LoginWithoutCredentialStoreIsRefused();
    test_UnknownFrameFlagsAreRejected();
    test_UnauthenticatedGatewayStillServesOrders();

    std::cout << "\n" << passed << " passed\n\n";
    return 0;
}
