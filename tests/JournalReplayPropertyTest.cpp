// JournalReplayPropertyTest — randomized property test for replay
// determinism.
//
// Property under test:
//
//     For any sequence of order-mutating operations applied to a live
//     OrderBook with Journal recording, replaying that journal into a
//     fresh OrderBook produces a state byte-for-byte equivalent to the
//     live book at the same point.
//
// Why: real venues depend on this for crash recovery and for replicating
// state to standby books. If replay diverges from live, every
// after-recovery decision is suspect. The hardcoded testJournalReplay in
// ManualTest.cpp covers a 4-operation sequence; this test sweeps random
// sequences across many seeds to find divergence the hardcoded test
// can't see.
//
// Operations exercised: AddOrder (Limit / Stop / StopLimit /
// TrailingStop), Cancel, Modify, CancelReplace.
//
// Why these stop-family types belong in the replay test even though
// they're "time-dependent": their triggers fire on lastTradePrice_, not
// on wall-clock time. Replay re-runs every operation in order, so the
// sequence of trades — and therefore the sequence of stop triggers —
// is the same across live and replay runs.
//
// Still excluded: expiring TIFs (Day, GTD). Those depend on real-time
// `now` via expireOrders() and would need a virtual clock for
// deterministic replay. Out of scope here.

#include "Journal.h"
#include "OrderBook.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

using namespace OrderMatcher;
namespace fs = std::filesystem;

namespace {

struct OrderSnapshot {
    OrderId id;
    ParticipantId participantId;
    SymbolId symbolId;
    Side side;
    Price price;
    Quantity remainingQty;
    OrderType type;
    bool operator==(const OrderSnapshot& o) const {
        return id == o.id && participantId == o.participantId &&
               symbolId == o.symbolId && side == o.side &&
               price == o.price && remainingQty == o.remainingQty &&
               type == o.type;
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
                         o->price, o->remainingQty, o->type});
    }
    std::sort(snaps.begin(), snaps.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });
    return snaps;
}

void dumpDiff(const std::vector<OrderSnapshot>& live,
              const std::vector<OrderSnapshot>& rep) {
    std::fprintf(stderr,
        "live size=%zu replayed size=%zu\n", live.size(), rep.size());
    size_t n = std::min(live.size(), rep.size());
    for (size_t i = 0; i < n; ++i) {
        if (!(live[i] == rep[i])) {
            std::fprintf(stderr,
                "  [%zu] LIVE id=%llu rem=%llu price=%lld type=%d  vs  "
                "REP id=%llu rem=%llu price=%lld type=%d\n",
                i,
                (unsigned long long)live[i].id,
                (unsigned long long)live[i].remainingQty,
                (long long)live[i].price, int(live[i].type),
                (unsigned long long)rep[i].id,
                (unsigned long long)rep[i].remainingQty,
                (long long)rep[i].price, int(rep[i].type));
        }
    }
}

void runSeed(uint64_t seed) {
    std::mt19937_64 rng(seed);
    auto journalPath = fs::temp_directory_path() /
        ("replay_prop_" + std::to_string(::getpid()) + "_" +
         std::to_string(seed) + ".log");
    fs::remove(journalPath);

    constexpr SymbolId kSym = 1;
    constexpr int kOps = 200;

    std::vector<OrderId> liveIds;       // currently in book per simulation
    OrderId nextOrderId = 1;

    OrderBook live(kSym);
    Journal journal(journalPath.string(), Journal::SyncPolicy::Immediate, 1);

    auto pickSide  = [&]() { return (rng() & 1) ? Side::Buy : Side::Sell; };
    auto pickPrice = [&]() {
        // Narrow grid keeps crosses common but stays within breaker
        // tolerance (default 5%). Reference price establishes around 1000.
        return Price(990 + int(rng() % 21));
    };
    auto pickQty   = [&]() { return Quantity(1 + int(rng() % 10)); };
    auto pickPid   = [&]() { return ParticipantId(1 + int(rng() % 4)); };

    for (int op = 0; op < kOps; ++op) {
        int kind = int(rng() % 4);

        // Bias toward AddOrder when book is small so operations have
        // targets to act on; otherwise distribute uniformly.
        if (liveIds.empty()) kind = 0;

        if (kind == 0) {
            // AddOrder — pick from {Limit, Stop, StopLimit, TrailingStop}
            // with weights skewed toward Limit so trades happen often
            // enough to actually trigger stops.
            OrderId id = nextOrderId++;
            ParticipantId pid = pickPid();
            Side side = pickSide();
            Price price = pickPrice();
            Quantity qty = pickQty();
            uint32_t roll = rng() % 10;
            OrderType ot;
            Price stopPrice = 0;
            Price stopLimitPrice = 0;
            Price trailAmount = 0;
            if (roll < 7) {
                ot = OrderType::Limit;
            } else if (roll == 7) {
                ot = OrderType::Stop;
                // Stop trigger near current price grid; Stop becomes a
                // Market on trigger.
                stopPrice = pickPrice();
            } else if (roll == 8) {
                ot = OrderType::StopLimit;
                stopPrice = pickPrice();
                stopLimitPrice = pickPrice();
            } else {
                ot = OrderType::TrailingStop;
                // Small trail (1-3 ticks).
                trailAmount = Price(1 + int(rng() % 3));
            }
            auto result = live.addOrder(id, pid, side, price, qty, ot,
                                         stopPrice, /*displayQty=*/0,
                                         TimeInForce::GTC, /*expiry=*/0,
                                         stopLimitPrice,
                                         PegType::None, /*pegOffset=*/0,
                                         trailAmount);
            if (std::holds_alternative<OrderId>(result)) {
                journal.logAddOrder(id, pid, kSym, side, price, qty, ot,
                                     TimeInForce::GTC, /*expiry=*/0,
                                     stopPrice, stopLimitPrice,
                                     /*displayQty=*/0, PegType::None,
                                     /*pegOffset=*/0, trailAmount);
                if (live.getOrder(id) != nullptr) {
                    liveIds.push_back(id);
                }
            }
            // Sync liveIds with reality — fills can have removed other
            // resting orders. Walk book once a while to refresh.
            if ((op & 15) == 0) {
                liveIds.clear();
                auto snap = snapshotBook(live);
                for (auto& s : snap) liveIds.push_back(s.id);
            }
        } else if (kind == 1 && !liveIds.empty()) {
            // Cancel a random existing order
            size_t i = rng() % liveIds.size();
            OrderId id = liveIds[i];
            if (live.getOrder(id) != nullptr) {
                live.cancelOrder(id);
                journal.logCancelOrder(id);
            }
            liveIds.erase(liveIds.begin() + i);
        } else if (kind == 2 && !liveIds.empty()) {
            // Modify (decrease qty)
            size_t i = rng() % liveIds.size();
            OrderId id = liveIds[i];
            const Order* o = live.getOrder(id);
            if (o && o->remainingQty > 1) {
                Quantity newQty = pickQty();
                if (newQty < o->remainingQty) {
                    if (live.modifyOrder(id, newQty)) {
                        journal.logModifyOrder(id, newQty);
                    }
                }
            }
        } else if (kind == 3 && !liveIds.empty()) {
            // CancelReplace
            size_t i = rng() % liveIds.size();
            OrderId id = liveIds[i];
            const Order* o = live.getOrder(id);
            if (o) {
                Price newPrice = pickPrice();
                Quantity newQty = pickQty();
                if (live.cancelReplace(id, newPrice, newQty)) {
                    journal.logCancelReplace(id, newPrice, newQty);
                }
            }
        }
    }

    journal.flush();

    // Snapshot the live book.
    auto liveSnap = snapshotBook(live);

    // Replay into a fresh book with replayMode on.
    OrderBook replayed(kSym);
    replayed.setReplayMode(true);
    Journal replayJ(journalPath.string());
    auto entries = replayJ.readAll(/*validateCRC=*/true,
                                    /*validateSequence=*/true);

    for (const auto& e : entries) {
        switch (e.entryType) {
        case JournalEntry::Type::AddOrder:
            replayed.addOrder(e.orderId, e.participantId, e.side, e.price,
                              e.quantity, e.orderType, e.stopPrice,
                              e.displayQty, e.timeInForce, e.expiryTime,
                              e.stopLimitPrice, e.pegType, e.pegOffset,
                              e.trailAmount, e.minQty, e.hidden);
            break;
        case JournalEntry::Type::CancelOrder:
            replayed.cancelOrder(e.orderId);
            break;
        case JournalEntry::Type::ModifyOrder:
            replayed.modifyOrder(e.orderId, e.newQty);
            break;
        case JournalEntry::Type::CancelReplace:
            replayed.cancelReplace(e.orderId, e.newPrice, e.newQty);
            break;
        case JournalEntry::Type::Snapshot:
            replayed.addOrder(e.orderId, e.participantId, e.side, e.price,
                              e.quantity, e.orderType);
            break;
        }
    }

    auto repSnap = snapshotBook(replayed);

    if (liveSnap != repSnap) {
        std::fprintf(stderr, "REPLAY DIVERGENCE seed=0x%llx ops=%d\n",
                     (unsigned long long)seed, kOps);
        dumpDiff(liveSnap, repSnap);
        std::abort();
    }

    std::printf("seed=0x%llx: %zu live entries, %zu journal entries — match\n",
                (unsigned long long)seed, liveSnap.size(), entries.size());

    fs::remove(journalPath);
}

}  // namespace

int main() {
    for (uint64_t seed : {uint64_t{1}, uint64_t{0xC0FFEE}, uint64_t{0xDEADBEEF},
                          uint64_t{0x123456}, uint64_t{0xABCDEF}}) {
        runSeed(seed);
    }
    std::puts("JournalReplayPropertyTest passed");
    return 0;
}
