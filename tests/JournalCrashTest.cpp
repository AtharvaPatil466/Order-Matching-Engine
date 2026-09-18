#include "MatchingEngine.h"
#include "Journal.h"
#include <cassert>
#include <iostream>
#include <fstream>
#include <array>
#include <cstdio>
#include <sys/stat.h>
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

        // Seek to the 4th entry and corrupt a byte in the middle. Offset is
        // measured from the first RECORD, not from the start of the file: the
        // journal now carries a header, and hard-coding a file offset here
        // would silently corrupt a different record than intended.
        long offset = static_cast<long>(Journal::headerBytesOf(JOURNAL_PATH) +
                                        3 * sizeof(JournalEntry) + 20);
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

    // MULTIPLE symbols, none of them 0.
    //
    // This swept 20 seeds x 250 ops against a single symbol 0, and every other
    // journal test in the suite is single-symbol too — JournalReplayProperty,
    // GTDReplay, GracefulShutdown, CombinedChaos and DurableAck all use one
    // book, and the two multi-symbol tests in ManualTest never enable the
    // journal. So the whole journal area was verified against the one shape
    // where cross-book routing cannot be wrong.
    //
    // That is not hypothetical: CancelOrder, ModifyOrder and CancelReplace
    // records carry no symbolId (Journal::logCancelOrder leaves it 0), and
    // replay only survives that by scanning every book for the order id. On
    // one book the scan is trivially right. ResearchHarness, which trusted the
    // field instead, silently dropped every cancel on any symbol but 0 — a
    // real bug that lived because nothing here ever used a second book.
    //
    // Deliberately not starting at 0: a symbol that equals the records'
    // zero-filled default is the one value that can pass for the wrong reason.
    static const std::array<SymbolId, 3> kSymbols{3, 7, 11};

    for (int seed = 1; seed <= kSeeds; ++seed) {
        cleanup();

        MatchingEngine original;
        original.enableJournal(JOURNAL_PATH);
        // Symbols BEFORE start(): start() sets booksFrozen_, so a later
        // addSymbol does not produce a book. The single-symbol version of this
        // test registered after start() and still worked, because symbol 0 is
        // auto-registered by the constructor — so addSymbol(0) was a no-op and
        // the ordering bug was invisible.
        for (SymbolId sym : kSymbols) {
            original.addSymbol(sym);
        }
        original.start();

        std::mt19937 rng(static_cast<uint32_t>(seed));
        // Order id -> the symbol it lives on. Replay resolves a CancelOrder
        // record by scanning every book for the id, because the record does
        // not carry a symbol; this map is how the test knows which book to
        // drive, and lets the assertions below be per-symbol.
        std::vector<std::pair<OrderId, SymbolId>> activeOrders;
        OrderId nextOrderId = 1;

        for (int op = 0; op < kOpsPerSeed; ++op) {
            int action = activeOrders.empty()
                ? 0
                : static_cast<int>(rng() % 10);

            if (action < 6) {
                const SymbolId sym = kSymbols[rng() % kSymbols.size()];
                Side side = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
                Price base = side == Side::Buy ? toPrice(90.00) : toPrice(110.00);
                Price price = base + static_cast<Price>((rng() % 500) * 10);
                Quantity qty = 1 + (rng() % 100);
                SubmitResult result = original.submitOrder(
                    sym, nextOrderId, 100 + (rng() % 8), side, price, qty,
                    OrderType::Limit);
                if (result.isAccepted()) {
                    activeOrders.emplace_back(nextOrderId, sym);
                }
                ++nextOrderId;
            } else if (action < 8) {
                size_t idx = static_cast<size_t>(rng() % activeOrders.size());
                const auto [id, sym] = activeOrders[idx];
                original.cancelOrder(sym, id);
                activeOrders.erase(activeOrders.begin() + static_cast<std::ptrdiff_t>(idx));
            } else {
                size_t idx = static_cast<size_t>(rng() % activeOrders.size());
                const auto [id, sym] = activeOrders[idx];
                Quantity newQty = 1 + (rng() % 120);
                original.modifyOrder(sym, id, newQty);
            }

            if (op == kOpsPerSeed / 2) {
                original.checkpoint();
            }
        }

        original.checkpoint();
        original.stop();

        MatchingEngine replayed;
        replayed.enableJournal(JOURNAL_PATH);
        for (SymbolId sym : kSymbols) {
            replayed.addSymbol(sym);
        }
        replayed.start();
        (void)replayed.replayJournal();
        for (SymbolId sym : kSymbols) {
            assertSameBookState(original, replayed, sym);
        }
        replayed.stop();
    }

    cleanup();
    std::cout << "testDeterministicReplayEquivalence PASSED" << std::endl;
}


// ── bytesOnDisk() must equal the actual file, not merely resemble it ─────────
//
// bytesOnDisk() used to ask the OS every time — fstat(2) on Linux, ftell()
// elsewhere — and needsCheckpoint() calls it on EVERY append, because the
// entry-count disjunct in front of it is false 249,999 times in 250,000 at the
// default threshold. On Linux that is a syscall per journaled order, inside
// journalMutex_, whose result is discarded almost every time. Measured on x86
// CI with the disk amortised away: 980k appends/s with it, 4.88M/s without.
//
// It is now a counter maintained where bytes actually land. That trades a
// syscall for an invariant, and this test is the invariant: drift either way
// breaks size-triggered checkpointing silently — too small and it never fires
// (the journal grows without bound), too large and it fires constantly (a
// rewrite stall on every append).
//
// The paths that replace the file underneath the counter are the interesting
// ones, so truncate() and rewriteAtomically() are both exercised, plus a
// reopen of an existing file.
void testBytesOnDiskTracksTheRealFile() {
    std::cout << "Running testBytesOnDiskTracksTheRealFile..." << std::endl;
    cleanup();

    auto realSize = []() -> size_t {
        struct stat st{};
        return ::stat(JOURNAL_PATH, &st) == 0 ? static_cast<size_t>(st.st_size) : 0;
    };
    auto agree = [&](Journal& j, const char* what) {
        j.flush();
        const size_t tracked = j.bytesOnDisk();
        const size_t actual  = realSize();
        if (tracked != actual) {
            std::cerr << "  " << what << ": tracked=" << tracked
                      << " actual=" << actual << std::endl;
        }
        assert(tracked == actual && "bytesOnDisk() drifted from the file");
    };

    {
        Journal j(JOURNAL_PATH, Journal::SyncPolicy::GroupCommit, 64);

        // Enough to span several commits, and not a multiple of the batch, so
        // a partly-filled batch_ is in flight at each check.
        for (int i = 1; i <= 200; ++i) {
            j.logAddOrder(i, 1, 0, Side::Buy, 1000, 10, OrderType::Limit);
        }
        agree(j, "after 200 appends");

        j.truncate();
        agree(j, "after truncate");

        for (int i = 1; i <= 50; ++i) {
            j.logAddOrder(i, 1, 0, Side::Buy, 1000, 10, OrderType::Limit);
        }
        agree(j, "after 50 more");

        // The rewrite closes, renames a smaller file over the live one, and
        // reopens — the counter has to come back DOWN, which a pure running
        // total would get wrong.
        const bool rewritten = j.rewriteAtomically([](Journal& snap) {
            for (int i = 1; i <= 7; ++i) {
                snap.logSnapshot(i, 1, 0, Side::Buy, 1000, 10, OrderType::Limit,
                                 TimeInForce::GTC, 0, 0, 0, 0,
                                 PegType::None, 0, 0, 0, false);
            }
        });
        assert(rewritten && "rewriteAtomically failed");
        agree(j, "after rewriteAtomically");
        assert(j.bytesOnDisk() ==
                   sizeof(JournalFileHeader) + 7 * sizeof(JournalEntry) &&
               "the snapshot should be exactly its header plus 7 entries");

        for (int i = 100; i < 110; ++i) {
            j.logAddOrder(i, 1, 0, Side::Buy, 1000, 10, OrderType::Limit);
        }
        agree(j, "after post-rewrite appends");
    }

    {
        // Reopening an existing journal must anchor to what is already there,
        // not start from zero.
        Journal j(JOURNAL_PATH, Journal::SyncPolicy::GroupCommit, 64);
        agree(j, "after reopen");
        assert(j.bytesOnDisk() > 0 && "reopen anchored to an empty file");
    }

    cleanup();
    std::cout << "testBytesOnDiskTracksTheRealFile PASSED" << std::endl;
}


// ── The same order id on two symbols must not lose an order on recovery ─────
//
// OrderBook's duplicate-id check is PER BOOK, so two participants trading
// different symbols with overlapping id ranges — entirely ordinary, ids come
// from the client — both get accepted. Cancel/Modify/CancelReplace records
// used to carry no symbol, so replay found the target by scanning every book
// for the id and taking the first hit. With the id present in two books that
// is a coin flip, and the loser is a resting order that silently does not come
// back after a restart.
//
// Live and replayed state diverged: A kept its order, replay did not.
//
// Note the checkpoint: there deliberately isn't one. A checkpoint rewrites the
// journal into Snapshot records, which DO carry symbolId, so checkpointing
// here would mask the very path this test exists to cover — the first version
// of this test did exactly that and passed for the wrong reason.
void testDuplicateIdAcrossSymbols() {
    std::cout << "Running testDuplicateIdAcrossSymbols..." << std::endl;
    cleanup();
    constexpr SymbolId kA = 3, kB = 7;
    constexpr OrderId kId = 5;

    auto depth = [](const MatchingEngine& e, SymbolId sym) {
        return e.getOrderBook(sym)->getSnapshot(MarketDataSnapshot::MAX_DEPTH).bidCount;
    };

    size_t liveA = 0, liveB = 0;
    {
        MatchingEngine original;
        original.enableJournal(JOURNAL_PATH);
        original.addSymbol(kA);
        original.addSymbol(kB);
        original.start();

        assert(original.submitOrder(kA, kId, 100, Side::Buy, toPrice(90.0), 10,
                                    OrderType::Limit).isAccepted());
        assert(original.submitOrder(kB, kId, 200, Side::Buy, toPrice(95.0), 20,
                                    OrderType::Limit).isAccepted() &&
               "per-book duplicate check should admit the same id on another symbol");

        original.cancelOrder(kB, kId);   // cancel B's copy only
        original.stop();

        liveA = depth(original, kA);
        liveB = depth(original, kB);
        // Scoped so the destructor runs here: stop() does NOT flush the
        // journal, only ~Journal() does, and with the default GroupCommit
        // batch of 64 these three entries would otherwise still be in memory.
        // checkpoint() would flush too — but it rewrites the journal into
        // Snapshot records, which DO carry symbolId, and would mask the
        // cancel-resolution path this test exists to cover.
    }
    assert(liveA == 1 && "A's order should still be resting");
    assert(liveB == 0 && "B's order was cancelled");

    MatchingEngine replayed;
    replayed.enableJournal(JOURNAL_PATH);
    replayed.addSymbol(kA);
    replayed.addSymbol(kB);
    replayed.start();
    (void)replayed.replayJournal();

    assert(depth(replayed, kA) == 1 &&
           "recovery cancelled the wrong book's order and lost A's");
    assert(depth(replayed, kB) == 0 &&
           "recovery resurrected an order that was cancelled");
    replayed.stop();

    cleanup();
    std::cout << "testDuplicateIdAcrossSymbols PASSED" << std::endl;
}


// ── An unreadable journal must fail loudly, not start empty and overwrite ───
//
// Records carry no magic, no length prefix and no version: the file is a bare
// array of packed structs and framing is implicit in sizeof(JournalEntry). A
// layout change therefore slices an existing file on the wrong boundaries,
// every record fails CRC, the replay returns nothing, and the engine starts
// with an EMPTY BOOK — then appends to that same file, because it is opened
// "ab+". Silent, total, and on the upgrade path.
//
// Simulated here by writing a file of the right length but the wrong content,
// which is what a layout change looks like from the reader's side.
void testUnreadableJournalRefusesToAppend() {
    std::cout << "Running testUnreadableJournalRefusesToAppend..." << std::endl;
    cleanup();

    // Several whole records' worth of bytes that decode as no valid record.
    const size_t bogusBytes = sizeof(JournalEntry) * 4;
    {
        std::ofstream out(JOURNAL_PATH, std::ios::binary);
        std::vector<char> junk(bogusBytes, '\x5A');
        out.write(junk.data(), static_cast<std::streamsize>(junk.size()));
    }

    struct stat before{};
    assert(::stat(JOURNAL_PATH, &before) == 0);
    assert(static_cast<size_t>(before.st_size) == bogusBytes);

    {
        Journal j(JOURNAL_PATH, Journal::SyncPolicy::Immediate, 1);
        assert(j.recoveryFailed() &&
               "a file of whole records yielding none must be reported, not ignored");

        // The second half of the old failure: appending would make the file
        // permanently unreadable. It must be refused.
        j.logAddOrder(1, 1, 0, Side::Buy, 1000, 10, OrderType::Limit);
        j.flush();
    }

    struct stat after{};
    assert(::stat(JOURNAL_PATH, &after) == 0);
    assert(static_cast<size_t>(after.st_size) == bogusBytes &&
           "the unreadable journal was appended to");

    cleanup();
    std::cout << "testUnreadableJournalRefusesToAppend PASSED" << std::endl;
}

// A torn FIRST write — a crash partway through record one — leaves a
// non-empty file with no valid records too, and that is legitimate. Recovery
// has always tolerated it, so the guard above must not fire on it.
void testTornFirstRecordStillAppends() {
    std::cout << "Running testTornFirstRecordStillAppends..." << std::endl;
    cleanup();

    {
        std::ofstream out(JOURNAL_PATH, std::ios::binary);
        std::vector<char> partial(sizeof(JournalEntry) / 2, '\x00');
        out.write(partial.data(), static_cast<std::streamsize>(partial.size()));
    }

    {
        Journal j(JOURNAL_PATH, Journal::SyncPolicy::Immediate, 1);
        assert(!j.recoveryFailed() &&
               "a partial first record is a torn write, not a format mismatch");
        j.logAddOrder(1, 1, 0, Side::Buy, 1000, 10, OrderType::Limit);
        j.flush();
    }

    struct stat st{};
    assert(::stat(JOURNAL_PATH, &st) == 0);
    assert(static_cast<size_t>(st.st_size) > sizeof(JournalEntry) / 2 &&
           "a torn first write must not block recovery from continuing");

    cleanup();
    std::cout << "testTornFirstRecordStillAppends PASSED" << std::endl;
}


// ── A format mismatch is NAMED, and a pre-header journal still reads ────────
//
// The previous guard made "we could not read this at all" loud. The header
// makes it specific: a version or record-size mismatch can say so, instead of
// leaving the operator to infer it from every record failing CRC at once.
void testFormatVersionMismatchIsRefused() {
    std::cout << "Running testFormatVersionMismatchIsRefused..." << std::endl;
    cleanup();

    // A well-formed header from a hypothetical future build, then a record.
    {
        JournalFileHeader h{};
        std::memcpy(h.magic, JOURNAL_MAGIC, sizeof(h.magic));
        h.formatVersion = JOURNAL_FORMAT_V1 + 1;         // written by a newer build
        h.recordSize    = sizeof(JournalEntry);
        std::ofstream out(JOURNAL_PATH, std::ios::binary);
        out.write(reinterpret_cast<const char*>(&h), sizeof(h));
        std::vector<char> rec(sizeof(JournalEntry), '\x11');
        out.write(rec.data(), static_cast<std::streamsize>(rec.size()));
    }

    struct stat before{};
    assert(::stat(JOURNAL_PATH, &before) == 0);
    {
        Journal j(JOURNAL_PATH, Journal::SyncPolicy::Immediate, 1);
        assert(j.recoveryFailed() && "a newer format version must be refused");
        j.logAddOrder(1, 1, 0, Side::Buy, 1000, 10, OrderType::Limit);
        j.flush();
    }
    struct stat after{};
    assert(::stat(JOURNAL_PATH, &after) == 0);
    assert(before.st_size == after.st_size &&
           "a journal in an unknown format was appended to");

    // Same again for a record-size change at the SAME version — the case a
    // version bump alone would miss, and the one a struct edit actually causes.
    cleanup();
    {
        JournalFileHeader h{};
        std::memcpy(h.magic, JOURNAL_MAGIC, sizeof(h.magic));
        h.formatVersion = JOURNAL_FORMAT_V1;
        h.recordSize    = sizeof(JournalEntry) + 8;      // a field was added
        std::ofstream out(JOURNAL_PATH, std::ios::binary);
        out.write(reinterpret_cast<const char*>(&h), sizeof(h));
        std::vector<char> rec(sizeof(JournalEntry), '\x22');
        out.write(rec.data(), static_cast<std::streamsize>(rec.size()));
    }
    {
        Journal j(JOURNAL_PATH, Journal::SyncPolicy::Immediate, 1);
        assert(j.recoveryFailed() && "a changed record size must be refused");
    }

    cleanup();
    std::cout << "testFormatVersionMismatchIsRefused PASSED" << std::endl;
}

// Journals written before the header existed are a bare array of records.
// They must still read: their first byte is an entryType (1..5) and the magic
// starts with 'O', so the two are told apart with certainty, not by guessing.
void testPreHeaderJournalStillReads() {
    std::cout << "Running testPreHeaderJournalStillReads..." << std::endl;
    cleanup();

    // Build a legitimate journal, then strip its header to forge a pre-header
    // file — so the records are real rather than hand-assembled.
    {
        Journal j(JOURNAL_PATH, Journal::SyncPolicy::Immediate, 1);
        for (int i = 1; i <= 4; ++i) {
            j.logAddOrder(i, 1, 0, Side::Buy, 1000 + i, 10, OrderType::Limit);
        }
        j.flush();
    }
    const size_t hdr = Journal::headerBytesOf(JOURNAL_PATH);
    assert(hdr == sizeof(JournalFileHeader) && "a fresh journal should carry a header");

    std::vector<char> body;
    {
        std::ifstream in(JOURNAL_PATH, std::ios::binary);
        in.seekg(static_cast<std::streamoff>(hdr));
        body.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    {
        std::ofstream out(JOURNAL_PATH, std::ios::binary | std::ios::trunc);
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
    }
    assert(Journal::headerBytesOf(JOURNAL_PATH) == 0 && "forged file should look pre-header");

    {
        Journal j(JOURNAL_PATH, Journal::SyncPolicy::Immediate, 1);
        assert(!j.recoveryFailed() && "a pre-header journal must still be readable");
        auto entries = j.readAll(true, false);
        assert(entries.size() == 4 && "all four pre-header records should read back");
        assert(entries[0].orderId == 1 && entries[3].orderId == 4);
    }

    cleanup();
    std::cout << "testPreHeaderJournalStillReads PASSED" << std::endl;
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
    testBytesOnDiskTracksTheRealFile();
    testDuplicateIdAcrossSymbols();
    testUnreadableJournalRefusesToAppend();
    testTornFirstRecordStillAppends();
    testFormatVersionMismatchIsRefused();
    testPreHeaderJournalStillReads();

    std::cout << "\nALL JOURNAL CRASH TESTS PASSED!" << std::endl;
    return 0;
}
