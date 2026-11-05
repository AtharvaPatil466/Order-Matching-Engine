// OrderEngineGatewayTest — OrderEngine is the binary clients trade through
// (roadmap 1.2-B2: DOC-7, THR-11).
//
// WHAT WAS WRONG. OrderEngine had a journal and no order-entry port, and the
// separate GatewayServer had the port, a second engine, and only symbols 0 and 1.
// No client order could reach the engine the operator was looking at.
//
// THE FLOW. Against the real binary: log in on the order port, rest an order and
// cross it on symbol 1, read the fill off /book, rest another, and SIGKILL the
// process the instant that last ack arrives. A restart on the same journal must
// show the same book. Then the refusals: a wrong secret closes the connection, an
// order with no login is denied and does not rest, a valid login cannot claim a
// participant its credential does not cover.
//
// Fills are read on /book because the port answers each request and sends nothing
// else: a session hears about its own fills only once roadmap 1.9-I7 lands.
//
// Usage: OrderEngineGatewayTest <path-to-OrderEngine>

#include "GatewayProcess.h"
#include "GatewayTestClient.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace OrderMatcher;
using namespace gwtest;

namespace {

constexpr SymbolId      kSymbol = 1;
constexpr ParticipantId kSeller = 7;
constexpr ParticipantId kBuyer  = 8;
constexpr const char*   kUser   = "firm-a";
constexpr const char*   kSecret = "not-a-real-secret";

constexpr uint16_t kFirstGatewayPort = 31500;
constexpr uint16_t kFirstAdminPort   = 31590;

pid_t g_engine = -1;

// A failed check must not leak a running engine, so no plain assert here.
void require(bool ok, const char* what) {
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    if (g_engine > 0) {
        kill(g_engine, SIGKILL);
        waitpid(g_engine, nullptr, 0);
    }
    std::exit(1);
}

struct Ports { uint16_t gateway; uint16_t admin; };

// Boot on the first free pair of the five starting at `slot`.
Ports boot(const char* bin, int slot, const std::string& journal, const std::string& creds) {
    for (int i = slot; i < slot + 5; ++i) {
        const Ports p{static_cast<uint16_t>(kFirstGatewayPort + i),
                      static_cast<uint16_t>(kFirstAdminPort + i)};
        g_engine = startEngine(bin, p.gateway,
                               {"--journal", journal, "--port", std::to_string(p.admin),
                                "--admin-no-auth", "--participant-credentials", creds,
                                "--symbols", "2", "--threads", "2"});
        if (g_engine > 0) return p;
    }
    require(false, "OrderEngine never opened its order-entry port");
    return {};
}

OrderRequest order(OrderRequest::Type type, OrderId id, ParticipantId who, Side side, Price price,
                   Quantity qty) {
    OrderRequest r{};
    r.type          = type;
    r.symbolId      = kSymbol;
    r.orderId       = id;
    r.participantId = who;
    r.side          = side;
    r.price         = price;
    r.qty           = qty;
    r.orderType     = OrderType::Limit;
    r.tif           = TimeInForce::GTC;
    return r;
}

void connected(TestClient& c, uint16_t port) {
    require(c.connect(port), "could not connect to the order port");
    c.setRecvTimeout(10);
}

void login(TestClient& c) {
    GatewayResponseV2 r{};
    require(c.sendLogin(kUser, kSecret) && c.recvResponseV2(r), "no answer to the login");
    require(r.response.type == GatewayResponse::Type::Ack, "a valid login was refused");
}

void acked(TestClient& c, const OrderRequest& req, const char* what) {
    GatewayResponseV2 r{};
    require(c.sendOrderV2(req, req.orderId) && c.recvResponseV2(r), "no answer to an order");
    if (r.response.type != GatewayResponse::Type::Ack)
        std::fprintf(stderr, "order %llu answered: %s\n",
                     static_cast<unsigned long long>(req.orderId), r.response.errorMessage);
    require(r.response.type == GatewayResponse::Type::Ack, what);
}

std::string book(uint16_t adminPort) {
    return httpGet(adminPort, "/book?symbolId=" + std::to_string(kSymbol));
}

bool has(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

// Rest, cross, see the fill, rest again, kill -9 on the ack.
void tradeThenKill(const char* bin, const std::string& journal, const std::string& creds) {
    const Ports p = boot(bin, 0, journal, creds);
    TestClient c;
    connected(c, p.gateway);
    login(c);

    acked(c, order(OrderRequest::Type::NewOrder, 101, kSeller, Side::Sell, 10000, 100),
          "the resting sell on symbol 1 was refused");
    acked(c, order(OrderRequest::Type::NewOrder, 102, kBuyer, Side::Buy, 10000, 40),
          "the crossing buy on symbol 1 was refused");

    std::string after = book(p.admin);
    for (int i = 0; i < 200 && !has(after, "\"lastTradeQty\":40"); ++i) {
        usleep(10 * 1000);
        after = book(p.admin);
    }
    std::printf("  after the cross: %s\n", after.c_str());
    require(has(after, "\"lastTradePrice\":10000,\"lastTradeQty\":40"), "no fill on symbol 1");
    require(has(after, "\"asks\":[{\"price\":10000,\"qty\":60,"),
            "the resting sell was not reduced by the fill");

    acked(c, order(OrderRequest::Type::NewOrder, 103, kSeller, Side::Buy, 9900, 10),
          "the second resting order was refused");
    kill(g_engine, SIGKILL);
    waitpid(g_engine, nullptr, 0);
    g_engine = -1;
    std::puts("  kill -9 on the ack of order 103");
}

// Everything acknowledged before the kill is back, and only that.
void restartAndCheck(const char* bin, const std::string& journal, const std::string& creds) {
    const Ports p = boot(bin, 5, journal, creds);

    const std::string restored = book(p.admin);
    std::printf("  after restart: %s\n", restored.c_str());
    require(has(restored, "\"bids\":[{\"price\":9900,\"qty\":10,"),
            "an acknowledged resting buy is gone after kill -9");
    require(has(restored, "\"asks\":[{\"price\":10000,\"qty\":60,"),
            "the partly filled sell did not come back at its remaining quantity");

    // A wrong secret is refused and the connection closed.
    {
        TestClient c;
        connected(c, p.gateway);
        GatewayResponseV2 r{};
        require(c.sendLogin(kUser, "wrong-secret") && c.recvResponseV2(r),
                "no answer to a bad login");
        require(r.response.type == GatewayResponse::Type::Error, "a wrong secret logged in");
        require(!c.recvResponseV2(r), "a failed login stayed connected");
    }
    // No login: denied, and it does not rest.
    {
        TestClient c;
        connected(c, p.gateway);
        GatewayResponseV2 r{};
        require(c.sendOrderV2(order(OrderRequest::Type::NewOrder, 200, kBuyer, Side::Buy, 9800, 5),
                              200) && c.recvResponseV2(r),
                "no answer to an order without a login");
        require(r.response.type == GatewayResponse::Type::Error, "an order without a login was acked");
    }
    // Logged in, but claiming a participant the credential does not cover.
    TestClient c;
    connected(c, p.gateway);
    login(c);
    {
        GatewayResponseV2 r{};
        require(c.sendOrderV2(order(OrderRequest::Type::NewOrder, 201, 9, Side::Buy, 9800, 5), 201) &&
                    c.recvResponseV2(r),
                "no answer to an order for an uncovered participant");
        require(r.response.type == GatewayResponse::Type::Error,
                "a credential traded for a participant it does not cover");
    }
    require(book(p.admin) == restored, "a refused order reached the book");

    // The restored order is live: its owner can cancel it.
    acked(c, order(OrderRequest::Type::Cancel, 103, kSeller, Side::Buy, 9900, 10),
          "the restored order's owner could not cancel it");
    std::string final = book(p.admin);
    for (int i = 0; i < 200 && has(final, "\"price\":9900"); ++i) {
        usleep(10 * 1000);
        final = book(p.admin);
    }
    require(!has(final, "\"price\":9900"), "the restored order did not cancel");

    require(stopCleanly(g_engine), "OrderEngine did not exit cleanly on SIGTERM");
    g_engine = -1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: OrderEngineGatewayTest <path-to-OrderEngine>\n");
        return 2;
    }
    const std::string journal = tempJournalPath("ob_engine_gateway");
    const std::string creds = uniqueTempPath("ob_engine_gateway.creds");
    std::remove(journal.c_str());
    {
        std::ofstream out(creds);
        out << kUser << ":" << kSecret << ":" << kSeller << "," << kBuyer << "\n";
    }

    tradeThenKill(argv[1], journal, creds);
    restartAndCheck(argv[1], journal, creds);

    std::remove(journal.c_str());
    std::remove(creds.c_str());
    std::puts("OrderEngineGatewayTest passed");
    return 0;
}
