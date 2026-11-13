// JournalRecordBeforeEventTest — nothing OrderEngine shows anyone gets ahead of
// the journal record behind it (JOURNAL-4).
//
// The review found that on the async path the maker's fill and the trade's
// market data were dispatched inside the match, before the aggressor's record
// was appended: a crash in that window reversed a fill a counterparty had been
// told about. OrderEngine's wiring (async workers, Immediate journal, TcpGateway
// with execution reports, the market-data listener behind the gateway's taps)
// is reproduced here, and every event the market-data listener receives is
// checked against the journal FILE at the moment it arrives: a trade needs both
// orders' AddOrder records on disk, an order update its order's AddOrder (and
// CancelOrder, for a cancel) record. GatewayExecutionReportTest "durable" pins
// the other half: a record the disk refuses releases nothing.

#include "GatewayTestClient.h"
#include "MatchingEngine.h"
#include "OrderBook.h"
#include "TcpGateway.h"
#include "TempPath.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace OrderMatcher;

namespace {

constexpr SymbolId kSym = 1;
constexpr ParticipantId kMaker = 3, kTaker = 4;
constexpr int kRounds = 20;

bool onDisk(const std::string& path, JournalEntry::Type type, OrderId id) {
    std::ifstream in(path, std::ios::binary);
    in.seekg(sizeof(JournalFileHeader));
    JournalEntry e{};
    while (in.read(reinterpret_cast<char*>(&e), sizeof(e)))
        if (e.entryType == type && e.orderId == id) return true;
    return false;
}

// The feed, as OrderEngine's main installs it: on the book before the gateway
// enables its reports, so the gateway's tap delivers to it.
struct Feed : EventListener {
    std::string path;
    std::atomic<int> events{0};
    std::atomic<int> ahead{0};
    void check(bool ok) {
        events.fetch_add(1);
        if (!ok) ahead.fetch_add(1);
    }
    void onTrade(const Trade& t) override {
        check(onDisk(path, JournalEntry::Type::AddOrder, t.buyOrderId) &&
              onDisk(path, JournalEntry::Type::AddOrder, t.sellOrderId));
    }
    void onOrderUpdate(const OrderUpdate& u) override {
        bool ok = onDisk(path, JournalEntry::Type::AddOrder, u.orderId);
        if (u.status == OrderStatus::Cancelled)
            ok = ok && onDisk(path, JournalEntry::Type::CancelOrder, u.orderId);
        check(ok);
    }
};

OrderRequest limit(ParticipantId p, OrderId id, Side side, Price px, Quantity qty) {
    OrderRequest r{};
    r.type = OrderRequest::Type::NewOrder;
    r.symbolId = kSym;
    r.orderId = id;
    r.participantId = p;
    r.side = side;
    r.price = px;
    r.qty = qty;
    r.orderType = OrderType::Limit;
    r.tif = TimeInForce::GTC;
    return r;
}

// Send and read until the answer; reports on the way are skipped.
bool ack(TestClient& c, const OrderRequest& r) {
    if (!c.sendOrder(r)) return false;
    GatewayResponse resp{};
    while (c.recvResponse(resp)) {
        if (resp.type == GatewayResponse::Type::Ack) return true;
        if (resp.type == GatewayResponse::Type::Error) return false;
    }
    return false;
}

}  // namespace

int main() {
    const std::string path = uniqueTempPath("record_before_event.wal");
    std::filesystem::remove(path);
    Feed feed;
    feed.path = path;
    {
        MatchingEngine engine;
        engine.addSymbol(kSym);
        engine.enableJournal(path, Journal::SyncPolicy::Immediate);
        engine.getOrderBook(kSym)->setEventListener(&feed);
        TcpGateway gateway(engine);
        gateway.enableExecutionReports();
        engine.startAsync(2);
        assert(gateway.start(0));
        TestClient maker, taker;
        assert(maker.connect(gateway.port()) && taker.connect(gateway.port()));
        maker.setRecvTimeout(5);
        taker.setRecvTimeout(5);

        OrderId id = 1;
        for (int round = 0; round < kRounds; ++round) {
            // A maker rests two orders; a taker takes one and part of the
            // other; the maker cancels the rest; then a kill sweeps a third.
            const Price px = 100 + round;
            assert(ack(maker, limit(kMaker, id, Side::Sell, px, 10)));
            assert(ack(maker, limit(kMaker, id + 1, Side::Sell, px, 10)));
            assert(ack(taker, limit(kTaker, id + 2, Side::Buy, px, 15)));
            OrderRequest cancel{};
            cancel.type = OrderRequest::Type::Cancel;
            cancel.symbolId = kSym;
            cancel.orderId = id + 1;
            cancel.participantId = kMaker;
            assert(ack(maker, cancel));
            assert(ack(maker, limit(kMaker, id + 3, Side::Sell, px + 50, 10)));
            OrderRequest kill{};
            kill.type = OrderRequest::Type::KillSwitch;
            kill.participantId = kMaker;
            assert(ack(maker, kill));
            id += 4;
        }
        gateway.stop();
        engine.stopAsync();
    }
    std::filesystem::remove(path);
    std::printf("  %d events reached the feed, %d ahead of their record\n",
                feed.events.load(), feed.ahead.load());
    assert(feed.events.load() > kRounds * 4 && "the feed heard too little to judge");
    assert(feed.ahead.load() == 0 && "an event reached the feed before its journal record");
    std::puts("JournalRecordBeforeEventTest passed");
    return 0;
}
