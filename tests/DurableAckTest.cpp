// DurableAckTest — audit C4: a client must not be told about a fill before the
// journal entry behind that fill is on stable storage.
//
// Pre-C4 ordering, which these tests pin against: submitOrder() calls
// book->addOrder(), which runs the whole match and dispatches every fill and
// status update to the client listener from inside it, and only afterwards
// does the engine call journal_->logAddOrder(). So a client-visible fill
// preceded not just the fdatasync behind it but the journal APPEND entirely.
// Under the default GroupCommit(64) that is up to 63 orders whose fills
// clients have acted on and the venue has no record of.
//
// Distinct from JournalAckDurabilityTest, which covers the REPLICATION ack
// (that onCommit_ fires only after a successful sync). That path was already
// correct; this one is about what reaches the client.

#include "MatchingEngine.h"
#include "OrderBook.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace OrderMatcher;

namespace {

int passed = 0;
#define TEST(name) std::cout << "  " << #name << "..." << std::flush
#define PASS() do { std::cout << " ok" << std::endl; ++passed; } while (0)

// Records what a client would actually have seen, in order.
struct ClientView : EventListener {
    std::vector<Trade>       trades;
    std::vector<OrderUpdate> updates;
    void onTrade(const Trade& t) override { trades.push_back(t); }
    void onOrderUpdate(const OrderUpdate& u) override { updates.push_back(u); }
    void onMarketData(const MarketDataUpdate&) override {}
    void onBookVisible(const BookVisibleUpdate&) override {}
};

std::string tempJournalPath(const char* tag) {
    auto p = std::filesystem::temp_directory_path() /
             ("durable_ack_" + std::string(tag) + ".journal");
    std::filesystem::remove(p);
    return p.string();
}

constexpr SymbolId kSym = 1;
constexpr Price    kPx  = 1'000'000;

// The journal batches, so "committed" is a state we can observe rather than
// something that happens instantly.
struct Fixture {
    MatchingEngine engine;
    ClientView     client;
    std::string    path;

    explicit Fixture(const char* tag) : path(tempJournalPath(tag)) {
        engine.addSymbol(kSym);
        engine.getOrderBook(kSym)->setEventListener(&client);
        engine.enableJournal(path);
        engine.start();
    }
    ~Fixture() {
        engine.stop();
        std::filesystem::remove(path);
    }

    void restingSell() {
        engine.submitOrder(kSym, 1, 100, Side::Sell, kPx, 50, OrderType::Limit);
    }
    void crossingBuy() {
        engine.submitOrder(kSym, 2, 200, Side::Buy, kPx, 50, OrderType::Limit);
    }
};

// ─── 1: off by default, and behaviour is unchanged ──────────────────────────
//
// The guarantee costs the commit interval on every client-visible event, so it
// is opt-in. What must NOT change is that leaving it off behaves exactly as
// before — no buffering, no deferral.
void test_OffByDefaultAndPassesThrough() {
    TEST(OffByDefaultAndPassesThrough);
    Fixture f("off");

    assert(!f.engine.durableClientAcks() && "must be off unless asked for");

    f.restingSell();
    f.crossingBuy();

    assert(!f.client.trades.empty() && "with the gate off, fills dispatch immediately");
    assert(f.engine.pendingDurableEvents() == 0 && "nothing may be held while off");
    PASS();
}

// ─── 2: with it on, nothing reaches the client before the commit ────────────
void test_NothingVisibleBeforeTheEntryIsDurable() {
    TEST(NothingVisibleBeforeTheEntryIsDurable);
    Fixture f("held");

    assert(f.engine.enableDurableClientAcks(true) && "sync + journal is supported");

    f.restingSell();
    f.crossingBuy();

    // GroupCommit(64) with only two entries appended, so no commit has run.
    // Pre-fix the client had already been told about the trade.
    assert(f.client.trades.empty() &&
           "a fill was published before its journal entry was durable");
    assert(f.engine.pendingDurableEvents() > 0 &&
           "the events should be held, not dropped");

    // Flushing makes those entries durable, which must release exactly the
    // events that were waiting on them.
    f.engine.getJournal()->flush();

    assert(!f.client.trades.empty() && "durable entries must release their events");
    assert(f.engine.pendingDurableEvents() == 0 && "nothing may still be held");
    PASS();
}

// ─── 3: held events are released in the order they were produced ────────────
//
// A client reconstructs state from this stream, so releasing it out of order
// would be its own corruption — a fill for an order it has not been told was
// accepted.
void test_ReleasedInProductionOrder() {
    TEST(ReleasedInProductionOrder);
    Fixture f("order");
    assert(f.engine.enableDurableClientAcks(true));

    f.restingSell();
    f.crossingBuy();
    f.engine.getJournal()->flush();

    assert(f.client.updates.size() >= 2 && "both orders must be reported");
    uint64_t prev = 0;
    for (const auto& u : f.client.updates) {
        assert(u.sequenceNumber >= prev && "events released out of order");
        prev = u.sequenceNumber;
    }
    PASS();
}

// ─── 4: a rejected order holds nothing ──────────────────────────────────────
//
// A reject writes no journal entry, so there is nothing for its events to wait
// on. Leaving the gate armed would spill them into the next order's group and
// hold them indefinitely.
void test_RejectedOrderIsNotHeldForever() {
    TEST(RejectedOrderIsNotHeldForever);
    Fixture f("reject");
    assert(f.engine.enableDurableClientAcks(true));

    // qty 0 is refused at admission — no entry, nothing to wait for.
    f.engine.submitOrder(kSym, 9, 100, Side::Buy, kPx, 0, OrderType::Limit);

    assert(f.engine.pendingDurableEvents() == 0 &&
           "a rejected order writes no entry and must hold nothing");
    PASS();
}

// ─── 5: disabling must not strand what is already held ──────────────────────
void test_DisablingReleasesHeldEvents() {
    TEST(DisablingReleasesHeldEvents);
    Fixture f("disable");
    assert(f.engine.enableDurableClientAcks(true));

    f.restingSell();
    f.crossingBuy();
    assert(f.client.trades.empty() && "held while enabled");

    f.engine.enableDurableClientAcks(false);

    // Those writes did happen; a client that never hears about them is worse
    // off than one told slightly early.
    assert(!f.client.trades.empty() && "disabling must not drop held events");
    assert(f.engine.pendingDurableEvents() == 0);
    PASS();
}

}  // namespace

int main() {
    std::cout << "\n=== Durable Ack Tests (C4) ===\n\n";

    test_OffByDefaultAndPassesThrough();
    test_NothingVisibleBeforeTheEntryIsDurable();
    test_ReleasedInProductionOrder();
    test_RejectedOrderIsNotHeldForever();
    test_DisablingReleasesHeldEvents();

    std::cout << "\n" << passed << " passed\n\n";
    return 0;
}
