// TestResearchHarness — Google Test suite for ResearchHarness.
//
// All tests use real Journal + MatchingEngine instances; no mocking.
// A temporary journal file is created in std::filesystem::temp_directory_path()
// and removed after each test.
//
// Test coverage:
//  (a) replay drives trades through engine
//  (b) TradeCallback fires for each trade produced
//  (c) step-by-step mode works (loadJournal + step())
//  (d) BookUpdateCallback fires
//  (e) eventsReplayed() and tradesObserved() counters
//  (f) replay on empty journal yields zero events
//  (g) onTrade callback receives correct trade fields

#include <gtest/gtest.h>

#include "Journal.h"
#include "MatchingEngine.h"
#include "MicrostructureMetrics.h"
#include "ResearchHarness.h"
#include "Types.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace OrderMatcher;

// ── Fixtures ────────────────────────────────────────────────────────────────

// Base fixture: manages a temp journal file and a started engine with
// a single symbol.
class ResearchHarnessTest : public ::testing::Test {
protected:
    static constexpr SymbolId kSym = 1;
    static constexpr ParticipantId kBuyer  = 10;
    static constexpr ParticipantId kSeller = 20;

    MatchingEngine engine_;
    std::string    journalPath_;

    void SetUp() override {
        // Build a unique temp path per test.
        journalPath_ = (fs::temp_directory_path() /
                        ("rh_test_" + std::to_string(::getpid()) + "_" +
                         std::to_string(reinterpret_cast<uintptr_t>(this)) +
                         ".log")).string();
        fs::remove(journalPath_);  // clean any leftover

        // Symbols must be registered before start(); ResearchHarness
        // installs listeners on already-existing books during loadJournal().
        engine_.addSymbol(kSym);
        engine_.start();
    }

    void TearDown() override {
        engine_.stop();
        fs::remove(journalPath_);
    }

    // Write a journal that produces exactly one trade:
    //   SELL 100 @ 100.0000 then BUY 100 @ 100.0000 (crosses).
    void writeCrossingJournal(Journal& j) {
        j.logAddOrder(1, kSeller, kSym, Side::Sell,
                      toPrice(100.0), 100, OrderType::Limit);
        j.logAddOrder(2, kBuyer,  kSym, Side::Buy,
                      toPrice(100.0), 100, OrderType::Limit);
        j.flush();
    }

    // Write N crossing pairs (N trades total).
    void writeNCrossingPairs(Journal& j, int n) {
        OrderId id = 1;
        for (int i = 0; i < n; ++i) {
            j.logAddOrder(id++, kSeller, kSym, Side::Sell,
                          toPrice(100.0), 10, OrderType::Limit);
            j.logAddOrder(id++, kBuyer,  kSym, Side::Buy,
                          toPrice(100.0), 10, OrderType::Limit);
        }
        j.flush();
    }
};

// ── Tests ────────────────────────────────────────────────────────────────────

// (a) replay drives trades through the engine: the engine produces a trade.
TEST_F(ResearchHarnessTest, ReplayDrivesTrades) {
    {
        Journal j(journalPath_, Journal::SyncPolicy::Immediate, 1);
        writeCrossingJournal(j);
    }

    ResearchHarness harness(engine_);
    uint64_t replayed = harness.replay(journalPath_);

    EXPECT_EQ(replayed, 2u);  // 2 journal entries (sell + buy)
    EXPECT_EQ(harness.tradesObserved(), 1u);
}

// (b) TradeCallback fires for each trade produced.
TEST_F(ResearchHarnessTest, TradeCallbackFires) {
    {
        Journal j(journalPath_, Journal::SyncPolicy::Immediate, 1);
        writeCrossingJournal(j);
    }

    ResearchHarness harness(engine_);
    std::vector<Trade> captured;
    harness.onTrade([&](const Trade& t) { captured.push_back(t); });

    harness.replay(journalPath_);

    ASSERT_EQ(captured.size(), 1u);
    EXPECT_EQ(captured[0].price,    toPrice(100.0));
    EXPECT_EQ(captured[0].quantity, 100u);
}

// (b) TradeCallback fires N times for N crossing pairs.
TEST_F(ResearchHarnessTest, TradeCallbackFiresNTimes) {
    constexpr int kN = 5;
    {
        Journal j(journalPath_, Journal::SyncPolicy::Immediate, 1);
        writeNCrossingPairs(j, kN);
    }

    ResearchHarness harness(engine_);
    int callCount = 0;
    harness.onTrade([&](const Trade&) { ++callCount; });
    harness.replay(journalPath_);

    EXPECT_EQ(callCount, kN);
    EXPECT_EQ(harness.tradesObserved(), static_cast<uint64_t>(kN));
}

// (c) Step-by-step mode: loadJournal + step().
TEST_F(ResearchHarnessTest, StepByStepMode) {
    {
        Journal j(journalPath_, Journal::SyncPolicy::Immediate, 1);
        writeCrossingJournal(j);
    }

    ResearchHarness harness(engine_);
    std::vector<Trade> trades;
    harness.onTrade([&](const Trade& t) { trades.push_back(t); });

    harness.loadJournal(journalPath_);

    // Before any steps, no events replayed.
    EXPECT_EQ(harness.eventsReplayed(), 0u);

    bool s1 = harness.step();  // replay SELL entry (no trade yet)
    EXPECT_TRUE(s1);
    EXPECT_EQ(harness.eventsReplayed(), 1u);
    EXPECT_EQ(trades.size(), 0u);

    bool s2 = harness.step();  // replay BUY entry → trade fires
    EXPECT_TRUE(s2);
    EXPECT_EQ(harness.eventsReplayed(), 2u);
    EXPECT_EQ(trades.size(), 1u);

    bool s3 = harness.step();  // exhausted
    EXPECT_FALSE(s3);
    EXPECT_EQ(harness.eventsReplayed(), 2u);
}

// (c) step() returns false immediately on an exhausted (or empty) journal.
TEST_F(ResearchHarnessTest, StepReturnsFalseWhenExhausted) {
    // Empty journal.
    {
        Journal j(journalPath_, Journal::SyncPolicy::Immediate, 1);
        j.flush();
    }

    ResearchHarness harness(engine_);
    harness.loadJournal(journalPath_);
    EXPECT_FALSE(harness.step());
}

// (d) BookUpdateCallback fires during replay.
TEST_F(ResearchHarnessTest, BookUpdateCallbackFires) {
    {
        Journal j(journalPath_, Journal::SyncPolicy::Immediate, 1);
        writeCrossingJournal(j);
    }

    ResearchHarness harness(engine_);
    int bookUpdates = 0;
    harness.onBookUpdate([&](SymbolId /*sym*/, Price /*mid*/,
                              Quantity /*bid*/, Quantity /*ask*/) {
        ++bookUpdates;
    });

    harness.replay(journalPath_);

    // At least one book-update should have fired (after the trade).
    EXPECT_GT(bookUpdates, 0);
}

// (e) eventsReplayed() matches the entry count.
TEST_F(ResearchHarnessTest, EventsReplayedCounter) {
    constexpr int kN = 4;
    {
        Journal j(journalPath_, Journal::SyncPolicy::Immediate, 1);
        writeNCrossingPairs(j, kN);  // 2*kN entries
    }

    ResearchHarness harness(engine_);
    harness.replay(journalPath_);

    EXPECT_EQ(harness.eventsReplayed(), static_cast<uint64_t>(2 * kN));
}

// (f) Replay on empty journal yields zero events and zero trades.
TEST_F(ResearchHarnessTest, EmptyJournalReturnsZero) {
    {
        Journal j(journalPath_, Journal::SyncPolicy::Immediate, 1);
        j.flush();
    }

    ResearchHarness harness(engine_);
    int tradeCalls = 0;
    harness.onTrade([&](const Trade&) { ++tradeCalls; });
    uint64_t replayed = harness.replay(journalPath_);

    EXPECT_EQ(replayed,    0u);
    EXPECT_EQ(tradeCalls,  0);
    EXPECT_EQ(harness.tradesObserved(), 0u);
}

// (g) Verify trade fields: buyer/seller sides match journal entries.
TEST_F(ResearchHarnessTest, TradeFieldsAreCorrect) {
    {
        Journal j(journalPath_, Journal::SyncPolicy::Immediate, 1);
        // Sell first (resting), then buy (aggressor).
        j.logAddOrder(1, kSeller, kSym, Side::Sell,
                      toPrice(50.0), 200, OrderType::Limit);
        j.logAddOrder(2, kBuyer, kSym, Side::Buy,
                      toPrice(50.0), 200, OrderType::Limit);
        j.flush();
    }

    ResearchHarness harness(engine_);
    std::vector<Trade> trades;
    harness.onTrade([&](const Trade& t) { trades.push_back(t); });
    harness.replay(journalPath_);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price,    toPrice(50.0));
    EXPECT_EQ(trades[0].quantity, 200u);
    EXPECT_EQ(trades[0].symbolId, kSym);
}

// Combined: verify ResearchHarness + MicrostructureMetrics integration.
// Feed harness-captured trades through MicrostructureMetrics and verify
// that the effective spread and OFI are sane.
TEST_F(ResearchHarnessTest, IntegrationWithMicrostructureMetrics) {
    constexpr int kN = 6;
    {
        Journal j(journalPath_, Journal::SyncPolicy::Immediate, 1);
        writeNCrossingPairs(j, kN);
    }

    ResearchHarness harness(engine_);
    MicrostructureMetrics metrics;

    harness.onTrade([&](const Trade& t) {
        MicrostructureMetrics::TradeEvent ev;
        ev.price       = t.price;
        ev.side        = Side::Buy;  // aggressor is buy for crossing pairs
        ev.qty         = t.quantity;
        ev.midpoint    = t.price;    // at-the-trade mid
        ev.timestampNs = t.timestamp;
        metrics.record(ev);
    });

    harness.replay(journalPath_);

    EXPECT_EQ(metrics.sampleCount(), static_cast<uint64_t>(kN));
    // All trades at exact mid → effective spread is 0.
    EXPECT_DOUBLE_EQ(metrics.effectiveSpread(), 0.0);
    // All buys → OFI = +1.
    EXPECT_DOUBLE_EQ(metrics.orderFlowImbalance(), 1.0);
}

// ── Cancel replay is routed by order id, not by a symbol the record lacks ───
//
// Journal::logCancelOrder builds a value-initialised JournalEntry and sets only
// the type, timestamp and order id — symbolId stays 0. ResearchHarness used to
// dispatch on that field, so every cancel went to book 0. On a single-symbol
// journal that is right by accident; on symbol 7 it silently did nothing, and
// research replay reported a book still full of orders the venue had cancelled.
//
// Two symbols, neither of them 0, so the old code cannot pass by luck.
TEST(ResearchHarnessCancelRouting, CancelAppliesToTheBookHoldingTheOrder) {
    const auto path = (fs::temp_directory_path() /
                       ("rh_cancel_" + std::to_string(::getpid()) + ".log")).string();
    fs::remove(path);

    constexpr SymbolId kA = 7;
    constexpr SymbolId kB = 9;

    {
        Journal j(path, Journal::SyncPolicy::GroupCommit, 64);
        j.logAddOrder(101, 10, kA, Side::Buy,  toPrice(100.0), 50, OrderType::Limit);
        j.logAddOrder(202, 10, kB, Side::Sell, toPrice(101.0), 40, OrderType::Limit);
        j.logCancelOrder(101, kA);              // kA's order
        j.logModifyOrder(202, kB, 25);          // kB's order, shrink
        j.flush();
    }

    MatchingEngine engine;
    engine.addSymbol(kA);
    engine.addSymbol(kB);
    engine.start();

    ResearchHarness harness(engine);
    harness.loadJournal(path);
    while (harness.step()) {}

    const auto a = engine.getOrderBook(kA)->getSnapshot(MarketDataSnapshot::MAX_DEPTH);
    EXPECT_EQ(a.bidCount, 0u) << "the cancel never reached symbol 7's book";

    const auto b = engine.getOrderBook(kB)->getSnapshot(MarketDataSnapshot::MAX_DEPTH);
    ASSERT_EQ(b.askCount, 1u) << "the modify removed the order instead of shrinking it";
    EXPECT_EQ(b.asks[0].totalQuantity, 25u) << "the modify never reached symbol 9's book";

    engine.stop();
    fs::remove(path);
}
