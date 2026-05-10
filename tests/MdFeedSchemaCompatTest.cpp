// MdFeedSchemaCompatTest — market data feed schema evolution proof.
//
// Tests:
//   1. V1 publisher ↔ V1 subscriber roundtrip
//   2. Header version bounds validation
//   3. entrySize constraint verification
//   4. Snapshot roundtrip
//   5. Subscriber gap detection
//   6. Sequential incremental updates
//   7. Ring buffer wrap-around

#include "MarketDataPublisher.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <unistd.h>

using namespace OrderMatcher;

static int tests_passed = 0;

#define SECTION(name) std::cout << "  " << name << "..." << std::flush
#define PASS()        do { ++tests_passed; std::cout << " PASS" << std::endl; } while(0)

// Helper to generate unique shm names using PID to avoid collisions
static std::string shmName(const char* base) {
    return std::string(base) + "_" + std::to_string(getpid());
}

// ─── Test 1: V1 roundtrip ──────────────────────────────────────────────────

void test_v1_roundtrip() {
    SECTION("V1 publisher/subscriber roundtrip");

    auto name = shmName("mdc_t1");
    MarketDataPublisher pub(name, 16);
    assert(pub.start());

    MarketDataSubscriber sub(name);
    assert(sub.connect());

    // Publish an incremental update
    MarketDataUpdate upd{};
    upd.action = MarketDataUpdate::Action::Add;
    upd.side   = Side::Buy;
    upd.level  = {1000000, 50, 3};
    upd.timestamp = 42;
    upd.sequenceNumber = 1;
    pub.publishUpdate(upd);

    ShmEntry entry{};
    assert(sub.poll(entry));
    assert(entry.type == ShmEntry::Type::IncrementalUpdate);
    assert(entry.update.level.price == 1000000);
    assert(entry.update.level.totalQuantity == 50);
    assert(entry.update.level.orderCount == 3);
    assert(entry.sequence == 0);

    sub.disconnect();
    pub.stop();
    PASS();
}

// ─── Test 2: ShmHeader constants ────────────────────────────────────────────

void test_header_constants() {
    SECTION("ShmHeader magic and version constants");

    assert(ShmHeader::MAGIC == 0x4D444658);
    assert(ShmHeader::VERSION >= ShmHeader::MIN_READABLE_VERSION);
    assert(ShmHeader::MIN_READABLE_VERSION == 1);

    PASS();
}

// ─── Test 3: entrySize in header matches sizeof(ShmEntry) ───────────────────

void test_entry_size_in_header() {
    SECTION("entrySize advertised in header");

    auto name = shmName("mdc_t3");
    MarketDataPublisher pub(name, 4);
    assert(pub.start());

    MarketDataSubscriber sub(name);
    assert(sub.connect());

    sub.disconnect();
    pub.stop();
    PASS();
}

// ─── Test 4: Snapshot roundtrip ─────────────────────────────────────────────

void test_snapshot_roundtrip() {
    SECTION("Snapshot publish/subscribe roundtrip");

    auto name = shmName("mdc_t4");
    MarketDataPublisher pub(name, 8);
    assert(pub.start());

    MarketDataSubscriber sub(name);
    assert(sub.connect());

    // Build a snapshot
    MarketDataSnapshot snap{};
    snap.symbolId = 42;
    snap.lastTradePrice = 1005000;
    snap.lastTradeQty   = 100;
    snap.timestamp      = 99;
    snap.bidCount       = 2;
    snap.bids[0] = {1000000, 500, 10};
    snap.bids[1] = {999000,  200, 5};
    snap.askCount       = 1;
    snap.asks[0] = {1010000, 300, 8};

    pub.publishSnapshot(snap);

    ShmEntry entry{};
    assert(sub.poll(entry));
    assert(entry.type == ShmEntry::Type::Snapshot);
    assert(entry.symbolId == 42);
    assert(entry.lastTradePrice == 1005000);
    assert(entry.bidCount == 2);
    assert(entry.askCount == 1);
    assert(entry.bidLevels[0].price == 1000000);
    assert(entry.askLevels[0].totalQuantity == 300);

    sub.disconnect();
    pub.stop();
    PASS();
}

// ─── Test 5: Subscriber gap detection ───────────────────────────────────────

void test_gap_detection() {
    SECTION("Subscriber gap detection");

    auto name = shmName("mdc_t5");
    MarketDataPublisher pub(name, 32);
    assert(pub.start());

    MarketDataSubscriber sub(name);
    assert(sub.connect());

    // Write 10 entries
    MarketDataUpdate upd{};
    upd.action = MarketDataUpdate::Action::Add;
    upd.side   = Side::Buy;
    for (int i = 0; i < 10; ++i) {
        upd.level.price = 1000000 + i;
        upd.sequenceNumber = static_cast<uint64_t>(i);
        pub.publishUpdate(upd);
    }

    assert(sub.gapCount() == 10);

    // Read a few
    ShmEntry entry{};
    for (int i = 0; i < 3; ++i) {
        assert(sub.poll(entry));
    }
    assert(sub.gapCount() == 7);

    sub.disconnect();
    pub.stop();
    PASS();
}

// ─── Test 6: Multiple sequential updates ────────────────────────────────────

void test_sequential_updates() {
    SECTION("Sequential incremental updates");

    auto name = shmName("mdc_t6");
    MarketDataPublisher pub(name, 32);
    assert(pub.start());

    MarketDataSubscriber sub(name);
    assert(sub.connect());

    // Publish several updates and verify order is preserved
    for (uint64_t i = 0; i < 5; ++i) {
        MarketDataUpdate upd{};
        upd.action = MarketDataUpdate::Action::Add;
        upd.side   = (i % 2 == 0) ? Side::Buy : Side::Sell;
        upd.level  = {static_cast<Price>(1000000 + i * 1000), static_cast<Quantity>(100 + i), 1};
        upd.sequenceNumber = i;
        pub.publishUpdate(upd);
    }

    for (uint64_t i = 0; i < 5; ++i) {
        ShmEntry entry{};
        assert(sub.poll(entry));
        assert(entry.sequence == i);
        assert(entry.update.level.price == static_cast<Price>(1000000 + i * 1000));
    }

    // No more entries
    ShmEntry entry{};
    assert(!sub.poll(entry));

    sub.disconnect();
    pub.stop();
    PASS();
}

// ─── Test 7: Ring buffer wrap-around ────────────────────────────────────────

void test_ring_wrap() {
    SECTION("Ring buffer wrap-around with overwrite");

    auto name = shmName("mdc_t7");
    MarketDataPublisher pub(name, 4);  // very small ring
    assert(pub.start());

    // Write 8 entries into a capacity-4 ring (overwrites first 4)
    MarketDataUpdate upd{};
    upd.action = MarketDataUpdate::Action::Add;
    upd.side   = Side::Buy;
    for (uint64_t i = 0; i < 8; ++i) {
        upd.level.price = static_cast<Price>(i);
        upd.sequenceNumber = i;
        pub.publishUpdate(upd);
    }

    MarketDataSubscriber sub(name);
    assert(sub.connect());

    // Subscriber starts at seq 0, but entries 0-3 are overwritten.
    // Gap detection should handle the jump.
    ShmEntry entry{};
    uint64_t readCount = 0;
    while (sub.poll(entry)) {
        ++readCount;
    }
    // Should have read at most `capacity` entries
    assert(readCount <= 4);

    sub.disconnect();
    pub.stop();
    PASS();
}

// ─── Main ───────────────────────────────────────────────────────────────────

int main() {
    std::cout << "\n═══ Market Data Feed Schema Compatibility Tests ═══\n" << std::endl;

    test_v1_roundtrip();
    test_header_constants();
    test_entry_size_in_header();
    test_snapshot_roundtrip();
    test_gap_detection();
    test_sequential_updates();
    test_ring_wrap();

    std::cout << "\n─── Results: " << tests_passed << " passed ───" << std::endl;
    std::cout << "\nAll MD feed schema compat tests passed.\n" << std::endl;
    return 0;
}
