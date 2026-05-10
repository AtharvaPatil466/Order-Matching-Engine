#include "MatchingEngine.h"
#include "Journal.h"
#include <cassert>
#include <iostream>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <random>
#include <vector>

using namespace OrderMatcher;

static const char* JOURNAL_PATH = "/tmp/test_journal_crash.bin";

void cleanup() {
    std::remove(JOURNAL_PATH);
    std::remove((std::string(JOURNAL_PATH) + ".tmp").c_str());
}

struct ActiveOrderView {
    OrderId id;
    ParticipantId participantId;
    SymbolId symbolId;
    Side side;
    Price price;
    Quantity remainingQty;
};

std::vector<ActiveOrderView> captureOrders(const MatchingEngine& engine, SymbolId symbolId) {
    std::vector<ActiveOrderView> orders;
    const OrderBook* book = engine.getOrderBook(symbolId);
    if (!book) {
        return orders;
    }

    book->forEachOrder([&](const Order& order) {
        orders.push_back({order.id, order.participantId, symbolId, order.side,
                          order.price, order.remainingQty});
    });

    std::sort(orders.begin(), orders.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.id < rhs.id;
    });
    return orders;
}

void assertSameBookState(const MatchingEngine& lhs, const MatchingEngine& rhs,
                         SymbolId symbolId) {
    const OrderBook* lhsBook = lhs.getOrderBook(symbolId);
    const OrderBook* rhsBook = rhs.getOrderBook(symbolId);
    assert(lhsBook != nullptr);
    assert(rhsBook != nullptr);
    assert(lhsBook->getBestBid() == rhsBook->getBestBid());
    assert(lhsBook->getBestAsk() == rhsBook->getBestAsk());
    assert(lhsBook->getBidLevelsCount() == rhsBook->getBidLevelsCount());
    assert(lhsBook->getAskLevelsCount() == rhsBook->getAskLevelsCount());

    auto lhsOrders = captureOrders(lhs, symbolId);
    auto rhsOrders = captureOrders(rhs, symbolId);
    assert(lhsOrders.size() == rhsOrders.size());
    for (size_t i = 0; i < lhsOrders.size(); ++i) {
        assert(lhsOrders[i].id == rhsOrders[i].id);
        assert(lhsOrders[i].participantId == rhsOrders[i].participantId);
        assert(lhsOrders[i].symbolId == rhsOrders[i].symbolId);
        assert(lhsOrders[i].side == rhsOrders[i].side);
        assert(lhsOrders[i].price == rhsOrders[i].price);
        assert(lhsOrders[i].remainingQty == rhsOrders[i].remainingQty);
    }
}

// ─── Test 1: Normal journal replay restores exact state ─────────────────────

void testNormalReplay() {
    std::cout << "Running testNormalReplay..." << std::endl;
    cleanup();

    // Phase 1: Create engine, add orders, journal everything
    {
        MatchingEngine engine;
        engine.enableJournal(JOURNAL_PATH);
        engine.start();
        engine.addSymbol(0);

        // Add some orders that will rest in the book
        engine.processOrder(0, 1, 100, Side::Buy, toPrice(99.00), 50, OrderType::Limit);
        engine.processOrder(0, 2, 100, Side::Buy, toPrice(98.00), 30, OrderType::Limit);
        engine.processOrder(0, 3, 200, Side::Sell, toPrice(101.00), 40, OrderType::Limit);
        engine.processOrder(0, 4, 200, Side::Sell, toPrice(102.00), 20, OrderType::Limit);

        // Verify state before "crash"
        auto* book = engine.getOrderBook(0);
        assert(book->getBestBid() == toPrice(99.00));
        assert(book->getBestAsk() == toPrice(101.00));
        assert(book->getBidLevelsCount() == 2);
        assert(book->getAskLevelsCount() == 2);

        engine.stop();
        // Engine destroyed — simulates crash
    }

    // Phase 2: Create new engine, replay journal, verify state matches
    {
        MatchingEngine engine;
        engine.enableJournal(JOURNAL_PATH);
        engine.start();

        size_t replayed = engine.replayJournal();
        assert(replayed == 4); // 4 add orders

        auto* book = engine.getOrderBook(0);
        assert(book != nullptr);
        assert(book->getBestBid() == toPrice(99.00));
        assert(book->getBestAsk() == toPrice(101.00));
        assert(book->getBidLevelsCount() == 2);
        assert(book->getAskLevelsCount() == 2);

        // Verify individual orders
        const Order* o1 = book->getOrder(1);
        assert(o1 != nullptr);
        assert(o1->remainingQty == 50);
        assert(o1->price == toPrice(99.00));

        const Order* o3 = book->getOrder(3);
        assert(o3 != nullptr);
        assert(o3->remainingQty == 40);

        engine.stop();
    }

    cleanup();
    std::cout << "testNormalReplay PASSED" << std::endl;
}

// ─── Test 2: Truncated write — simulate kill -9 mid-entry ───────────────────

void testTruncatedWrite() {
    std::cout << "Running testTruncatedWrite..." << std::endl;
    cleanup();

    // Phase 1: Write valid entries
    {
        Journal journal(JOURNAL_PATH);
        journal.logAddOrder(1, 100, 0, Side::Buy, toPrice(99.00), 50, OrderType::Limit);
        journal.logAddOrder(2, 100, 0, Side::Buy, toPrice(98.00), 30, OrderType::Limit);
        journal.logAddOrder(3, 200, 0, Side::Sell, toPrice(101.00), 40, OrderType::Limit);
        journal.flush();
    }

    // Phase 2: Append a partial/truncated entry (simulate crash mid-write)
    {
        FILE* f = std::fopen(JOURNAL_PATH, "ab");
        assert(f != nullptr);
        // Write half of a JournalEntry (garbage bytes that form an incomplete entry)
        char garbage[sizeof(JournalEntry) / 2];
        std::memset(garbage, 0xAB, sizeof(garbage));
        std::fwrite(garbage, 1, sizeof(garbage), f);
        std::fclose(f);
    }

    // Phase 3: Replay — should recover 3 valid entries, stop at truncated one
    {
        Journal journal(JOURNAL_PATH);
        auto entries = journal.readAll(true); // CRC validation ON
        assert(entries.size() == 3); // Only the 3 valid entries

        assert(entries[0].orderId == 1);
        assert(entries[1].orderId == 2);
        assert(entries[2].orderId == 3);
    }

    // Phase 4: Replay through MatchingEngine
    {
        MatchingEngine engine;
        engine.enableJournal(JOURNAL_PATH);
        engine.start();

        size_t replayed = engine.replayJournal();
        assert(replayed == 3);

        auto* book = engine.getOrderBook(0);
        assert(book != nullptr);
        assert(book->getBestBid() == toPrice(99.00));
        assert(book->getBestAsk() == toPrice(101.00));

        engine.stop();
    }

    cleanup();
    std::cout << "testTruncatedWrite PASSED" << std::endl;
}

// ─── Test 3: Corrupted CRC — flip bits in a valid entry ─────────────────────

void testCorruptedCRC() {
    std::cout << "Running testCorruptedCRC..." << std::endl;
    cleanup();

    // Phase 1: Write 5 valid entries
    {
        Journal journal(JOURNAL_PATH);
        for (int i = 1; i <= 5; ++i) {
            journal.logAddOrder(static_cast<OrderId>(i), 100, 0, Side::Buy,
                               toPrice(90.0 + i), 10 * i, OrderType::Limit);
        }
        journal.flush();
    }

    // Phase 2: Corrupt the 4th entry (flip a byte in the price field)
    {
        FILE* f = std::fopen(JOURNAL_PATH, "r+b");
        assert(f != nullptr);

        // Seek to the 4th entry and corrupt a byte in the middle
        long offset = static_cast<long>(3 * sizeof(JournalEntry) + 20); // somewhere in the data
        std::fseek(f, offset, SEEK_SET);
        uint8_t byte;
        std::fread(&byte, 1, 1, f);
        byte ^= 0xFF; // flip all bits
        std::fseek(f, offset, SEEK_SET);
        std::fwrite(&byte, 1, 1, f);
        std::fclose(f);
    }

    // Phase 3: Replay — should get only 3 valid entries (stops at corrupted 4th)
    {
        Journal journal(JOURNAL_PATH);
        auto entries = journal.readAll(true); // CRC validation ON
        assert(entries.size() == 3);

        assert(entries[0].orderId == 1);
        assert(entries[1].orderId == 2);
        assert(entries[2].orderId == 3);
    }

    // Phase 4: Without CRC validation, all 5 entries are read
    {
        Journal journal(JOURNAL_PATH);
        auto entries = journal.readAll(false); // CRC validation OFF
        assert(entries.size() == 5);
    }

    cleanup();
    std::cout << "testCorruptedCRC PASSED" << std::endl;
}

// ─── Test 4: Checkpoint + replay ─────────────────────────────────────────────

void testCheckpointReplay() {
    std::cout << "Running testCheckpointReplay..." << std::endl;
    cleanup();

    // Phase 1: Add orders, then checkpoint
    {
        MatchingEngine engine;
        engine.enableJournal(JOURNAL_PATH);
        engine.start();

        engine.processOrder(0, 1, 100, Side::Buy, toPrice(99.00), 50, OrderType::Limit);
        engine.processOrder(0, 2, 100, Side::Buy, toPrice(98.00), 30, OrderType::Limit);
        engine.processOrder(0, 3, 200, Side::Sell, toPrice(101.00), 40, OrderType::Limit);

        // Checkpoint: truncates journal, writes snapshot of current state
        engine.checkpoint();

        // Add more orders after checkpoint
        engine.processOrder(0, 4, 200, Side::Sell, toPrice(102.00), 20, OrderType::Limit);
        engine.processOrder(0, 5, 100, Side::Buy, toPrice(97.00), 60, OrderType::Limit);

        auto* book = engine.getOrderBook(0);
        assert(book->getBidLevelsCount() == 3);
        assert(book->getAskLevelsCount() == 2);

        engine.stop();
    }

    // Phase 2: Replay from checkpoint — should restore full state
    {
        MatchingEngine engine;
        engine.enableJournal(JOURNAL_PATH);
        engine.start();

        size_t replayed = engine.replayJournal();
        // Should have 3 snapshot entries (from checkpoint) + 2 new orders = 5
        assert(replayed == 5);

        auto* book = engine.getOrderBook(0);
        assert(book != nullptr);
        assert(book->getBidLevelsCount() == 3);
        assert(book->getAskLevelsCount() == 2);
        assert(book->getBestBid() == toPrice(99.00));
        assert(book->getBestAsk() == toPrice(101.00));

        // Verify post-checkpoint orders exist
        const Order* o4 = book->getOrder(4);
        assert(o4 != nullptr);
        assert(o4->remainingQty == 20);

        const Order* o5 = book->getOrder(5);
        assert(o5 != nullptr);
        assert(o5->remainingQty == 60);

        engine.stop();
    }

    cleanup();
    std::cout << "testCheckpointReplay PASSED" << std::endl;
}

// ─── Test 5: Empty journal ──────────────────────────────────────────────────

void testEmptyJournal() {
    std::cout << "Running testEmptyJournal..." << std::endl;
    cleanup();

    // Create empty file
    { std::ofstream f(JOURNAL_PATH); }

    MatchingEngine engine;
    engine.enableJournal(JOURNAL_PATH);
    engine.start();

    size_t replayed = engine.replayJournal();
    assert(replayed == 0);

    engine.stop();
    cleanup();
    std::cout << "testEmptyJournal PASSED" << std::endl;
}

// ─── Test 6: Journal with matching (trades) then replay ─────────────────────

void testJournalWithTrades() {
    std::cout << "Running testJournalWithTrades..." << std::endl;
    cleanup();

    // Phase 1: Add orders that will match
    {
        MatchingEngine engine;
        engine.enableJournal(JOURNAL_PATH);
        engine.start();

        // Resting sell at 100.00
        engine.processOrder(0, 1, 200, Side::Sell, toPrice(100.00), 100, OrderType::Limit);
        // Aggressive buy at 100.00 — matches 50
        engine.processOrder(0, 2, 100, Side::Buy, toPrice(100.00), 50, OrderType::Limit);
        // Remaining: sell order #1 has 50 left

        auto* book = engine.getOrderBook(0);
        const Order* o1 = book->getOrder(1);
        assert(o1 != nullptr);
        assert(o1->remainingQty == 50);

        engine.stop();
    }

    // Phase 2: Replay — journal records the addOrder commands, matching re-executes
    {
        MatchingEngine engine;
        engine.enableJournal(JOURNAL_PATH);
        engine.start();

        size_t replayed = engine.replayJournal();
        assert(replayed == 2);

        auto* book = engine.getOrderBook(0);
        const Order* o1 = book->getOrder(1);
        assert(o1 != nullptr);
        assert(o1->remainingQty == 50); // Same state after replay

        engine.stop();
    }

    cleanup();
    std::cout << "testJournalWithTrades PASSED" << std::endl;
}

// ─── Test 7: randomized command stream replays to identical final state ──────

void testDeterministicReplayEquivalence() {
    std::cout << "Running testDeterministicReplayEquivalence..." << std::endl;
    cleanup();

    constexpr int kSeeds = 20;
    constexpr int kOpsPerSeed = 250;

    for (int seed = 1; seed <= kSeeds; ++seed) {
        cleanup();

        MatchingEngine original;
        original.enableJournal(JOURNAL_PATH);
        original.start();
        original.addSymbol(0);

        std::mt19937 rng(static_cast<uint32_t>(seed));
        std::vector<OrderId> activeOrders;
        OrderId nextOrderId = 1;

        for (int op = 0; op < kOpsPerSeed; ++op) {
            int action = activeOrders.empty()
                ? 0
                : static_cast<int>(rng() % 10);

            if (action < 6) {
                Side side = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
                Price base = side == Side::Buy ? toPrice(90.00) : toPrice(110.00);
                Price price = base + static_cast<Price>((rng() % 500) * 10);
                Quantity qty = 1 + (rng() % 100);
                SubmitResult result = original.submitOrder(
                    0, nextOrderId, 100 + (rng() % 8), side, price, qty, OrderType::Limit);
                if (result.isAccepted()) {
                    activeOrders.push_back(nextOrderId);
                }
                ++nextOrderId;
            } else if (action < 8) {
                size_t idx = static_cast<size_t>(rng() % activeOrders.size());
                original.cancelOrder(0, activeOrders[idx]);
                activeOrders.erase(activeOrders.begin() + static_cast<std::ptrdiff_t>(idx));
            } else {
                size_t idx = static_cast<size_t>(rng() % activeOrders.size());
                Quantity newQty = 1 + (rng() % 120);
                original.modifyOrder(0, activeOrders[idx], newQty);
            }

            if (op == kOpsPerSeed / 2) {
                original.checkpoint();
            }
        }

        original.checkpoint();
        original.stop();

        MatchingEngine replayed;
        replayed.enableJournal(JOURNAL_PATH);
        replayed.start();
        (void)replayed.replayJournal();
        assertSameBookState(original, replayed, 0);
        replayed.stop();
    }

    cleanup();
    std::cout << "testDeterministicReplayEquivalence PASSED" << std::endl;
}

int main() {
    std::cout << "\n=== Journal Crash Recovery Tests ===" << std::endl;

    testNormalReplay();
    testTruncatedWrite();
    testCorruptedCRC();
    testCheckpointReplay();
    testEmptyJournal();
    testJournalWithTrades();
    testDeterministicReplayEquivalence();

    std::cout << "\nALL JOURNAL CRASH TESTS PASSED!" << std::endl;
    return 0;
}
