// JournalFollowerTest — verifies that a JournalFollower running against
// a leader's journal converges the follower book to byte-identical state
// with the leader.
//
// This is the empirical complement to spec/EngineConsumer.tla — that
// proved internal consumer-loop correctness; this proves the external
// log-shipping replication primitive lands every leader-side mutation
// on the follower side, in order.
//
// Two scenarios:
//   1. Sequential: leader writes everything, then we poll the follower
//      once. Tests the catch-up path (simulates promoting a stale
//      replica that's been offline).
//   2. Concurrent: follower polls in a thread while the leader writes.
//      Tests the steady-state replication path.

#include "Journal.h"
#include "JournalFollower.h"
#include "OrderBook.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace OrderMatcher;
namespace fs = std::filesystem;

namespace {

// Field-complete by design. This used to compare six fields — id,
// participantId, side, price, remainingQty, type — which made it blind to
// exactly the ten the journal's Snapshot record carries and the follower used
// to drop on restore (timeInForce, expiryTime, stopPrice, stopLimitPrice,
// displayQty, pegType, pegOffset, trailAmount, minQty, hidden). A GTD iceberg
// coming back as a plain GTC limit compared EQUAL. Every convergence assertion
// in this file runs through here, so the comparator has to see everything a
// JournalEntry can carry, or "converged" means less than it claims.
struct OrderSnapshot {
    OrderId id;
    ParticipantId participantId;
    Side side;
    Price price;
    Quantity remainingQty;
    OrderType type;
    TimeInForce timeInForce;
    uint64_t expiryTime;
    Price stopPrice;
    Price stopLimitPrice;
    Quantity displayQty;
    PegType pegType;
    Price pegOffset;
    Price trailAmount;
    Quantity minQty;
    bool hidden;

    bool operator==(const OrderSnapshot& o) const {
        return id == o.id && participantId == o.participantId &&
               side == o.side && price == o.price &&
               remainingQty == o.remainingQty && type == o.type &&
               timeInForce == o.timeInForce && expiryTime == o.expiryTime &&
               stopPrice == o.stopPrice && stopLimitPrice == o.stopLimitPrice &&
               displayQty == o.displayQty && pegType == o.pegType &&
               pegOffset == o.pegOffset && trailAmount == o.trailAmount &&
               minQty == o.minQty && hidden == o.hidden;
    }
};

std::vector<OrderSnapshot> snapshotBook(const OrderBook& book) {
    constexpr size_t kMax = 4096;
    std::vector<const Order*> ptrs(kMax);
    size_t n = book.getAllOrders(ptrs.data(), ptrs.size());
    std::vector<OrderSnapshot> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const Order* o = ptrs[i];
        out.push_back({o->id, o->participantId, o->side, o->price,
                       o->remainingQty, o->type, o->timeInForce, o->expiryTime,
                       o->stopPrice, o->stopLimitPrice, o->displayQty,
                       o->pegType, o->pegOffset, o->trailAmount, o->minQty,
                       o->isHidden});
    }
    std::sort(out.begin(), out.end(),
              [](auto& a, auto& b) { return a.id < b.id; });
    return out;
}

std::string tmpJournalPath(const char* tag) {
    auto p = fs::temp_directory_path() /
        ("follower_" + std::string(tag) + "_" +
         std::to_string(::getpid()) + ".log");
    fs::remove(p);
    return p.string();
}

// Drive the leader: produce a deterministic stream of operations.
void driveLeader(Journal& j, OrderBook& leader, uint64_t seed,
                 int numOps) {
    std::mt19937_64 rng(seed);
    std::vector<OrderId> liveIds;
    OrderId nextId = 1;

    for (int op = 0; op < numOps; ++op) {
        int kind = liveIds.empty() ? 0 : int(rng() % 4);

        if (kind == 0) {
            OrderId id = nextId++;
            Side side = (rng() & 1) ? Side::Buy : Side::Sell;
            Price price = Price(990 + int(rng() % 21));
            Quantity qty = Quantity(1 + int(rng() % 10));
            auto r = leader.addOrder(id, /*pid=*/1, side, price, qty,
                                      OrderType::Limit);
            if (std::holds_alternative<OrderId>(r)) {
                j.logAddOrder(id, 1, /*sym=*/0, side, price, qty,
                               OrderType::Limit);
                if (leader.getOrder(id) != nullptr) liveIds.push_back(id);
            }
            // Resync liveIds periodically (matches/cancels remove orders).
            if ((op & 15) == 0) {
                liveIds.clear();
                for (auto& s : snapshotBook(leader)) liveIds.push_back(s.id);
            }
        } else if (kind == 1 && !liveIds.empty()) {
            size_t i = rng() % liveIds.size();
            OrderId id = liveIds[i];
            if (leader.getOrder(id)) {
                leader.cancelOrder(id);
                j.logCancelOrder(id, 0);
            }
            liveIds.erase(liveIds.begin() + i);
        } else if (kind == 2 && !liveIds.empty()) {
            size_t i = rng() % liveIds.size();
            OrderId id = liveIds[i];
            const Order* o = leader.getOrder(id);
            if (o && o->remainingQty > 1) {
                Quantity newQty = Quantity(1 + int(rng() % o->remainingQty));
                if (newQty < o->remainingQty &&
                    leader.modifyOrder(id, newQty)) {
                    j.logModifyOrder(id, 0, newQty);
                }
            }
        } else if (kind == 3 && !liveIds.empty()) {
            size_t i = rng() % liveIds.size();
            OrderId id = liveIds[i];
            if (leader.getOrder(id)) {
                Price np = Price(990 + int(rng() % 21));
                Quantity nq = Quantity(1 + int(rng() % 10));
                if (leader.cancelReplace(id, np, nq)) {
                    j.logCancelReplace(id, 0, np, nq);
                }
            }
        }
    }
    j.flush();
}

void testSequential(uint64_t seed) {
    auto path = tmpJournalPath("seq");
    OrderBook leader(0);
    OrderBook follower(0);

    {
        Journal j(path, Journal::SyncPolicy::Immediate, 1);
        driveLeader(j, leader, seed, /*numOps=*/200);
    }  // journal closed — all entries persisted

    JournalFollower f(path, SymbolId{0}, follower);
    f.poll();  // single-shot catch-up

    auto leaderSnap = snapshotBook(leader);
    auto followerSnap = snapshotBook(follower);
    assert(leaderSnap == followerSnap &&
           "follower did not converge to leader state in sequential mode");

    std::printf("sequential seed=0x%llx: leader=%zu follower=%zu — match (applied=%llu)\n",
                (unsigned long long)seed, leaderSnap.size(),
                followerSnap.size(),
                (unsigned long long)f.appliedCount());

    fs::remove(path);
}

void testConcurrent(uint64_t seed) {
    auto path = tmpJournalPath("conc");
    OrderBook leader(0);
    OrderBook follower(0);

    JournalFollower f(path, SymbolId{0}, follower, /*pollIntervalMs=*/1);
    f.start();

    {
        Journal j(path, Journal::SyncPolicy::Immediate, 1);
        driveLeader(j, leader, seed, /*numOps=*/300);
    }

    // Give the follower time to drain. The follower's loop polls every
    // 1ms; 100ms is generous for ~300 entries.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    f.stop();

    auto leaderSnap = snapshotBook(leader);
    auto followerSnap = snapshotBook(follower);
    if (leaderSnap != followerSnap) {
        std::fprintf(stderr,
            "concurrent seed=0x%llx: divergence — leader=%zu follower=%zu\n",
            (unsigned long long)seed, leaderSnap.size(), followerSnap.size());
        std::abort();
    }

    std::printf("concurrent seed=0x%llx: leader=%zu follower=%zu — match (applied=%llu)\n",
                (unsigned long long)seed, leaderSnap.size(),
                followerSnap.size(),
                (unsigned long long)f.appliedCount());

    fs::remove(path);
}

}  // namespace

void testPromotion() {
    // End-to-end failover rehearsal. Phase 1: leader writes orders to a
    // journal while the follower tails it. Phase 2: leader stops
    // (simulated coordinator-detected death). Phase 3: follower
    // promote()s; the formerly-replay-mode book becomes writable, and
    // the caller can submit new orders that take the normal matching
    // path.
    auto path = tmpJournalPath("promote");

    OrderBook leader(0);
    OrderBook standby(0);
    JournalFollower f(path, SymbolId{0}, standby, /*pollIntervalMs=*/1);
    f.start();

    // ---- Phase 1: leader writes ------------------------------------------
    {
        Journal j(path, Journal::SyncPolicy::Immediate, 1);
        leader.addOrder(1, 1, Side::Buy, 1000, 50, OrderType::Limit);
        j.logAddOrder(1, 1, 0, Side::Buy, 1000, 50, OrderType::Limit);

        leader.addOrder(2, 1, Side::Sell, 1010, 30, OrderType::Limit);
        j.logAddOrder(2, 1, 0, Side::Sell, 1010, 30, OrderType::Limit);

        // Leader stops here (coordinator decides it's dead).
    }

    // ---- Phase 2: promote ------------------------------------------------
    assert(!f.promoted());
    f.promote();
    assert(f.promoted());
    assert(!standby.isReplayMode() && "promotion must exit replay mode");

    // Standby caught up to leader's two orders.
    auto leaderSnap = snapshotBook(leader);
    auto standbySnap = snapshotBook(standby);
    assert(leaderSnap == standbySnap &&
           "standby state diverged from leader at promotion time");

    // ---- Phase 3: promoted book accepts new orders -----------------------
    auto r = standby.addOrder(/*id=*/3, /*pid=*/1, Side::Buy, 1005, 25,
                               OrderType::Limit);
    assert(std::holds_alternative<OrderId>(r) &&
           "promoted book must accept new orders");
    assert(standby.getOrder(3) != nullptr);

    // The new order's matching ran for real: id=3 (buy 1005, 25) does
    // NOT cross the resting sell at 1010. So id=3 rests; verify count.
    auto post = snapshotBook(standby);
    assert(post.size() == leaderSnap.size() + 1 &&
           "promoted book added the new order without dropping prior state");

    std::printf("promotion: standby caught up to leader (%zu entries) and "
                "accepted new order — promoted=%d, replayMode=%d\n",
                leaderSnap.size(), f.promoted(),
                standby.isReplayMode() ? 1 : 0);

    fs::remove(path);
}

// Verifies that JournalFollower (which re-reads via Journal(path).readAll with
// no advisory lock) cleanly DROPS a torn trailing record rather than counting
// or applying it. Two corruption shapes are covered:
//   1. A short final record (a partial append, e.g. a crash mid-write) — fread
//      sees fewer than sizeof(JournalEntry) bytes and stops without counting.
//   2. A full-length but CRC-corrupt final record — the per-entry CRC32 check
//      rejects it.
// In both cases appliedCount() must stay at the number of intact entries.
void testTornTrailingRecord() {
    // ---- Case 1: short (partial) trailing record -------------------------
    {
        auto path = tmpJournalPath("torn_short");
        OrderBook leader(0);
        OrderBook follower(0);

        {
            Journal j(path, Journal::SyncPolicy::Immediate, 1);
            for (OrderId id = 1; id <= 3; ++id) {
                leader.addOrder(id, 1, Side::Buy, Price(1000 + int(id)), 10,
                                OrderType::Limit);
                j.logAddOrder(id, 1, 0, Side::Buy, Price(1000 + int(id)), 10,
                              OrderType::Limit);
            }
        }  // 3 intact records flushed + closed

        // Simulate a crash mid-append: write half a record's worth of bytes.
        {
            FILE* f = std::fopen(path.c_str(), "ab");
            assert(f);
            std::vector<uint8_t> partial(sizeof(JournalEntry) / 2, 0xAB);
            assert(std::fwrite(partial.data(), 1, partial.size(), f) ==
                   partial.size());
            std::fclose(f);
        }

        JournalFollower f(path, SymbolId{0}, follower);
        f.poll();
        assert(f.appliedCount() == 3 &&
               "short torn trailing record must not be counted/applied");
        assert(snapshotBook(leader) == snapshotBook(follower) &&
               "follower diverged after a short torn trailing record");
        fs::remove(path);
    }

    // ---- Case 2: full-length but CRC-corrupt trailing record -------------
    {
        auto path = tmpJournalPath("torn_crc");
        OrderBook leader(0);
        OrderBook follower(0);

        {
            Journal j(path, Journal::SyncPolicy::Immediate, 1);
            for (OrderId id = 1; id <= 3; ++id) {
                leader.addOrder(id, 1, Side::Buy, Price(1000 + int(id)), 10,
                                OrderType::Limit);
                j.logAddOrder(id, 1, 0, Side::Buy, Price(1000 + int(id)), 10,
                              OrderType::Limit);
            }
        }

        // Append an aligned full record with a deliberately wrong checksum.
        {
            JournalEntry bad{};
            bad.entryType = JournalEntry::Type::AddOrder;
            bad.sequenceNumber = 4;       // would-be next contiguous sequence
            bad.orderId = 99;
            bad.participantId = 1;
            bad.side = Side::Buy;
            bad.price = 1234;
            bad.quantity = 10;
            bad.orderType = OrderType::Limit;
            bad.checksum = 0xDEADBEEF;     // does NOT match the body's CRC32
            FILE* f = std::fopen(path.c_str(), "ab");
            assert(f);
            assert(std::fwrite(&bad, sizeof(bad), 1, f) == 1);
            std::fclose(f);
        }

        JournalFollower f(path, SymbolId{0}, follower);
        f.poll();
        assert(f.appliedCount() == 3 &&
               "CRC-corrupt trailing record must not be counted/applied");
        assert(follower.getOrder(99) == nullptr &&
               "corrupt record's order must not appear in the follower book");
        assert(snapshotBook(leader) == snapshotBook(follower) &&
               "follower diverged after a CRC-corrupt trailing record");
        fs::remove(path);
    }

    std::printf("torn-record: short + CRC-corrupt trailing records both "
                "dropped; only intact entries applied\n");
}

// ─── Checkpoint (journal replacement) ───────────────────────────────────────
//
// A checkpoint REPLACES the journal: one Snapshot record per resting order,
// renamed over the old file and renumbered from sequence 1. Everything below
// exercises the follower across that event.

namespace {

// Mirrors MatchingEngine::checkpointInternal's snapshot writer, through the
// same Journal::rewriteAtomically (prepare .tmp -> rename) path the engine
// uses, so the follower sees exactly the file swap production produces.
bool checkpointLeader(Journal& j, const OrderBook& leader) {
    std::vector<Order> resting;
    leader.forEachOrderLocked([&](const Order& o) { resting.push_back(o); });
    return j.rewriteAtomically([&resting](Journal& snap) {
        for (const auto& o : resting) {
            snap.logSnapshot(o.id, o.participantId, /*sym=*/0, o.side, o.price,
                             o.remainingQty, o.type, o.timeInForce, o.expiryTime,
                             o.stopPrice, o.stopLimitPrice, o.displayQty,
                             o.pegType, o.pegOffset, o.trailAmount, o.minQty,
                             o.isHidden);
        }
    });
}

void addBoth(Journal& j, OrderBook& leader, OrderId id, Side side, Price px,
             Quantity qty) {
    auto r = leader.addOrder(id, 1, side, px, qty, OrderType::Limit);
    assert(std::holds_alternative<OrderId>(r));
    j.logAddOrder(id, 1, 0, side, px, qty, OrderType::Limit);
}

// A caught-up follower must keep following after a checkpoint.
//
// The journal here holds MORE records than the book holds resting orders (5
// adds + 2 cancels = 7 records, 3 resting), so the snapshot that replaces it is
// strictly shorter than the follower's position. That is the shape that stalled
// the positional-index follower forever: entries.size() (3, then 4) never
// exceeded appliedCount_ (7), so it applied nothing and never moved again.
void testCheckpointCaughtUp() {
    auto path = tmpJournalPath("ckpt_caught_up");
    OrderBook leader(0);
    OrderBook follower(0);

    Journal j(path, Journal::SyncPolicy::Immediate, 1);
    JournalFollower f(path, SymbolId{0}, follower);

    for (OrderId id = 1; id <= 5; ++id) {
        addBoth(j, leader, id, Side::Buy, Price(990 + int(id)), 10);
    }
    for (OrderId id : {OrderId{4}, OrderId{5}}) {
        leader.cancelOrder(id);
        j.logCancelOrder(id, 0);
    }
    j.flush();

    f.poll();
    assert(snapshotBook(leader) == snapshotBook(follower) &&
           "follower must converge before the checkpoint");
    const uint64_t appliedBefore = f.appliedCount();
    assert(appliedBefore == 7 && "7 records written pre-checkpoint");

    assert(checkpointLeader(j, leader) && "checkpoint must commit");

    // Post-checkpoint traffic the follower has to pick up.
    addBoth(j, leader, 6, Side::Sell, 1010, 7);
    j.flush();

    f.poll();
    assert(follower.getOrder(6) != nullptr &&
           "follower stalled: post-checkpoint entry never applied");
    assert(snapshotBook(leader) == snapshotBook(follower) &&
           "follower diverged across a checkpoint");
    assert(f.appliedCount() == 4 &&
           "position is relative to the current file: 3 snapshots + 1 add");

    std::printf("checkpoint/caught-up: applied %llu pre-checkpoint, converged on "
                "a %llu-record replacement\n",
                (unsigned long long)appliedBefore,
                (unsigned long long)f.appliedCount());

    fs::remove(path);
}

// A follower that is BEHIND when the checkpoint happens must not keep stale
// state. The leader cancels an order the follower is still holding, then
// checkpoints, all between two polls. The snapshot describes the book AFTER
// that cancel, so an idempotent "add what's missing" apply would fix nothing:
// order 2 is absent from the snapshot precisely because it is gone, and the
// follower would hold it forever. Only discarding local state and rebuilding
// from the snapshot converges.
void testCheckpointWhileBehind() {
    auto path = tmpJournalPath("ckpt_behind");
    OrderBook leader(0);
    OrderBook follower(0);

    Journal j(path, Journal::SyncPolicy::Immediate, 1);
    JournalFollower f(path, SymbolId{0}, follower);

    for (OrderId id = 1; id <= 3; ++id) {
        addBoth(j, leader, id, Side::Buy, Price(990 + int(id)), 10);
    }
    j.flush();
    f.poll();
    assert(follower.getOrder(2) != nullptr && "follower holds order 2");

    // --- the follower is asleep from here ---
    leader.cancelOrder(2);
    j.logCancelOrder(2, 0);
    addBoth(j, leader, 4, Side::Buy, 995, 10);
    j.flush();

    assert(checkpointLeader(j, leader) && "checkpoint must commit");

    addBoth(j, leader, 5, Side::Sell, 1010, 4);
    j.flush();
    // --- follower wakes up ---

    f.poll();
    assert(follower.getOrder(2) == nullptr &&
           "follower kept an order the leader cancelled before the checkpoint");
    assert(follower.getOrder(5) != nullptr && "post-checkpoint entry missing");
    assert(snapshotBook(leader) == snapshotBook(follower) &&
           "follower diverged: checkpoint arrived while it was behind");

    std::printf("checkpoint/behind: stale order dropped, follower rebuilt from "
                "the snapshot (%llu records)\n",
                (unsigned long long)f.appliedCount());

    fs::remove(path);
}

// A Snapshot record carries the full order, not just the six fields a plain
// limit needs. Restoring it through the 6-argument addOrder silently turned a
// GTD iceberg into a plain GTC limit — no error, no reject, just an order that
// never expires and shows its full size.
void testSnapshotFieldFidelity() {
    auto path = tmpJournalPath("ckpt_fidelity");
    OrderBook leader(0);
    OrderBook follower(0);

    constexpr uint64_t kExpiry = 4'000'000'000'000'000'000ULL;  // far future
    Journal j(path, Journal::SyncPolicy::Immediate, 1);
    JournalFollower f(path, SymbolId{0}, follower);

    // GTD iceberg with a minimum execution quantity.
    auto r1 = leader.addOrder(1, 7, Side::Buy, 1000, 100, OrderType::Limit,
                              /*stopPrice=*/0, /*displayQty=*/10,
                              TimeInForce::GTD, kExpiry);
    assert(std::holds_alternative<OrderId>(r1));
    j.logAddOrder(1, 7, 0, Side::Buy, 1000, 100, OrderType::Limit,
                  TimeInForce::GTD, kExpiry, /*stopPrice=*/0,
                  /*stopLimitPrice=*/0, /*displayQty=*/10);

    // Hidden pegged order with a minQty.
    auto r2 = leader.addOrder(2, 7, Side::Sell, 1010, 50, OrderType::Pegged,
                              /*stopPrice=*/0, /*displayQty=*/0,
                              TimeInForce::GTC, /*expiryTime=*/0,
                              /*stopLimitPrice=*/0, PegType::PrimaryPeg,
                              /*pegOffset=*/2, /*trailAmount=*/0,
                              /*minQty=*/5, /*hidden=*/true);
    assert(std::holds_alternative<OrderId>(r2));
    j.logAddOrder(2, 7, 0, Side::Sell, 1010, 50, OrderType::Pegged,
                  TimeInForce::GTC, 0, 0, 0, /*displayQty=*/0,
                  PegType::PrimaryPeg, /*pegOffset=*/2, /*trailAmount=*/0,
                  /*minQty=*/5, /*hidden=*/true);
    j.flush();

    // Replace the journal with a pure snapshot, then make the follower take a
    // COLD path through it: it has applied nothing, so every order it ends up
    // with came from a Snapshot record.
    assert(checkpointLeader(j, leader) && "checkpoint must commit");
    f.poll();

    for (OrderId id : {OrderId{1}, OrderId{2}}) {
        const Order* want = leader.getOrder(id);
        const Order* got = follower.getOrder(id);
        assert(want && got && "snapshot-restored order missing on the follower");
        assert(got->timeInForce == want->timeInForce && "TIF lost");
        assert(got->expiryTime == want->expiryTime && "expiry lost");
        assert(got->displayQty == want->displayQty && "iceberg displayQty lost");
        assert(got->pegType == want->pegType && "pegType lost");
        assert(got->pegOffset == want->pegOffset && "pegOffset lost");
        assert(got->stopPrice == want->stopPrice && "stopPrice lost");
        assert(got->stopLimitPrice == want->stopLimitPrice &&
               "stopLimitPrice lost");
        assert(got->trailAmount == want->trailAmount && "trailAmount lost");
        assert(got->minQty == want->minQty && "minQty lost");
        assert(got->isHidden == want->isHidden && "hidden flag lost");
        assert(got->participantId == want->participantId && "participant lost");
        assert(got->type == want->type && "order type lost");
        assert(got->remainingQty == want->remainingQty && "quantity lost");
    }

    // Re-applying the same snapshot must be a no-op, not a DuplicateOrderId.
    f.poll();
    assert(snapshotBook(leader) == snapshotBook(follower) &&
           "re-reading an unchanged snapshot file changed the follower book");

    std::printf("checkpoint/fidelity: GTD iceberg + hidden peg restored with "
                "every field intact\n");

    fs::remove(path);
}

// ─── Multi-symbol journals ──────────────────────────────────────────────────
//
// A leader running N symbols writes them all interleaved into ONE journal, and
// every record names the symbol it belongs to. A follower that applies records
// without looking at that field collapses all N books into one.

// Writes two symbols' traffic, interleaved, leaving b1 and b2 holding exactly
// what a correct follower of each symbol must converge to. Returns the record
// count.
//
// Two of these records are the ones that actually catch a symbol-blind
// follower; the rest is ordinary traffic around them:
//   * order 20 (symbol 2, Sell 999) CROSSES the resting symbol-1 Buy 1000 if it
//     is misrouted — a fill the leader never had, on a book that should not
//     have seen the order at all. Both symbols trade around 1000 on purpose: a
//     misrouted order must be ACCEPTED by the wrong book for the divergence to
//     appear, and a symbol parked a few percent away is rejected on arrival by
//     the venue's volatility collar, which would green this test against a
//     follower that routes nothing.
//   * order id 5 exists on BOTH symbols. Ids are unique per book, not globally
//     (OrderBook::addOrder's duplicate check is per-book and nothing above it
//     enforces more), so the symbol-2 cancel of id 5 destroys the symbol-1
//     order of the same id if it is misrouted.
size_t writeTwoSymbolJournal(Journal& j, OrderBook& b1, OrderBook& b2) {
    // Each symbol is traded by its own participant. Not decoration: with one
    // participant on both sides, self-trade prevention cancels the misrouted
    // aggressor instead of letting it match, the resting order survives
    // untouched, and the crossing assertion below goes green against a follower
    // that routes nothing.
    auto add = [&](OrderBook& book, SymbolId sym, OrderId id, Side side,
                   Price px, Quantity qty) {
        const ParticipantId pid = ParticipantId(sym);
        auto r = book.addOrder(id, pid, side, px, qty, OrderType::Limit);
        assert(std::holds_alternative<OrderId>(r));
        j.logAddOrder(id, pid, sym, side, px, qty, OrderType::Limit);
    };

    add(b1, 1, 10, Side::Buy, 1000, 50);
    add(b2, 2, 20, Side::Sell, 999, 30);   // would cross id 10 if misrouted
    add(b1, 1, 5, Side::Sell, 1010, 10);
    add(b2, 2, 5, Side::Buy, 995, 7);      // same id, different symbol
    b2.cancelOrder(5);
    j.logCancelOrder(5, 2);                // would kill symbol 1's id 5
    add(b1, 1, 11, Side::Buy, 996, 20);
    assert(b2.modifyOrder(20, 15));
    j.logModifyOrder(20, 2, 15);
    assert(b1.cancelReplace(11, 997, 25));
    j.logCancelReplace(11, 1, 997, 25);
    j.flush();
    return 8;
}

// A follower hosting ONE symbol out of a two-symbol journal.
void testMultiSymbolSingleFollower() {
    auto path = tmpJournalPath("multisym_single");
    OrderBook leader1(1);
    OrderBook leader2(2);
    OrderBook follower1(1);

    size_t records = 0;
    {
        Journal j(path, Journal::SyncPolicy::Immediate, 1);
        records = writeTwoSymbolJournal(j, leader1, leader2);
    }

    JournalFollower f(path, SymbolId{1}, follower1);
    f.poll();

    // The assertion that catches the bug: symbol 2's sell at 990 must not have
    // traded against symbol 1's buy at 1000. A symbol-blind follower fills 30
    // of the 50 here and reports a trade the leader never printed.
    const Order* resting = follower1.getOrder(10);
    assert(resting && resting->remainingQty == 50 &&
           "a symbol-2 sell crossed a symbol-1 buy: follower invented a fill");
    assert(follower1.getOrder(5) != nullptr &&
           "a symbol-2 cancel removed the symbol-1 order sharing its id");
    assert(follower1.getOrder(20) == nullptr &&
           "a symbol-2 order rests in the symbol-1 follower book");
    assert(snapshotBook(leader1) == snapshotBook(follower1) &&
           "symbol-1 follower did not converge to the leader's symbol-1 book");
    assert(f.appliedCount() == records &&
           "the cursor is a file position: skipped records still advance it");

    std::printf("multi-symbol/single: %zu records in, symbol-1 book converged "
                "to %zu orders (position=%llu)\n",
                records, snapshotBook(follower1).size(),
                (unsigned long long)f.appliedCount());

    fs::remove(path);
}

// A follower hosting BOTH symbols through a resolver: two books, each
// converging to its own leader book from the one interleaved journal.
void testMultiSymbolResolverFollower() {
    auto path = tmpJournalPath("multisym_resolver");
    OrderBook leader1(1);
    OrderBook leader2(2);
    OrderBook follower1(1);
    OrderBook follower2(2);

    size_t records = 0;
    {
        Journal j(path, Journal::SyncPolicy::Immediate, 1);
        records = writeTwoSymbolJournal(j, leader1, leader2);
    }

    JournalFollower f(path, [&](SymbolId sym) -> OrderBook* {
        if (sym == 1) return &follower1;
        if (sym == 2) return &follower2;
        return nullptr;  // not hosted here
    });
    f.poll();

    assert(snapshotBook(leader1) == snapshotBook(follower1) &&
           "symbol-1 book diverged under the resolver follower");
    assert(snapshotBook(leader2) == snapshotBook(follower2) &&
           "symbol-2 book diverged under the resolver follower");
    assert(follower1.isReplayMode() && follower2.isReplayMode() &&
           "a resolved book must be put into replay mode");
    assert(f.appliedCount() == records);

    std::printf("multi-symbol/resolver: %zu records routed to 2 books — both "
                "converged (applied=%llu)\n",
                records, (unsigned long long)f.appliedCount());

    fs::remove(path);
}

// Mirrors checkpointLeader, but snapshots TWO books into one journal — what a
// multi-symbol leader's checkpoint actually writes.
bool checkpointTwoSymbols(Journal& j, const OrderBook& b1, const OrderBook& b2) {
    std::vector<std::pair<SymbolId, Order>> resting;
    b1.forEachOrderLocked([&](const Order& o) { resting.emplace_back(1, o); });
    b2.forEachOrderLocked([&](const Order& o) { resting.emplace_back(2, o); });
    return j.rewriteAtomically([&resting](Journal& snap) {
        for (const auto& [sym, o] : resting) {
            snap.logSnapshot(o.id, o.participantId, sym, o.side, o.price,
                             o.remainingQty, o.type, o.timeInForce, o.expiryTime,
                             o.stopPrice, o.stopLimitPrice, o.displayQty,
                             o.pegType, o.pegOffset, o.trailAmount, o.minQty,
                             o.isHidden);
        }
    });
}

// Filtering must not move the cursor off the file.
//
// appliedCount_ is a POSITION in the journal, not a count of mutations, so a
// skipped record has to advance it exactly like an applied one. Hold it back
// and the next poll's anchors are read at the wrong offsets: classify() then
// compares the wrong record and either misses a replacement or invents one.
// This drives the follower across a checkpoint with skipped records on both
// sides of it — before, inside the snapshot, and after — and pins the position
// at every step.
void testSkippedEntriesKeepCursorAligned() {
    auto path = tmpJournalPath("multisym_cursor");
    OrderBook leader1(1);
    OrderBook leader2(2);
    OrderBook follower1(1);

    Journal j(path, Journal::SyncPolicy::Immediate, 1);
    JournalFollower f(path, SymbolId{1}, follower1);

    auto add = [&](OrderBook& book, SymbolId sym, OrderId id, Side side,
                   Price px, Quantity qty) {
        // One participant per symbol — see writeTwoSymbolJournal.
        const ParticipantId pid = ParticipantId(sym);
        auto r = book.addOrder(id, pid, side, px, qty, OrderType::Limit);
        assert(std::holds_alternative<OrderId>(r));
        j.logAddOrder(id, pid, sym, side, px, qty, OrderType::Limit);
    };

    // 5 records, 2 of them skipped by this follower.
    //
    // Both symbols trade around 1000 ON PURPOSE. A misrouted order has to be
    // ACCEPTED by the wrong book for the divergence to show; park symbol 2 a
    // few percent away and the venue's volatility collar rejects it on
    // arrival, and the test goes green against a follower that routes nothing.
    // Symbol 2's sell at 1002 crosses symbol 1's bid at 1003 if misrouted,
    // while resting harmlessly above its own book's bid at 1000.
    add(leader1, 1, 1, Side::Buy, 1001, 10);
    add(leader2, 2, 101, Side::Buy, 1000, 10);
    add(leader1, 1, 2, Side::Buy, 1002, 10);
    add(leader2, 2, 102, Side::Sell, 1002, 10);
    add(leader1, 1, 3, Side::Buy, 1003, 10);
    j.flush();

    f.poll();
    assert(f.appliedCount() == 5 &&
           "cursor must count the 2 skipped records, not just the 3 applied");
    assert(snapshotBook(leader1) == snapshotBook(follower1) &&
           "symbol-1 follower diverged before the checkpoint");

    // --- the follower is asleep from here ---
    leader1.cancelOrder(2);
    j.logCancelOrder(2, 1);
    add(leader2, 2, 103, Side::Sell, 1003, 10);
    j.flush();
    // Snapshot: 2 symbol-1 orders + 3 symbol-2 orders = 5 records, renumbered
    // from 1, so record 1 of the new file is a symbol-1 Snapshot.
    assert(checkpointTwoSymbols(j, leader1, leader2) && "checkpoint must commit");
    add(leader2, 2, 104, Side::Buy, 999, 10);
    add(leader1, 1, 4, Side::Buy, 1004, 10);
    j.flush();
    // --- follower wakes up ---

    f.poll();
    assert(f.appliedCount() == 7 &&
           "position in the REPLACEMENT file: 5 snapshot records + 2 appends, "
           "skipped ones included");
    assert(follower1.getOrder(2) == nullptr &&
           "stale symbol-1 order survived a checkpoint taken while behind");
    assert(follower1.getOrder(4) != nullptr &&
           "post-checkpoint symbol-1 entry never applied");
    assert(follower1.getOrder(101) == nullptr &&
           "a symbol-2 snapshot record landed in the symbol-1 book");
    assert(snapshotBook(leader1) == snapshotBook(follower1) &&
           "symbol-1 follower diverged across a multi-symbol checkpoint");

    std::printf("multi-symbol/cursor: checkpoint detected across skipped "
                "records, position=%llu\n",
                (unsigned long long)f.appliedCount());

    fs::remove(path);
}

// A poll reads only the NEW tail, not the whole file.
//
// Deterministic by construction rather than by clock: the follower counts the
// whole records it pulls off the disk, and this asserts a CONSTANT bound per
// poll on a large unchanging file. That is a statement about the algorithm —
// a wall-clock version would be flaky in CI and would prove less, since a fast
// machine re-reading megabytes still looks quick.
void testIncrementalRead() {
    auto path = tmpJournalPath("incremental");
    constexpr int kRecords = 1000;
    constexpr int kPolls = 20;
    OrderBook leader(7);
    OrderBook follower(7);

    {
        // GroupCommit: this is a bulk write, and 1000 fsyncs is time spent
        // proving nothing this test is about.
        Journal j(path, Journal::SyncPolicy::GroupCommit, 256);
        for (int i = 1; i <= kRecords; ++i) {
            // All buys across 20 levels well below any ask: nothing crosses, so
            // every record leaves a resting order and the file is all history.
            const OrderId id = OrderId(i);
            const Price px = Price(900 + (i % 20));
            auto r = leader.addOrder(id, 1, Side::Buy, px, 10, OrderType::Limit);
            assert(std::holds_alternative<OrderId>(r));
            j.logAddOrder(id, 1, /*sym=*/7, Side::Buy, px, 10, OrderType::Limit);
        }
        j.flush();
    }

    JournalFollower f(path, SymbolId{7}, follower);
    f.poll();
    const uint64_t afterColdRead = f.recordsReadFromDisk();
    assert(f.appliedCount() == uint64_t(kRecords));
    assert(afterColdRead >= uint64_t(kRecords) &&
           "the cold catch-up really did walk the whole file — without this the "
           "bound below could pass on a follower that read nothing at all");

    for (int i = 0; i < kPolls; ++i) f.poll();

    // Per poll on an unchanging file: record 1 and the record before the
    // cursor. The budget is a CONSTANT — it must not scale with kRecords.
    constexpr uint64_t kPerPollBudget = 4;
    const uint64_t steadyState = f.recordsReadFromDisk() - afterColdRead;
    assert(steadyState <= kPerPollBudget * kPolls &&
           "poll re-reads the whole journal instead of just the new tail");
    assert(f.appliedCount() == uint64_t(kRecords) &&
           "repeated polls of an unchanging file moved the cursor");
    assert(snapshotBook(leader) == snapshotBook(follower) &&
           "follower diverged while polling an unchanging file");

    std::printf("incremental: %d-record file, cold read %llu records, then "
                "%llu records over %d polls (budget %llu)\n",
                kRecords, (unsigned long long)afterColdRead,
                (unsigned long long)steadyState, kPolls,
                (unsigned long long)(kPerPollBudget * kPolls));

    fs::remove(path);
}

}  // namespace

int main() {
    for (uint64_t seed : {uint64_t{1}, uint64_t{0xC0FFEE}, uint64_t{0xDEADBEEF}}) {
        testSequential(seed);
        testConcurrent(seed);
    }
    testPromotion();
    testTornTrailingRecord();
    testCheckpointCaughtUp();
    testCheckpointWhileBehind();
    testSnapshotFieldFidelity();
    testMultiSymbolSingleFollower();
    testMultiSymbolResolverFollower();
    testSkippedEntriesKeepCursorAligned();
    testIncrementalRead();
    std::puts("JournalFollowerTest passed");
    return 0;
}
