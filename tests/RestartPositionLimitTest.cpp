// RestartPositionLimitTest — the engine-wide position limit (setPositionLimit)
// sees the same position after a restart as the engine that traded.
//
// That position is filled net plus the signed remaining quantity of every
// working order: admission reserves the remainder, a fill accrues to the taker,
// a cancel releases what was left. A restart drives the books directly, so none
// of that ran: the limit started every participant at 0 and admitted a whole
// new position on top of the one held, and cancelling a restored order
// released exposure that was never reserved, moving the position the wrong way.
//
// Each probe runs on the engine as it is, on one restarted from the plain
// journal and on one restarted from a checkpoint; all three must agree.

#include "MatchingEngine.h"
#include "TempPath.h"

#include <cassert>
#include <cstdio>
#include <functional>
#include <string>

using namespace OrderMatcher;

namespace {

constexpr SymbolId kSym = 1;
constexpr Price kPx = 10000;
constexpr ParticipantId kBuyer = 7;
constexpr ParticipantId kSeller = 8;
constexpr int64_t kLimit = 12;

enum class Life { Uninterrupted, Replayed, Checkpointed };
const char* name(Life l) {
    switch (l) {
    case Life::Uninterrupted: return "uninterrupted";
    case Life::Replayed:      return "replayed";
    case Life::Checkpointed:  return "checkpointed";
    }
    return "";
}

void configure(MatchingEngine& engine) {
    engine.addSymbol(kSym);
    engine.setPositionLimit(kBuyer, kLimit);
    engine.setPositionLimit(kSeller, kLimit);
}

void boot(MatchingEngine& engine, const std::string& path) {
    configure(engine);
    assert(engine.enableJournal(path, Journal::SyncPolicy::Immediate));
    engine.start();
}

using Probe = std::function<long long(MatchingEngine&)>;

// The seller offered 10 and 4 of it filled; the buyer bought those 4 and
// still bids 5 lower down. Buyer: 4 filled + 5 working = 9. Seller: 4 filled
// + 6 working = -10.
void traded(MatchingEngine& e) {
    assert(e.submitOrder(kSym, 1, kSeller, Side::Sell, kPx, 10, OrderType::Limit).isAccepted());
    assert(e.submitOrder(kSym, 2, kBuyer, Side::Buy, kPx, 4, OrderType::Limit).isAccepted());
    assert(e.submitOrder(kSym, 3, kBuyer, Side::Buy, kPx - 100, 5, OrderType::Limit).isAccepted());
    assert(e.getOrderBook(kSym)->getOrder(1) && e.getOrderBook(kSym)->getOrder(3));
}

long long after(Life life, const Probe& probe) {
    const std::string path = uniqueTempPath("restart_position_limit.wal");
    std::remove(path.c_str());
    long long result = 0;
    {
        MatchingEngine engine;
        boot(engine, path);
        traded(engine);
        if (life == Life::Uninterrupted) result = probe(engine);
        if (life == Life::Checkpointed) engine.checkpoint();
        engine.stop();
    }
    if (life == Life::Replayed || life == Life::Checkpointed) {
        MatchingEngine restarted;
        boot(restarted, path);
        restarted.replayJournal();
        result = probe(restarted);
        restarted.stop();
    }
    std::remove(path.c_str());
    return result;
}

long long buyerProbe(MatchingEngine& e)  { return e.getPosition(kBuyer); }
long long sellerProbe(MatchingEngine& e) { return e.getPosition(kSeller); }

// Holding 9 against a limit of 12, 4 more (not crossing) is refused.
long long limitProbe(MatchingEngine& e) {
    const SubmitResult r = e.submitOrder(kSym, 50, kBuyer, Side::Buy, kPx - 200, 4, OrderType::Limit);
    return !r.isAccepted() && r.rejectReason == RejectReason::PositionLimitExceeded;
}

// Cancelling the 5 still bid leaves the 4 bought.
long long cancelProbe(MatchingEngine& e) {
    assert(e.submitCancel(kSym, 3).isAccepted());
    return e.getPosition(kBuyer);
}

int check(const char* what, const Probe& probe, long long want) {
    int failures = 0;
    for (Life life : {Life::Uninterrupted, Life::Replayed, Life::Checkpointed}) {
        const long long got = after(life, probe);
        std::printf("  %-13s %-36s %lld%s\n", name(life), what, got, got == want ? "" : "  <- differs");
        failures += got != want;
    }
    return failures;
}

}  // namespace

int main() {
    int failures = 0;
    failures += check("buyer's position", buyerProbe, 9);
    failures += check("seller's position", sellerProbe, -10);
    failures += check("limit 12 refuses 4 more", limitProbe, 1);
    failures += check("buyer's position after cancelling 5", cancelProbe, 4);
    if (failures != 0) {
        std::printf("RestartPositionLimitTest FAILED: %d restarted engine(s) lost the position\n", failures);
        return 1;
    }
    std::puts("RestartPositionLimitTest passed");
    return 0;
}
