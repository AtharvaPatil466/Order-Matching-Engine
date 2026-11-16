// CheckpointPositionTest — a participant's position and order-to-trade
// counters on a book survive a checkpoint.
//
// They live only in what the journal replays: every fill moves the buyer's and
// seller's position, every admission counts an order, every fill a trade. A
// checkpoint keeps the resting orders and drops the trades, so a book
// restarted from one had a participant at position 0 — maxPositionSize then
// admitted a whole new position on top of the one already held — and counted
// only the orders still resting, which the Snapshot records re-added.
//
// Each probe runs on the book as it is, on one restarted from the plain
// journal and on one restarted from a checkpoint; all three must agree.

#include "MatchingEngine.h"
#include "TempPath.h"

#include <cassert>
#include <cstdio>
#include <functional>
#include <string>
#include <variant>

using namespace OrderMatcher;

namespace {

constexpr SymbolId kSym = 0;
constexpr Price kPx = 10000;
constexpr ParticipantId kBuyer = 7;
constexpr ParticipantId kSeller = 8;

enum class Life { Uninterrupted, Replayed, Checkpointed };
const char* name(Life l) {
    switch (l) {
    case Life::Uninterrupted: return "uninterrupted";
    case Life::Replayed:      return "replayed";
    case Life::Checkpointed:  return "checkpointed";
    }
    return "";
}

void boot(MatchingEngine& engine, const std::string& path) {
    engine.addSymbol(kSym);
    assert(engine.enableJournal(path, Journal::SyncPolicy::Immediate));
    engine.start();
}

using Probe = std::function<long long(OrderBook&)>;

// The buyer bought 10 from the seller, and still bids 5 lower down.
void traded(MatchingEngine& e) {
    assert(e.submitOrder(kSym, 1, kSeller, Side::Sell, kPx, 10, OrderType::Limit).isAccepted());
    assert(e.submitOrder(kSym, 2, kBuyer, Side::Buy, kPx, 10, OrderType::Limit).isAccepted());
    assert(e.submitOrder(kSym, 3, kBuyer, Side::Buy, kPx - 100, 5, OrderType::Limit).isAccepted());
    assert(e.getOrderBook(kSym)->getOrder(3));
}

long long after(Life life, const Probe& probe) {
    const std::string path = uniqueTempPath("checkpoint_position.wal");
    std::remove(path.c_str());
    long long result = 0;
    {
        MatchingEngine engine;
        boot(engine, path);
        traded(engine);
        if (life == Life::Uninterrupted) result = probe(*engine.getOrderBook(kSym));
        if (life != Life::Uninterrupted && life != Life::Replayed) engine.checkpoint();
        engine.stop();
    }
    if (life == Life::Replayed || life == Life::Checkpointed) {
        MatchingEngine restarted;
        boot(restarted, path);
        restarted.replayJournal();
        result = probe(*restarted.getOrderBook(kSym));
        restarted.stop();
    }
    std::remove(path.c_str());
    return result;
}

// Holding 10 against a limit of 10, one more is refused.
long long limitProbe(OrderBook& b) {
    b.setRiskLimits(kBuyer, RiskLimits{0, 0, 10});
    const AddOrderResult r = b.addOrder(50, kBuyer, Side::Buy, kPx - 200, 1, OrderType::Limit);
    return std::holds_alternative<RejectReason>(r) &&
           std::get<RejectReason>(r) == RejectReason::RiskLimitBreached;
}

long long positionProbe(OrderBook& b) { return b.getParticipantStats(kBuyer).netPosition; }
long long sellerProbe(OrderBook& b)   { return b.getParticipantStats(kSeller).netPosition; }
long long ordersProbe(OrderBook& b)   { return static_cast<long long>(b.getParticipantStats(kBuyer).ordersSubmitted); }
long long tradesProbe(OrderBook& b)   { return static_cast<long long>(b.getParticipantStats(kBuyer).tradesExecuted); }

int check(const char* what, const Probe& probe, long long want) {
    int failures = 0;
    for (Life life : {Life::Uninterrupted, Life::Replayed, Life::Checkpointed}) {
        const long long got = after(life, probe);
        std::printf("  %-13s %-34s %lld%s\n", name(life), what, got, got == want ? "" : "  <- differs");
        failures += got != want;
    }
    return failures;
}

}  // namespace

int main() {
    int failures = 0;
    failures += check("buyer's position", positionProbe, 10);
    failures += check("seller's position", sellerProbe, -10);
    failures += check("buyer's orders submitted", ordersProbe, 2);
    failures += check("buyer's trades executed", tradesProbe, 1);
    failures += check("maxPositionSize 10 refuses 1 more", limitProbe, 1);
    if (failures != 0) {
        std::printf("CheckpointPositionTest FAILED: %d restarted book(s) lost participant state\n", failures);
        return 1;
    }
    std::puts("CheckpointPositionTest passed");
    return 0;
}
