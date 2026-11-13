// CrashRecoveryE2ETest — kill -9 the real order-entry binary at a random
// point in a stream of orders and cancels, restart it on the same journal, and
// diff what it holds against what it acknowledged (roadmap 1.3-C4, 2.8).
//
// WHY THE BINARY. JRN-1 (the journal was never replayed at boot) and JRN-3
// (acks went out while the entry sat in a user-space batch) both survived
// every in-process test, because each was about what the shipped binary does,
// not about what a function can do. OrderEngine is the binary that takes orders
// over TCP and journals them.
//
// THE FLOW. One session pipelines a seeded stream of adds and cancels with up
// to kWindow requests in flight, and the process is SIGKILLed the instant the
// K-th answer arrives (K random), so the kill lands with requests still
// unanswered. After a restart on the same journal, cancelling through the order
// port and reading /book:
//   - every order whose add was ACKED and that no cancel was sent for must
//     be there (cancelling it takes an order off the book);
//   - every order whose cancel was ACKED must still be gone (cancelling it takes
//     nothing off) — an acked cancel must not come back.
// Requests that were in flight at the kill may land either way.
//
// Adds are all buys below one another, so nothing trades and presence is the
// whole state. They sit on ten price levels, all that /book lists.
//
// THE CROSSING ROUND (N6/DA-3). Two participants on one session send sells and
// buys at one price, so nearly every order trades, and the process is
// SIGKILLed mid-stream the same way. Every fill a TradeNotification told the
// session about must be in the journal: replaying it rebuilds a trade with the
// same trade id, price and quantity. A report sent ahead of its entry's fsync
// can name a trade the replay never makes; with the reports' wiring deleted
// from OrderEngine's main, the round is told of no fill and fails too.
//
// Usage: CrashRecoveryE2ETest <path-to-OrderEngine>

#include "GatewayProcess.h"

#include <cassert>
#include <cstdio>
#include <deque>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace OrderMatcher;
using namespace gwtest;

namespace {

constexpr ParticipantId kOwner = 7;
constexpr SymbolId kSymbol = 0;
constexpr int kOps = 80;
constexpr int kWindow = 8;  // requests in flight when the kill lands

struct Op {
    OrderRequest::Type type;
    OrderId id;
};

struct Fate {
    bool addAcked = false;
    bool cancelSent = false;
    bool cancelAcked = false;
};

OrderRequest request(const Op& op) {
    OrderRequest r{};
    r.type = op.type;
    r.symbolId = kSymbol;
    r.orderId = op.id;
    r.participantId = kOwner;
    r.side = Side::Buy;
    r.price = 10000 - static_cast<Price>(op.id % 10);
    r.qty = 10;
    r.orderType = OrderType::Limit;
    r.tif = TimeInForce::GTC;
    return r;
}

// Seventy percent adds of fresh ids; the rest cancel an earlier add.
std::vector<Op> generate(std::mt19937& rng, OrderId firstId) {
    std::vector<Op> ops;
    std::vector<OrderId> cancellable;
    OrderId next = firstId;
    for (int i = 0; i < kOps; ++i) {
        if (!cancellable.empty() && rng() % 10 < 3) {
            const size_t k = rng() % cancellable.size();
            ops.push_back({OrderRequest::Type::Cancel, cancellable[k]});
            cancellable.erase(cancellable.begin() + static_cast<long>(k));
        } else {
            ops.push_back({OrderRequest::Type::NewOrder, next});
            cancellable.push_back(next++);
        }
    }
    return ops;
}

// Stream `ops`, SIGKILL the engine once `killAfter` answers are in, and
// record which adds and cancels were acknowledged before that.
void streamThenKill(pid_t pid, uint16_t port, const std::vector<Op>& ops, int killAfter,
                    std::vector<Fate>& fates, OrderId firstId) {
    Client c;
    assert(c.connect(port));
    std::deque<const Op*> inFlight;
    size_t sent = 0;
    int answered = 0;
    while (answered < killAfter) {
        while (sent < ops.size() && inFlight.size() < static_cast<size_t>(kWindow)) {
            const Op& op = ops[sent++];
            if (op.type == OrderRequest::Type::Cancel) fates[op.id - firstId].cancelSent = true;
            assert(c.send(request(op)));
            inFlight.push_back(&op);
        }
        const Op& op = *inFlight.front();
        inFlight.pop_front();
        GatewayResponse r{};
        assert(c.answerFor(op.id, r) && "no answer from the gateway before the kill");
        const bool acked = r.type == GatewayResponse::Type::Ack;
        Fate& f = fates[op.id - firstId];
        if (op.type == OrderRequest::Type::NewOrder) {
            assert(acked && "a fresh resting buy was refused");
            f.addAcked = true;
        } else {
            f.cancelAcked = acked;
        }
        ++answered;
    }
    kill(pid, SIGKILL);  // the instant the answer is in hand
    waitpid(pid, nullptr, 0);
    std::printf("  kill -9 after %d answers, %zu requests still in flight\n", answered,
                inFlight.size());
}

// Cancel as the probe: /book losing an order means the order was there.
bool present(Client& c, uint16_t adminPort, OrderId id) {
    return cancelRemoved(c, adminPort, request({OrderRequest::Type::Cancel, id}));
}

void crashRound(const char* bin, uint16_t& port, uint32_t seed) {
    const std::string journal = tempJournalPath("ob_crash_e2e_" + std::to_string(seed));
    std::remove(journal.c_str());
    std::mt19937 rng(seed);
    const OrderId firstId = 1000 * seed;
    const std::vector<Op> ops = generate(rng, firstId);
    const int killAfter = 10 + static_cast<int>(rng() % (kOps - 20));
    std::vector<Fate> fates(kOps + 1);

    pid_t pid = startOnFreePort(bin, port, journal);
    assert(pid > 0 && "OrderEngine never started listening");
    streamThenKill(pid, port, ops, killAfter, fates, firstId);

    ++port;
    pid = startOnFreePort(bin, port, journal);
    assert(pid > 0 && "OrderEngine did not restart on its own journal");
    const uint16_t admin = static_cast<uint16_t>(port + kAdminPortOffset);
    int mustBeThere = 0, mustBeGone = 0, lost = 0, resurrected = 0;
    {
        Client c;
        assert(c.connect(port));
        for (size_t k = 0; k < fates.size(); ++k) {
            const Fate& f = fates[k];
            const OrderId id = firstId + k;
            if (f.addAcked && !f.cancelSent) {
                ++mustBeThere;
                if (!present(c, admin, id)) {
                    ++lost;
                    std::printf("  LOST: order %llu was acknowledged\n", (unsigned long long)id);
                }
            } else if (f.cancelAcked) {
                ++mustBeGone;
                if (present(c, admin, id)) {
                    ++resurrected;
                    std::printf("  RESURRECTED: order %llu's cancel was acknowledged\n",
                                (unsigned long long)id);
                }
            }
        }
    }
    assert(stopCleanly(pid));
    std::remove(journal.c_str());
    std::printf("  seed %u: %d acked orders checked present, %d acked cancels checked gone\n",
                seed, mustBeThere, mustBeGone);
    assert(mustBeThere > 0 && mustBeGone > 0 && "the round checked nothing");
    assert(lost == 0 && "an order the gateway acknowledged did not survive kill -9");
    assert(resurrected == 0 && "a cancel the gateway acknowledged was undone by kill -9");
    ++port;
}

struct Told { Price price; Quantity qty; };

void crossingRound(const char* bin, uint16_t& port, uint32_t seed) {
    constexpr ParticipantId kSeller = 7, kBuyer = 8;
    const std::string journal = tempJournalPath("ob_crash_cross_" + std::to_string(seed));
    std::remove(journal.c_str());
    std::mt19937 rng(seed);
    const int killAfter = 10 + static_cast<int>(rng() % (kOps - 20));

    pid_t pid = startOnFreePort(bin, port, journal);
    assert(pid > 0 && "OrderEngine never started listening");
    std::map<uint64_t, Told> told;   // trade id -> what the session was told
    {
        Client c;
        assert(c.connect(port));
        size_t sent = 0, inFlight = 0;
        int answered = 0;
        while (answered < killAfter) {
            while (sent < static_cast<size_t>(kOps) && inFlight < static_cast<size_t>(kWindow)) {
                OrderRequest r{};
                r.type = OrderRequest::Type::NewOrder;
                r.symbolId = kSymbol;
                r.orderId = sent + 1;
                const bool sell = sent % 2 == 0;
                r.participantId = sell ? kSeller : kBuyer;
                r.side = sell ? Side::Sell : Side::Buy;
                r.price = 10000;
                r.qty = 1 + rng() % 10;
                r.orderType = OrderType::Limit;
                r.tif = TimeInForce::GTC;
                assert(c.send(r));
                ++sent;
                ++inFlight;
            }
            GatewayResponse f{};
            assert(c.next(f) && "the gateway went quiet before the kill");
            if (f.type == GatewayResponse::Type::TradeNotification) {
                told[f.trade.tradeId] = Told{f.trade.price, f.trade.quantity};
                continue;
            }
            if (f.type == GatewayResponse::Type::OrderUpdateNotification) continue;
            assert(f.type == GatewayResponse::Type::Ack && "a crossing order was refused");
            --inFlight;
            ++answered;
        }
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
    }
    ++port;

    // Replay the journal the way every boot does, and read back its trades.
    MatchingEngine replayed;
    replayed.addSymbol(0);
    replayed.addSymbol(1);
    replayed.enableJournal(journal);
    replayed.replayJournal();
    // Single-threaded here, so draining the book's trade ring is safe.
    auto& ring = const_cast<RingBuffer<Trade>&>(
        replayed.getOrderBook(kSymbol)->getTradeRingBuffer());
    std::map<uint64_t, Told> rebuilt;
    for (Trade t{}; ring.pop(t);) rebuilt[t.tradeId] = Told{t.price, t.quantity};
    std::remove(journal.c_str());

    int missing = 0;
    for (const auto& [id, t] : told) {
        const auto it = rebuilt.find(id);
        if (it == rebuilt.end() || it->second.price != t.price || it->second.qty != t.qty) {
            ++missing;
            std::printf("  MISSING: trade %llu (%lld x %llu) was reported, replay has %s\n",
                        (unsigned long long)id, (long long)t.price, (unsigned long long)t.qty,
                        it == rebuilt.end() ? "no such trade" : "a different one");
        }
    }
    std::printf("  seed %u: %zu reported fills checked against %zu replayed trades\n", seed,
                told.size(), rebuilt.size());
    assert(!told.empty() && "the session was told of no fill: are execution reports wired?");
    assert(missing == 0 && "a fill reported to a client is not in the journal after kill -9");
}

}  // namespace

int main(int argc, char** argv) {
    assert(argc == 2 && "usage: CrashRecoveryE2ETest <path-to-OrderEngine>");
    uint16_t port = 31540;
    for (uint32_t seed : {1u, 2u, 3u}) crashRound(argv[1], port, seed);
    for (uint32_t seed : {4u, 5u, 6u}) crossingRound(argv[1], port, seed);
    std::puts("CrashRecoveryE2ETest passed");
    return 0;
}
