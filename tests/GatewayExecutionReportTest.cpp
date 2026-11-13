// GatewayExecutionReportTest — participants hear what happened to their orders
// (review N6 / DA-3, DA-4; roadmap 1.9-I7).
//
// The protocol declared TradeNotification and OrderUpdateNotification and
// nothing sent either: a maker never learned it traded, an aggressor got an Ack
// and nothing else, and an order the book refused after its Ack, or the engine
// cancelled, simply went silent. And OrderEngine published market data from
// the workers before the journal append behind it (DA-4).
//
// Each case runs OrderEngine's shape: an async engine behind TcpGateway with
// enableExecutionReports() called before startAsync, authentication off.

#include "GatewayTestClient.h"
#include "MatchingEngine.h"
#include "OrderBook.h"
#include "TempPath.h"

#include <cassert>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace OrderMatcher;

namespace {

constexpr SymbolId kSym = 0;
constexpr ParticipantId kMaker = 1;
constexpr ParticipantId kTaker = 2;

OrderRequest limit(ParticipantId p, OrderId id, Side side, Price px, Quantity qty,
                   OrderType type = OrderType::Limit) {
    OrderRequest r{};
    r.type = OrderRequest::Type::NewOrder;
    r.symbolId = kSym;
    r.orderId = id;
    r.participantId = p;
    r.side = side;
    r.price = px;
    r.qty = qty;
    r.orderType = type;
    r.tif = TimeInForce::GTC;
    return r;
}

bool isAnswer(const GatewayResponse& r) {
    return r.type == GatewayResponse::Type::Ack || r.type == GatewayResponse::Type::Error;
}

// One session: sends requests, answers come back in order, and every report
// that arrives on the way is kept.
struct Session {
    TestClient c;
    std::vector<GatewayResponse> reports;
    explicit Session(uint16_t port) {
        assert(c.connect(port));
        c.setRecvTimeout(5);
    }
    GatewayResponse request(const OrderRequest& r) {
        assert(c.sendOrder(r));
        GatewayResponse resp{};
        for (;;) {
            assert(c.recvResponse(resp) && "no answer");
            if (isAnswer(resp)) return resp;
            reports.push_back(resp);
        }
    }
    // Read reports until `pred` holds for one of them; false on timeout.
    bool await(const std::function<bool(const GatewayResponse&)>& pred) {
        for (const auto& r : reports)
            if (pred(r)) return true;
        GatewayResponse resp{};
        while (c.recvResponse(resp)) {
            reports.push_back(resp);
            if (pred(resp)) return true;
        }
        return false;
    }
    const GatewayResponse* find(GatewayResponse::Type t, OrderId id, OrderStatus s) const {
        for (const auto& r : reports)
            if (r.type == t && r.orderId == id && r.orderUpdate.status == s) return &r;
        return nullptr;
    }
};

auto reportOf(GatewayResponse::Type t, OrderId id, OrderStatus s) {
    return [=](const GatewayResponse& r) {
        return r.type == t && r.orderId == id && r.orderUpdate.status == s;
    };
}

struct Venue {
    MatchingEngine engine;
    TcpGateway gateway{engine};
    explicit Venue(EventListener* marketData = nullptr) {
        engine.addSymbol(kSym);
        if (marketData) engine.getOrderBook(kSym)->setEventListener(marketData);
        gateway.enableExecutionReports();
        engine.startAsync(2);
        assert(gateway.start(0));
    }
    ~Venue() { gateway.stop(); engine.stopAsync(); }
};

constexpr auto kTrade = GatewayResponse::Type::TradeNotification;
constexpr auto kUpdate = GatewayResponse::Type::OrderUpdateNotification;

// Every fill reaches both owners, with cumulative, leaves and one trade id.
void fillsReachBothSides() {
    Venue v;
    Session maker(v.gateway.port()), taker(v.gateway.port());
    assert(maker.request(limit(kMaker, 1, Side::Sell, 100, 10)).type == GatewayResponse::Type::Ack);
    assert(taker.request(limit(kTaker, 1, Side::Buy, 100, 4)).type == GatewayResponse::Type::Ack);

    assert(taker.await(reportOf(kUpdate, 1, OrderStatus::Filled)) && "the aggressor never heard");
    assert(maker.await(reportOf(kTrade, 1, OrderStatus::PartiallyFilled)) && "the maker never heard");
    const GatewayResponse* t = taker.find(kTrade, 1, OrderStatus::Filled);
    const GatewayResponse* m = maker.find(kTrade, 1, OrderStatus::PartiallyFilled);
    assert(t && m);
    assert(t->trade.tradeId == m->trade.tradeId && t->trade.tradeId != 0);
    assert(t->trade.price == 100 && t->trade.quantity == 4);
    assert(t->orderUpdate.filledQty == 4 && t->orderUpdate.remainingQty == 0);
    assert(m->orderUpdate.filledQty == 4 && m->orderUpdate.remainingQty == 6);
    // Ids are the client's own; the contra side is anonymous.
    assert(t->trade.buyOrderId == 1 && t->trade.sellOrderId == 0 && t->trade.sellerId == 0);
    assert(m->trade.sellOrderId == 1 && m->trade.buyOrderId == 0 && m->trade.buyerId == 0);
    // The taker's reports are its own: nothing about the maker's order.
    for (const auto& r : taker.reports) assert(r.trade.sellerId != kMaker);

    // A second fill accumulates.
    assert(taker.request(limit(kTaker, 2, Side::Buy, 100, 5)).type == GatewayResponse::Type::Ack);
    assert(maker.await([](const GatewayResponse& r) {
        return r.type == kTrade && r.orderId == 1 && r.orderUpdate.filledQty == 9;
    }));
    assert(maker.find(kTrade, 1, OrderStatus::PartiallyFilled));
    std::puts("fillsReachBothSides passed");
}

// Cancels the owner did not send, and a reject after the Ack.
void engineCancelsAndLateRejects() {
    Venue v;
    Session maker(v.gateway.port()), taker(v.gateway.port());
    assert(maker.request(limit(kMaker, 1, Side::Sell, 100, 3)).type == GatewayResponse::Type::Ack);

    // IOC remainder: fills 3, the other 7 are cancelled by the engine.
    assert(taker.request(limit(kTaker, 1, Side::Buy, 100, 10, OrderType::IOC)).type ==
           GatewayResponse::Type::Ack);
    assert(taker.await(reportOf(kUpdate, 1, OrderStatus::Cancelled)) && "IOC remainder unreported");
    assert(taker.find(kUpdate, 1, OrderStatus::Cancelled)->orderUpdate.filledQty == 3);
    assert(maker.await(reportOf(kTrade, 1, OrderStatus::Filled)));

    // Kill switch: the engine cancels the maker's resting order.
    assert(maker.request(limit(kMaker, 2, Side::Sell, 105, 3)).type == GatewayResponse::Type::Ack);
    OrderRequest kill{};
    kill.type = OrderRequest::Type::KillSwitch;
    kill.participantId = kMaker;
    assert(maker.request(kill).type == GatewayResponse::Type::Ack);
    assert(maker.await(reportOf(kUpdate, 2, OrderStatus::Cancelled)) && "kill-switch cancel unreported");

    // A reject decided after the Ack: the id is the maker's own live order.
    assert(maker.request(limit(kMaker, 3, Side::Sell, 110, 3)).type == GatewayResponse::Type::Ack);
    assert(maker.request(limit(kMaker, 3, Side::Sell, 111, 3)).type == GatewayResponse::Type::Ack);
    assert(maker.await(reportOf(kUpdate, 3, OrderStatus::Rejected)) && "post-ack reject unreported");
    assert(maker.find(kUpdate, 3, OrderStatus::Rejected)->rejectReason ==
           RejectReason::DuplicateOrderId);
    std::puts("engineCancelsAndLateRejects passed");
}

// A participant's reports go to each of its sessions, and nowhere else; with
// no session they are counted, not queued.
void routedByParticipant() {
    Venue v;
    Session a1(v.gateway.port()), a2(v.gateway.port()), other(v.gateway.port());
    assert(a1.request(limit(kMaker, 1, Side::Sell, 100, 5)).type == GatewayResponse::Type::Ack);
    assert(a2.request(limit(kMaker, 2, Side::Sell, 101, 5)).type == GatewayResponse::Type::Ack);
    assert(other.request(limit(kTaker, 1, Side::Buy, 90, 5)).type == GatewayResponse::Type::Ack);
    assert(a1.await(reportOf(kUpdate, 2, OrderStatus::Accepted)) && "a second session of one participant");
    // Both sessions now act for kMaker: a fill of order 1 reaches both.
    assert(other.request(limit(kTaker, 2, Side::Buy, 100, 5)).type == GatewayResponse::Type::Ack);
    assert(a1.await(reportOf(kTrade, 1, OrderStatus::Filled)));
    assert(a2.await(reportOf(kTrade, 1, OrderStatus::Filled)));
    for (const auto& r : other.reports)
        assert(r.trade.sellOrderId == 0 && "another participant's order in a report");

    // Participant 9 has no session: its orders' reports are dropped and counted.
    const uint64_t before = v.gateway.reportsUndelivered();
    {
        Session gone(v.gateway.port());
        assert(gone.request(limit(9, 1, Side::Sell, 95, 5)).type == GatewayResponse::Type::Ack);
    }
    // Until the gateway has seen the close, the session still counts.
    for (int i = 0; i < 500 && v.gateway.clientCount() != 3; ++i) usleep(10 * 1000);
    assert(v.gateway.clientCount() == 3);
    assert(other.request(limit(kTaker, 3, Side::Buy, 95, 5)).type == GatewayResponse::Type::Ack);
    assert(other.await(reportOf(kUpdate, 3, OrderStatus::Filled)));
    assert(v.gateway.reportsUndelivered() > before);
    std::puts("routedByParticipant passed");
}

// A session that resets its connection while reports for its orders are on
// the way must not take the gateway with it. On Linux the direct send in
// sendResponse had no MSG_NOSIGNAL, so the write to the reset socket raised
// SIGPIPE and killed the process; macOS hid it behind SO_NOSIGPIPE.
void peerResetDoesNotKillTheGateway() {
    Venue v;
    Session taker(v.gateway.port());
    for (int round = 0; round < 20; ++round) {
        const OrderId base = 1'000 * (round + 1);
        TestClient maker;
        assert(maker.connect(v.gateway.port()));
        maker.setRecvTimeout(5);
        GatewayResponse ack{};
        assert(maker.sendOrder(limit(kMaker, base, Side::Sell, 100, 1'000)) &&
               maker.recvResponse(ack) && ack.type == GatewayResponse::Type::Ack);
        // Reset, not close: SO_LINGER 0 sends RST, so the gateway's next
        // write to this session finds a dead peer.
        const struct linger rst{1, 0};
        setsockopt(maker.fd(), SOL_SOCKET, SO_LINGER, &rst, sizeof(rst));
        maker.disconnect();
        // Each IOC fills against the vanished maker: a report to its session.
        for (int i = 0; i < 50; ++i) {
            const GatewayResponse r =
                taker.request(limit(kTaker, base + i, Side::Buy, 100, 1, OrderType::IOC));
            assert(r.type == GatewayResponse::Type::Ack && "the gateway stopped answering");
        }
        taker.reports.clear();
    }
    std::puts("peerResetDoesNotKillTheGateway passed");
}

// A session that stops reading is disconnected; matching and every other
// session carry on.
void slowConsumerIsDisconnected() {
    Venue v;
    TestClient slow;
    assert(slow.connect(v.gateway.port(), /*rcvBuf=*/4096));
    slow.setRecvTimeout(5);
    OrderRequest big = limit(kMaker, 1, Side::Sell, 100, 1'000'000);
    GatewayResponse ack{};
    assert(slow.sendOrder(big) && slow.recvResponse(ack) && ack.type == GatewayResponse::Type::Ack);
    // From here `slow` reads nothing. Each IOC below is a fill it is told of.
    Session taker(v.gateway.port());
    assert(taker.request(limit(kTaker, 0, Side::Buy, 1, 1)).type == GatewayResponse::Type::Ack);
    assert(v.gateway.clientCount() == 2);
    int sent = 0;
    while (v.gateway.clientCount() == 2 && sent < 50'000) {
        const GatewayResponse r = taker.request(limit(kTaker, 1 + sent, Side::Buy, 100, 1,
                                                      OrderType::IOC));
        assert(r.type == GatewayResponse::Type::Ack && "matching stalled behind a slow reader");
        taker.reports.clear();
        ++sent;
    }
    std::printf("  slow reader disconnected after %d fills\n", sent);
    assert(v.gateway.clientCount() == 1 && "a session that never reads was buffered for forever");
    assert(taker.request(limit(kTaker, 1'000'000, Side::Buy, 100, 1, OrderType::IOC)).type ==
           GatewayResponse::Type::Ack);
    std::puts("slowConsumerIsDisconnected passed");
}

// ── Never ahead of durability ────────────────────────────────────────────────
//
// RLIMIT_FSIZE in a child, as JournalHaltTest: the journal holds the sequence
// lease and the maker's order, and the taker's crossing order is the write it
// refuses. Neither the maker nor the market-data feed may hear of that trade.

enum Code : int { kOk = 0, kSetup = 90, kMakerTold, kFeedTold, kNotWithheld };

bool limitFileSize(rlim_t bytes) {
    std::signal(SIGXFSZ, SIG_IGN);
    struct rlimit rl{bytes, bytes};
    return setrlimit(RLIMIT_FSIZE, &rl) == 0;
}

struct Feed : EventListener {
    std::atomic<int> trades{0};
    std::atomic<int> depth{0};
    void onTrade(const Trade&) override { trades.fetch_add(1); }
    void onMarketData(const MarketDataUpdate&) override { depth.fetch_add(1); }
};

int refusedWriteChild(const std::string& path) {
    constexpr int kWholeRecords = 2;   // the sequence lease, the maker's order
    if (!limitFileSize(sizeof(JournalFileHeader) + kWholeRecords * sizeof(JournalEntry) +
                       sizeof(JournalEntry) / 2))
        return kSetup;
    MatchingEngine engine;
    engine.addSymbol(kSym);
    engine.enableJournal(path, Journal::SyncPolicy::Immediate);
    Feed feed;
    engine.getOrderBook(kSym)->setEventListener(&feed);   // as OrderEngine's main does
    TcpGateway gateway(engine);
    gateway.enableExecutionReports();
    engine.startAsync(1);
    if (!gateway.start(0)) return kSetup;
    {
        Session maker(gateway.port()), taker(gateway.port());
        if (maker.request(limit(kMaker, 1, Side::Sell, 100, 10)).type != GatewayResponse::Type::Ack)
            return kSetup;
        engine.waitForDrain();
        maker.c.setRecvTimeout(1);
        maker.await(reportOf(kUpdate, 1, OrderStatus::Accepted));   // the durable rest
        const int depthBefore = feed.depth.load();
        if (taker.request(limit(kTaker, 1, Side::Buy, 100, 10)).type == GatewayResponse::Type::Ack)
            return kSetup;   // JournalHaltTest's concern; here it only sets the scene
        if (feed.trades.load() != 0 || feed.depth.load() != depthBefore) return kFeedTold;
        if (maker.await(reportOf(kTrade, 1, OrderStatus::Filled))) return kMakerTold;
        if (gateway.reportsWithheld() == 0) return kNotWithheld;
    }
    gateway.stop();
    engine.stopAsync();
    return kOk;
}

void nothingAheadOfTheJournal() {
    const std::string path = uniqueTempPath("gateway_reports_halt.wal");
    std::filesystem::remove(path);
    std::fflush(nullptr);
    const pid_t pid = fork();
    if (pid == 0) _exit(refusedWriteChild(path));
    int status = 0;
    waitpid(pid, &status, 0);
    std::filesystem::remove(path);
    const int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    std::printf("  refused write: child exit %d\n", rc);
    assert(rc != kMakerTold && "the maker was told of a fill the journal refused");
    assert(rc != kFeedTold && "the feed published a trade the journal refused (DA-4)");
    assert(rc == kOk);
    std::puts("nothingAheadOfTheJournal passed");
}

}  // namespace

// An argument names one case, to run it alone.
int main(int argc, char** argv) {
    const std::string only = argc > 1 ? argv[1] : "";
    if (only.empty() || only == "fills") fillsReachBothSides();
    if (only.empty() || only == "cancels") engineCancelsAndLateRejects();
    if (only.empty() || only == "routing") routedByParticipant();
    if (only.empty() || only == "reset") peerResetDoesNotKillTheGateway();
    if (only.empty() || only == "slow") slowConsumerIsDisconnected();
    if (only.empty() || only == "durable") nothingAheadOfTheJournal();
    std::puts("GatewayExecutionReportTest passed");
    return 0;
}
