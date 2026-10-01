// JournalReplayPropertyTest — randomized property test for replay
// determinism.
//
// Property under test:
//
//     For any sequence of operations a MatchingEngine with a journal
//     accepts, a fresh engine that runs MatchingEngine::replayJournal() on
//     that journal holds the same resting orders.
//
// Why: real venues depend on this for crash recovery and for replicating
// state to standby books. If replay diverges from live, every
// after-recovery decision is suspect.
//
// BOTH SIDES ARE THE PRODUCTION CODE. This test used to write the journal
// itself (journal.logAddOrder after each book call) and replay it with its
// own switch over entry types, so it tested a copy of replay that nothing
// ships, against a journal no engine wrote (ENGTEST-7). Now the engine
// journals its own operations and replayJournal() rebuilds the book. Each
// seed runs twice: synchronously, and on the async worker path that both
// binaries run — where CancelReplace, Modify and the expiry sweep
// (OrderRequest::ExpireCheck) are executed and journaled by a worker
// thread (GTS-3).
//
// Operations exercised: AddOrder (Limit / Stop / StopLimit / TrailingStop,
// GTC or GTD), Cancel, Modify, CancelReplace, and expiry sweeps on a virtual
// clock. Stop triggers fire on lastTradePrice_ and expiry is journaled as a
// cancel at its position in the sequence, so replay reproduces both.

#include "Journal.h"
#include "MatchingEngine.h"
#include "OrderBook.h"
#include "TempPath.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using namespace OrderMatcher;

namespace {

constexpr SymbolId kSym = 1;
constexpr int kOps = 200;

enum class Mode { Sync, Async };
const char* modeName(Mode m) { return m == Mode::Sync ? "sync" : "async"; }

struct OrderSnapshot {
    OrderId id;
    ParticipantId participantId;
    SymbolId symbolId;
    Side side;
    Price price;
    Quantity remainingQty;
    OrderType type;
    uint64_t expiryTime;
    bool operator==(const OrderSnapshot& o) const {
        return id == o.id && participantId == o.participantId &&
               symbolId == o.symbolId && side == o.side &&
               price == o.price && remainingQty == o.remainingQty &&
               type == o.type && expiryTime == o.expiryTime;
    }
};

std::vector<OrderSnapshot> snapshotBook(const OrderBook& book) {
    constexpr size_t kMax = 1024;
    std::vector<const Order*> ptrs(kMax);
    size_t n = book.getAllOrders(ptrs.data(), ptrs.size());
    assert(n < kMax && "increase snapshot buffer");
    std::vector<OrderSnapshot> snaps;
    snaps.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const Order* o = ptrs[i];
        snaps.push_back({o->id, o->participantId, o->symbolId, o->side,
                         o->price, o->remainingQty, o->type, o->expiryTime});
    }
    std::sort(snaps.begin(), snaps.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });
    return snaps;
}

void dumpDiff(const std::vector<OrderSnapshot>& live,
              const std::vector<OrderSnapshot>& rep) {
    std::fprintf(stderr,
        "live size=%zu replayed size=%zu\n", live.size(), rep.size());
    size_t n = std::max(live.size(), rep.size());
    for (size_t i = 0; i < n; ++i) {
        if (i < live.size() && i < rep.size() && live[i] == rep[i]) continue;
        auto show = [](const char* who, const std::vector<OrderSnapshot>& v, size_t k) {
            if (k >= v.size()) { std::fprintf(stderr, "%s <none>", who); return; }
            std::fprintf(stderr, "%s id=%llu rem=%llu price=%lld type=%d", who,
                         (unsigned long long)v[k].id, (unsigned long long)v[k].remainingQty,
                         (long long)v[k].price, int(v[k].type));
        };
        std::fprintf(stderr, "  [%zu] ", i);
        show("LIVE", live, i);
        show("  vs  REP", rep, i);
        std::fprintf(stderr, "\n");
    }
}

// The live engine under test, in either mode. In async mode every call
// returns once the worker has processed it, so the generator can read the
// book between operations without racing the worker.
class LiveEngine {
public:
    LiveEngine(const std::string& journalPath, Mode mode, const uint64_t* clock) : mode_(mode) {
        engine_.addSymbol(kSym);
        const bool ok = engine_.enableJournal(journalPath);
        assert(ok && "enableJournal failed");
        (void)ok;
        engine_.setExpiryClock([clock] { return *clock; });
        if (mode_ == Mode::Sync) engine_.start();
        else engine_.startAsync(1, 1024);
    }
    ~LiveEngine() {
        if (mode_ == Mode::Sync) engine_.stop();
        else engine_.stopAsync();
    }

    MatchingEngine& engine() { return engine_; }
    const OrderBook& book() { return *engine_.getOrderBook(kSym); }
    void settle() { if (mode_ == Mode::Async) engine_.waitForDrain(); }

private:
    MatchingEngine engine_;
    Mode mode_;
};

struct Generator {
    std::mt19937_64 rng;
    OrderId nextOrderId = 1;
    std::vector<OrderId> liveIds;

    explicit Generator(uint64_t seed) : rng(seed) {}
    Side side() { return (rng() & 1) ? Side::Buy : Side::Sell; }
    // Narrow grid keeps crosses common but stays within breaker tolerance
    // (default 5%).
    Price price() { return Price(990 + int(rng() % 21)); }
    Quantity qty() { return Quantity(1 + int(rng() % 10)); }
    ParticipantId pid() { return ParticipantId(1 + int(rng() % 4)); }
    OrderId pickLive() { return liveIds[rng() % liveIds.size()]; }
};

void addOrder(LiveEngine& live, Generator& g, uint64_t now) {
    // Skewed toward Limit so trades happen often enough to trigger stops.
    const OrderId id = g.nextOrderId++;
    const uint32_t roll = g.rng() % 10;
    OrderType ot = OrderType::Limit;
    Price stopPrice = 0, stopLimitPrice = 0, trailAmount = 0;
    if (roll == 7) {
        ot = OrderType::Stop;  // becomes a Market on trigger
        stopPrice = g.price();
    } else if (roll == 8) {
        ot = OrderType::StopLimit;
        stopPrice = g.price();
        stopLimitPrice = g.price();
    } else if (roll == 9) {
        ot = OrderType::TrailingStop;
        trailAmount = Price(1 + int(g.rng() % 3));
    }
    // A third of the limits are GTD, expiring a few clock ticks from now.
    const bool gtd = ot == OrderType::Limit && g.rng() % 3 == 0;
    const TimeInForce tif = gtd ? TimeInForce::GTD : TimeInForce::GTC;
    const uint64_t expiry = gtd ? now + 1 + g.rng() % 40 : 0;
    live.engine().submitOrder(kSym, id, g.pid(), g.side(), g.price(), g.qty(), ot, stopPrice,
                              /*displayQty=*/0, tif, expiry, stopLimitPrice, PegType::None,
                              /*pegOffset=*/0, trailAmount);
    live.settle();
    if (live.book().getOrder(id) != nullptr) g.liveIds.push_back(id);
}

void runSeed(uint64_t seed, Mode mode) {
    const std::string journalPath = uniqueTempPath(
        "replay_prop_" + std::to_string(seed) + "_" + modeName(mode) + ".log");
    std::remove(journalPath.c_str());

    std::vector<OrderSnapshot> liveSnap;
    size_t expirySweeps = 0;
    {
        uint64_t now = 1;
        LiveEngine live(journalPath, mode, &now);
        Generator g(seed);

        for (int op = 0; op < kOps; ++op) {
            // Fills and expiry remove orders behind the generator's back.
            g.liveIds.erase(std::remove_if(g.liveIds.begin(), g.liveIds.end(),
                                           [&](OrderId id) { return !live.book().getOrder(id); }),
                            g.liveIds.end());
            // Half adds, so the book keeps enough resting orders for the final
            // comparison to mean something.
            const int kind = g.liveIds.empty() ? 0 : int(g.rng() % 10);

            if (kind < 5) {
                addOrder(live, g, now);
            } else if (kind == 5) {
                live.engine().cancelOrder(kSym, g.pickLive());
            } else if (kind == 6) {
                live.engine().modifyOrder(kSym, g.pickLive(), g.qty());
            } else if (kind < 9) {
                live.engine().cancelReplace(kSym, g.pickLive(), g.price(), g.qty());
            } else {
                now += 1 + g.rng() % 5;
                live.engine().expireOrdersFromClock();  // async: an ExpireCheck request
                ++expirySweeps;
            }
            live.settle();
        }
        liveSnap = snapshotBook(live.book());
    }

    MatchingEngine replayed;
    replayed.addSymbol(kSym);
    const bool ok = replayed.enableJournal(journalPath);
    assert(ok && "enableJournal failed on replay");
    (void)ok;
    replayed.start();
    const size_t entries = replayed.replayJournal();
    const auto repSnap = snapshotBook(*replayed.getOrderBook(kSym));
    replayed.stop();

    if (liveSnap != repSnap) {
        std::fprintf(stderr, "REPLAY DIVERGENCE seed=0x%llx mode=%s ops=%d\n",
                     (unsigned long long)seed, modeName(mode), kOps);
        dumpDiff(liveSnap, repSnap);
        std::abort();
    }

    std::printf("seed=0x%llx %-5s: %zu resting, %zu journal entries, %zu expiry sweeps — match\n",
                (unsigned long long)seed, modeName(mode), liveSnap.size(), entries, expirySweeps);
    std::remove(journalPath.c_str());
}

}  // namespace

int main() {
    for (uint64_t seed : {uint64_t{1}, uint64_t{0xC0FFEE}, uint64_t{0xDEADBEEF},
                          uint64_t{0x123456}, uint64_t{0xABCDEF}}) {
        runSeed(seed, Mode::Sync);
        runSeed(seed, Mode::Async);
    }
    std::puts("JournalReplayPropertyTest passed");
    return 0;
}
