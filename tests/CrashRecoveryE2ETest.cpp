// CrashRecoveryE2ETest — kill -9 the real order-entry binary at a random
// point in a stream of orders and cancels, restart it on the same journal, and
// diff what it holds against what it acknowledged (roadmap 1.3-C4, 2.8).
//
// WHY THE BINARY. JRN-1 (the journal was never replayed at boot) and JRN-3
// (acks went out while the entry sat in a user-space batch) both survived
// every in-process test, because each was about what the shipped binary does,
// not about what a function can do. GatewayServer is the binary that takes
// orders over TCP and journals them; OrderEngine has no order-entry port.
//
// THE FLOW. One session pipelines a seeded stream of adds and cancels with up
// to kWindow requests in flight, and the process is SIGKILLed the instant the
// K-th answer arrives (K random), so the kill lands with requests still
// unanswered. After a restart on the same journal, through the order port:
//   - every order whose add was ACKED and that no cancel was sent for must
//     be there (a cancel for it is acknowledged);
//   - every order whose cancel was ACKED must still be gone (a cancel for it
//     is refused with OrderNotFound) — an acked cancel must not come back.
// Requests that were in flight at the kill may land either way.
//
// Adds are all buys below one another, so nothing trades and presence is the
// whole state. Quantity after a modify is not observable through this port.
//
// Usage: CrashRecoveryE2ETest <path-to-GatewayServer>

#include "GatewayProcess.h"

#include <cassert>
#include <cstdio>
#include <deque>
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
    r.price = 10000 - static_cast<Price>(op.id % 50);
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

// Stream `ops`, SIGKILL the gateway once `killAfter` answers are in, and
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

// Cancel answers as the probe: Ack means the order was there.
bool present(Client& c, OrderId id) {
    assert(c.send(request({OrderRequest::Type::Cancel, id})));
    GatewayResponse r{};
    assert(c.answerFor(id, r) && "no answer to the probe");
    if (r.type == GatewayResponse::Type::Ack) return true;
    assert(r.rejectReason == RejectReason::OrderNotFound && "probe refused for another reason");
    return false;
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
    assert(pid > 0 && "GatewayServer never started listening");
    streamThenKill(pid, port, ops, killAfter, fates, firstId);

    ++port;
    pid = startOnFreePort(bin, port, journal);
    assert(pid > 0 && "GatewayServer did not restart on its own journal");
    int mustBeThere = 0, mustBeGone = 0, lost = 0, resurrected = 0;
    {
        Client c;
        assert(c.connect(port));
        for (size_t k = 0; k < fates.size(); ++k) {
            const Fate& f = fates[k];
            const OrderId id = firstId + k;
            if (f.addAcked && !f.cancelSent) {
                ++mustBeThere;
                if (!present(c, id)) {
                    ++lost;
                    std::printf("  LOST: order %llu was acknowledged\n", (unsigned long long)id);
                }
            } else if (f.cancelAcked) {
                ++mustBeGone;
                if (present(c, id)) {
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

}  // namespace

int main(int argc, char** argv) {
    assert(argc == 2 && "usage: CrashRecoveryE2ETest <path-to-GatewayServer>");
    uint16_t port = 48500;
    for (uint32_t seed : {1u, 2u, 3u}) crashRound(argv[1], port, seed);
    std::puts("CrashRecoveryE2ETest passed");
    return 0;
}
