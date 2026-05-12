// JournalReplayCLI — command-line journal replay and inspection tool.
//
// Roadmap Phase 3, Week 9: Replay CLI
//
// Usage:
//   JournalReplayCLI --journal path/to/journal [options]
//
// Options:
//   --stats        Print summary statistics without replaying
//   --tail N       Show last N journal entries
//   --benchmark    Replay and measure throughput
//   --dump-orders  After replay, dump all resting orders

#include "MatchingEngine.h"
#include "Journal.h"
#include "Utils.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

using namespace OrderMatcher;

// ─── Stats mode: read journal, count entries ─────────────────────────────────

void printStats(const std::string& journalPath) {
    Journal journal(journalPath);
    auto entries = journal.readAll(true, false);

    uint64_t addOrders = 0, cancels = 0, modifies = 0;
    uint64_t cancelReplaces = 0, snapshots = 0;

    for (const auto& e : entries) {
        switch (e.entryType) {
        case JournalEntry::Type::AddOrder:       addOrders++; break;
        case JournalEntry::Type::CancelOrder:    cancels++; break;
        case JournalEntry::Type::ModifyOrder:    modifies++; break;
        case JournalEntry::Type::CancelReplace:  cancelReplaces++; break;
        case JournalEntry::Type::Snapshot:       snapshots++; break;
        default: break;
        }
    }

    std::printf("=== Journal Statistics ===\n");
    std::printf("Path:            %s\n", journalPath.c_str());
    std::printf("Total entries:   %zu\n", entries.size());
    std::printf("  Add orders:    %llu\n", (unsigned long long)addOrders);
    std::printf("  Cancels:       %llu\n", (unsigned long long)cancels);
    std::printf("  Modifies:      %llu\n", (unsigned long long)modifies);
    std::printf("  CancelReplace: %llu\n", (unsigned long long)cancelReplaces);
    std::printf("  Snapshots:     %llu\n", (unsigned long long)snapshots);
}

// ─── Tail mode: show last N entries ──────────────────────────────────────────

void printTail(const std::string& journalPath, size_t count) {
    Journal journal(journalPath);
    auto entries = journal.readAll(true, false);

    size_t start = (entries.size() > count) ? entries.size() - count : 0;

    std::printf("=== Last %zu entries (of %zu total) ===\n",
                count, entries.size());

    for (size_t i = start; i < entries.size(); ++i) {
        const auto& e = entries[i];

        const char* typeName = "Unknown";
        switch (e.entryType) {
        case JournalEntry::Type::AddOrder:       typeName = "AddOrder"; break;
        case JournalEntry::Type::CancelOrder:    typeName = "CancelOrder"; break;
        case JournalEntry::Type::ModifyOrder:    typeName = "ModifyOrder"; break;
        case JournalEntry::Type::CancelReplace:  typeName = "CancelReplace"; break;
        case JournalEntry::Type::Snapshot:       typeName = "Snapshot"; break;
        default: break;
        }

        std::printf("[%zu] %-14s sym=%llu oid=%llu pid=%llu %s p=%llu q=%llu\n",
                    i + 1,
                    typeName,
                    (unsigned long long)e.symbolId,
                    (unsigned long long)e.orderId,
                    (unsigned long long)e.participantId,
                    e.side == Side::Buy ? "BUY " : "SELL",
                    (unsigned long long)e.price,
                    (unsigned long long)e.quantity);
    }
}

// ─── Replay + benchmark ──────────────────────────────────────────────────────

void replayBenchmark(const std::string& journalPath) {
    MatchingEngine engine;
    engine.enableJournal(journalPath);
    engine.start();

    auto start = std::chrono::high_resolution_clock::now();
    size_t entries = engine.replayJournal();
    auto end = std::chrono::high_resolution_clock::now();

    double durationMs = std::chrono::duration<double, std::milli>(end - start).count();
    double entriesPerSec = (durationMs > 0)
        ? static_cast<double>(entries) / (durationMs / 1000.0) : 0;

    std::printf("=== Replay Benchmark ===\n");
    std::printf("Entries replayed: %zu\n", entries);
    std::printf("Duration:         %.1f ms\n", durationMs);
    std::printf("Throughput:       %.0f entries/sec\n", entriesPerSec);

    engine.stop();
}

// ─── Replay + dump resting orders ────────────────────────────────────────────

void replayAndDump(const std::string& journalPath) {
    MatchingEngine engine;
    engine.enableJournal(journalPath);
    engine.start();

    size_t entries = engine.replayJournal();
    std::printf("Replayed %zu entries\n\n", entries);

    std::printf("=== Resting Orders ===\n");
    for (SymbolId sym = 0; sym < 256; ++sym) {
        const OrderBook* book = engine.getOrderBook(sym);
        if (!book) continue;

        auto snap = book->getSnapshot(20);
        if (snap.bidCount == 0 && snap.askCount == 0) continue;

        std::printf("\nSymbol %llu:\n", (unsigned long long)sym);
        std::printf("  BIDS:\n");
        for (size_t i = 0; i < snap.bidCount; ++i) {
            std::printf("    %llu @ %llu (%u orders)\n",
                        (unsigned long long)snap.bids[i].totalQuantity,
                        (unsigned long long)snap.bids[i].price,
                        snap.bids[i].orderCount);
        }
        std::printf("  ASKS:\n");
        for (size_t i = 0; i < snap.askCount; ++i) {
            std::printf("    %llu @ %llu (%u orders)\n",
                        (unsigned long long)snap.asks[i].totalQuantity,
                        (unsigned long long)snap.asks[i].price,
                        snap.asks[i].orderCount);
        }
    }

    engine.stop();
}

// ─── Main ───────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    std::string journalPath;
    bool doStats = false;
    bool doBenchmark = false;
    bool doDump = false;
    size_t tailCount = 0;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--journal") == 0 && i + 1 < argc) {
            journalPath = argv[++i];
        } else if (std::strcmp(argv[i], "--stats") == 0) {
            doStats = true;
        } else if (std::strcmp(argv[i], "--benchmark") == 0) {
            doBenchmark = true;
        } else if (std::strcmp(argv[i], "--dump-orders") == 0) {
            doDump = true;
        } else if (std::strcmp(argv[i], "--tail") == 0 && i + 1 < argc) {
            tailCount = std::stoull(argv[++i]);
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("Usage: JournalReplayCLI --journal <path> [--stats] [--benchmark] [--dump-orders] [--tail N]\n");
            return 0;
        }
    }

    if (journalPath.empty()) {
        std::fprintf(stderr, "Error: --journal <path> is required\n");
        return 1;
    }

    if (doStats) {
        printStats(journalPath);
    } else if (tailCount > 0) {
        printTail(journalPath, tailCount);
    } else if (doBenchmark) {
        replayBenchmark(journalPath);
    } else if (doDump) {
        replayAndDump(journalPath);
    } else {
        replayBenchmark(journalPath);
    }

    return 0;
}
