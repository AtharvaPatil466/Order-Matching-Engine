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

    JournalFollower f(path, follower);
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

    JournalFollower f(path, follower, /*pollIntervalMs=*/1);
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
    JournalFollower f(path, standby, /*pollIntervalMs=*/1);
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

        JournalFollower f(path, follower);
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

        JournalFollower f(path, follower);
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
    JournalFollower f(path, follower);

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
    JournalFollower f(path, follower);

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
    JournalFollower f(path, follower);

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
    std::puts("JournalFollowerTest passed");
    return 0;
}
