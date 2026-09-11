// ReplicationApplyTest — audit H11: the replication apply path had ZERO test
// coverage.
//
// Measured with llvm-cov over all 515 tests before this file existed:
//
//     MatchingEngine::applyReplicatedEntry   0.00%
//     MatchingEngine::streamSnapshot         0.00%
//
// These two are how a backup learns what the primary did. If they are wrong the
// backup's book silently diverges, and a failover trades against state that
// never existed — which is exactly the "no split-brain" property the README
// claims is verified, resting on code nothing executed.
//
// The audit named this: "487 lines of MatchingEngine.cpp never execute in any
// test, incl. replication apply edge cases".
//
// Distinct from JournalFollowerTest (replays a journal FILE) and
// ReplicationProtocolTest (wire framing). Neither calls either function.

#include "MatchingEngine.h"
#include "OrderBook.h"

#include <cassert>
#include <iostream>
#include <map>
#include <string>

using namespace OrderMatcher;

namespace {

int passed = 0;
#define TEST(name) std::cout << "  " << #name << "..." << std::flush
#define PASS() do { std::cout << " ok" << std::endl; ++passed; } while (0)

constexpr SymbolId kSym = 7;
constexpr Price    kPx  = 1'000'000;

// Depth as a comparable value, so "the backup matches the primary" is one
// assertion rather than a pile of field checks.
std::map<Price, Quantity> depthOf(const OrderBook& book, Side side) {
    std::map<Price, Quantity> out;
    const auto snap = book.getSnapshot(MarketDataSnapshot::MAX_DEPTH);
    const size_t n = (side == Side::Buy) ? snap.bidCount : snap.askCount;
    const PriceLevel* src = (side == Side::Buy) ? snap.bids : snap.asks;
    for (size_t i = 0; i < n; ++i) {
        if (src[i].totalQuantity == 0) continue;
        out[src[i].price] = src[i].totalQuantity;
    }
    return out;
}

std::string describe(const std::map<Price, Quantity>& d) {
    std::string s = "{";
    for (const auto& [p, q] : d) s += " " + std::to_string(p) + ":" + std::to_string(q);
    return s + " }";
}

// Ship the primary's live state to a backup the way a joining follower gets it.
void bootstrap(const MatchingEngine& primary, MatchingEngine& backup) {
    primary.streamSnapshot([&](const JournalEntry& e) {
        backup.applyReplicatedEntry(e);
    });
}

void assertBooksMatch(const MatchingEngine& primary, MatchingEngine& backup,
                      const char* what) {
    const auto* pb = primary.getOrderBook(kSym);
    auto* bb = backup.getOrderBook(kSym);
    assert(pb && "primary must have the book");
    assert(bb && "backup must have created the book from the stream");

    for (Side side : {Side::Buy, Side::Sell}) {
        const auto p = depthOf(*pb, side);
        const auto b = depthOf(*bb, side);
        if (p != b) {
            std::cout << "\n    " << what << " diverged on "
                      << (side == Side::Buy ? "bids" : "asks")
                      << "\n      primary: " << describe(p)
                      << "\n      backup:  " << describe(b) << "\n";
            assert(false && "backup diverged from primary");
        }
    }

    // Structural check too: equal depth totals do not prove the backup's book
    // is internally sound, and a backup is only worth having if it can be
    // promoted.
    std::string err;
    assert(bb->validateIntegrity(&err) && err.empty());
}

// ─── 1: a snapshot reproduces the primary's book on a cold backup ───────────
void test_SnapshotBootstrapsAnEmptyBackup() {
    TEST(SnapshotBootstrapsAnEmptyBackup);
    MatchingEngine primary;
    primary.addSymbol(kSym);
    primary.start();

    primary.submitOrder(kSym, 1, 100, Side::Buy,  kPx - 2000, 50, OrderType::Limit);
    primary.submitOrder(kSym, 2, 100, Side::Buy,  kPx - 1000, 30, OrderType::Limit);
    primary.submitOrder(kSym, 3, 200, Side::Sell, kPx + 1000, 40, OrderType::Limit);

    MatchingEngine backup;
    backup.addSymbol(kSym);   // see test_ApplyReportsFailureForUnknownSymbol
    backup.start();
    bootstrap(primary, backup);

    assertBooksMatch(primary, backup, "snapshot bootstrap");
    primary.stop();
    backup.stop();
    PASS();
}

// ─── 2: the snapshot carries REMAINING quantity, not original size ──────────
//
// streamSnapshot deliberately writes remainingQty so a partially-filled order
// arrives at its live size. Getting this wrong hands the backup more liquidity
// than exists, and the divergence only surfaces once the backup is promoted and
// starts matching against phantom size.
void test_SnapshotUsesRemainingQuantityAfterPartialFill() {
    TEST(SnapshotUsesRemainingQuantityAfterPartialFill);
    MatchingEngine primary;
    primary.addSymbol(kSym);
    primary.start();

    // Rest 100, trade 40 against it: 60 remains.
    primary.submitOrder(kSym, 1, 100, Side::Sell, kPx, 100, OrderType::Limit);
    primary.submitOrder(kSym, 2, 200, Side::Buy,  kPx, 40,  OrderType::Limit);

    const Order* resting = primary.getOrderBook(kSym)->getOrder(1);
    assert(resting && resting->remainingQty == 60);

    MatchingEngine backup;
    backup.addSymbol(kSym);   // see test_ApplyReportsFailureForUnknownSymbol
    backup.start();
    bootstrap(primary, backup);

    const Order* copied = backup.getOrderBook(kSym)->getOrder(1);
    assert(copied && "the resting order must reach the backup");
    assert(copied->remainingQty == 60 &&
           "backup must hold the LIVE size, not the original 100");
    assertBooksMatch(primary, backup, "partial fill snapshot");

    primary.stop();
    backup.stop();
    PASS();
}

// ─── 3: incremental entries keep a bootstrapped backup in step ──────────────
void test_IncrementalEntriesTrackThePrimary() {
    TEST(IncrementalEntriesTrackThePrimary);
    MatchingEngine primary;
    primary.addSymbol(kSym);
    primary.start();
    primary.submitOrder(kSym, 1, 100, Side::Buy, kPx - 1000, 50, OrderType::Limit);

    MatchingEngine backup;
    backup.addSymbol(kSym);   // see test_ApplyReportsFailureForUnknownSymbol
    backup.start();
    bootstrap(primary, backup);

    auto replicate = [&](JournalEntry e) { backup.applyReplicatedEntry(e); };

    JournalEntry add{};
    add.entryType = JournalEntry::Type::AddOrder;
    add.orderId = 2; add.participantId = 100; add.symbolId = kSym;
    add.side = Side::Buy; add.price = kPx - 500; add.quantity = 25;
    add.orderType = OrderType::Limit; add.timeInForce = TimeInForce::GTC;
    primary.submitOrder(kSym, 2, 100, Side::Buy, kPx - 500, 25, OrderType::Limit);
    replicate(add);
    assertBooksMatch(primary, backup, "after AddOrder");

    JournalEntry mod{};
    mod.entryType = JournalEntry::Type::ModifyOrder;
    mod.orderId = 1; mod.newQty = 20;
    primary.modifyOrder(kSym, 1, 20);
    replicate(mod);
    assertBooksMatch(primary, backup, "after ModifyOrder");

    JournalEntry rep{};
    rep.entryType = JournalEntry::Type::CancelReplace;
    rep.orderId = 2; rep.newPrice = kPx - 300; rep.newQty = 15;
    primary.cancelReplace(kSym, 2, kPx - 300, 15);
    replicate(rep);
    assertBooksMatch(primary, backup, "after CancelReplace");

    JournalEntry can{};
    can.entryType = JournalEntry::Type::CancelOrder;
    can.orderId = 1;
    primary.cancelOrder(kSym, 1);
    replicate(can);
    assertBooksMatch(primary, backup, "after CancelOrder");

    primary.stop();
    backup.stop();
    PASS();
}

// ─── 4: an entry for an unknown symbol creates the book ─────────────────────
//
// A backup that joined before a symbol existed still has to accept its orders.
// Dropping them would diverge silently, visible only at promotion.
void test_ApplyCreatesAnUnknownSymbolsBook() {
    TEST(ApplyCreatesAnUnknownSymbolsBook);
    MatchingEngine backup;
    assert(backup.getOrderBook(99) == nullptr && "symbol must be unknown first");

    JournalEntry e{};
    e.entryType = JournalEntry::Type::AddOrder;
    e.orderId = 1; e.participantId = 100; e.symbolId = 99;
    e.side = Side::Buy; e.price = kPx; e.quantity = 10;
    e.orderType = OrderType::Limit; e.timeInForce = TimeInForce::GTC;

    assert(backup.applyReplicatedEntry(e) && "apply must report success");
    auto* book = backup.getOrderBook(99);
    assert(book && "the book must have been created");
    assert(book->getOrder(1) && "and hold the order");

    backup.stop();
    PASS();
}

// ─── 4b: a RUNNING backup cannot learn a new symbol, and must say so ────────
//
// start()/startAsync() set booksFrozen_, and addSymbolLocked returns early
// while frozen — the freeze is real thread-safety, since worker threads iterate
// books_ and inserting under them would race. The consequence is that a running
// backup cannot create a book for a symbol it was not configured with, so a
// symbol added on the primary at runtime replicates to nothing.
//
// applyReplicatedEntry does report this by returning false. What matters is
// that callers check: a backup that ignores the result carries on looking
// healthy while diverging, and only discovers it at promotion. main.cpp
// ignored it until this test was written.
void test_ApplyReportsFailureForUnknownSymbolWhileRunning() {
    TEST(ApplyReportsFailureForUnknownSymbolWhileRunning);
    MatchingEngine backup;
    backup.addSymbol(kSym);
    backup.start();          // books frozen from here

    JournalEntry e{};
    e.entryType = JournalEntry::Type::Snapshot;
    e.orderId = 1; e.participantId = 100; e.symbolId = 4242;  // never configured
    e.side = Side::Buy; e.price = kPx; e.quantity = 10;
    e.orderType = OrderType::Limit; e.timeInForce = TimeInForce::GTC;

    assert(!backup.applyReplicatedEntry(e) &&
           "a backup that cannot apply an entry must report it, not drop it quietly");
    assert(backup.getOrderBook(4242) == nullptr);

    // And the same for an incremental add, which is the steady-state case.
    e.entryType = JournalEntry::Type::AddOrder;
    assert(!backup.applyReplicatedEntry(e));

    backup.stop();
    PASS();
}

// ─── 5: entries for orders the backup has never seen are refused, not faked ──
//
// A cancel or modify for an unknown id means the backup missed something. Its
// job is to say it could not apply the entry so the caller can resync;
// reporting success is how a divergence becomes invisible.
void test_ApplyRefusesEntriesForUnknownOrders() {
    TEST(ApplyRefusesEntriesForUnknownOrders);
    MatchingEngine backup;
    backup.addSymbol(kSym);
    backup.start();

    JournalEntry can{};
    can.entryType = JournalEntry::Type::CancelOrder;
    can.orderId = 4242;
    assert(!backup.applyReplicatedEntry(can) &&
           "cancel of an unknown order must not report applied");

    JournalEntry mod{};
    mod.entryType = JournalEntry::Type::ModifyOrder;
    mod.orderId = 4242; mod.newQty = 5;
    assert(!backup.applyReplicatedEntry(mod) &&
           "modify of an unknown order must not report applied");

    JournalEntry rep{};
    rep.entryType = JournalEntry::Type::CancelReplace;
    rep.orderId = 4242; rep.newPrice = kPx; rep.newQty = 5;
    assert(!backup.applyReplicatedEntry(rep) &&
           "replace of an unknown order must not report applied");

    backup.stop();
    PASS();
}

// ─── 6: a snapshot of an empty primary is a no-op, not a malformed entry ────
void test_SnapshotOfEmptyPrimaryEmitsNothing() {
    TEST(SnapshotOfEmptyPrimaryEmitsNothing);
    MatchingEngine primary;
    primary.addSymbol(kSym);
    primary.start();

    int emitted = 0;
    primary.streamSnapshot([&](const JournalEntry&) { ++emitted; });
    assert(emitted == 0 && "an empty book must emit no snapshot entries");

    // A null callback must not crash — the coordinator may not be attached.
    primary.streamSnapshot(nullptr);

    primary.stop();
    PASS();
}

// ─── 7: order types with extra state survive the round trip ─────────────────
//
// The snapshot carries displayQty, peg and stop fields. Drop any and the backup
// holds an order that looks similar and behaves differently — an iceberg that
// shows its whole size, or a stop that never triggers.
void test_SnapshotPreservesOrderTypeDetail() {
    TEST(SnapshotPreservesOrderTypeDetail);
    MatchingEngine primary;
    primary.addSymbol(kSym);
    primary.start();

    primary.submitOrder(kSym, 1, 100, Side::Buy, kPx - 1000, 100,
                        OrderType::Iceberg, /*stopPrice=*/0, /*displayQty=*/20);

    MatchingEngine backup;
    backup.addSymbol(kSym);   // see test_ApplyReportsFailureForUnknownSymbol
    backup.start();
    bootstrap(primary, backup);

    const Order* src = primary.getOrderBook(kSym)->getOrder(1);
    const Order* dst = backup.getOrderBook(kSym)->getOrder(1);
    assert(src && dst);
    assert(dst->type == OrderType::Iceberg && "type must survive");
    assert(dst->displayQty == src->displayQty && "displayQty must survive");
    assert(dst->remainingQty == src->remainingQty);

    // And the visible depth must agree — an iceberg replicated as a plain limit
    // would show its whole size here.
    assertBooksMatch(primary, backup, "iceberg snapshot");

    primary.stop();
    backup.stop();
    PASS();
}

}  // namespace

int main() {
    std::cout << "\n=== Replication Apply Tests (H11) ===\n\n";

    test_SnapshotBootstrapsAnEmptyBackup();
    test_SnapshotUsesRemainingQuantityAfterPartialFill();
    test_IncrementalEntriesTrackThePrimary();
    test_ApplyCreatesAnUnknownSymbolsBook();
    test_ApplyReportsFailureForUnknownSymbolWhileRunning();
    test_ApplyRefusesEntriesForUnknownOrders();
    test_SnapshotOfEmptyPrimaryEmitsNothing();
    test_SnapshotPreservesOrderTypeDetail();

    std::cout << "\n" << passed << " passed\n\n";
    return 0;
}
