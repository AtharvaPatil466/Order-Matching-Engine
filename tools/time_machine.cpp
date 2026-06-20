// TimeMachine — regulatory audit / postmortem replay tool.
//
// Replays a journal WAL file entry-by-entry and prints the L2 order book
// state at a given point in time (--to <unix-ns>) or at end-of-journal.
//
// Usage:
//   ./bin/TimeMachine --journal /path/to/journal.wal [--to <unix-ns>] [--depth 5]

#include "MatchingEngine.h"
#include "Journal.h"
#include "Types.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

using namespace OrderMatcher;

static void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s --journal <file> [--to <unix-ns>] [--depth <N>]\n"
        "\n"
        "  --journal FILE   path to the journal WAL file (required)\n"
        "  --to NS          stop replaying at this Unix nanosecond timestamp\n"
        "                   (default: replay entire journal)\n"
        "  --depth N        price levels to show on each side (default: 5)\n",
        prog);
}

int main(int argc, char* argv[]) {
    std::string journalPath;
    uint64_t stopTimestamp = UINT64_MAX;  // default: replay everything
    size_t   depth         = 5;

    // ── Argument parsing ──────────────────────────────────────────────────────
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--journal") == 0 && i + 1 < argc) {
            journalPath = argv[++i];
        } else if (std::strcmp(argv[i], "--to") == 0 && i + 1 < argc) {
            stopTimestamp = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--depth") == 0 && i + 1 < argc) {
            depth = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
            if (depth == 0) depth = 1;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (journalPath.empty()) {
        std::fprintf(stderr, "Error: --journal <file> is required\n");
        usage(argv[0]);
        return 1;
    }

    std::printf("[TimeMachine] Replaying journal: %s\n", journalPath.c_str());
    if (stopTimestamp != UINT64_MAX) {
        std::printf("[TimeMachine] Stopping at ts=%llu\n",
                    (unsigned long long)stopTimestamp);
    }

    // ── Read journal entries from disk ────────────────────────────────────────
    // Open read-only: we only need the on-disk bytes, no writes.
    Journal journal(journalPath, Journal::SyncPolicy::GroupCommit, 64);
    auto entries = journal.readAll(/*validateCRC=*/true, /*validateSequence=*/false);

    if (entries.empty()) {
        std::fprintf(stderr, "[TimeMachine] No valid entries found in journal.\n");
        return 1;
    }

    // ── Replay into a fresh engine ────────────────────────────────────────────
    MatchingEngine engine;
    engine.start();
    engine.setReplayModeAllBooks(true);  // suppress local market-data callbacks

    size_t replayed     = 0;
    uint64_t lastTs     = 0;

    for (const auto& entry : entries) {
        // Stop-at-timestamp gate: apply entries whose timestamp <= stopTimestamp.
        if (entry.timestamp > stopTimestamp) {
            break;
        }

        engine.applyReplicatedEntry(entry);
        lastTs = entry.timestamp;
        ++replayed;
    }

    engine.setReplayModeAllBooks(false);

    // ── Summary ───────────────────────────────────────────────────────────────
    std::printf("[TimeMachine] Replayed %zu entries", replayed);
    if (lastTs > 0) {
        std::printf(", stopped at ts=%llu", (unsigned long long)lastTs);
    }
    std::printf("\n\n");

    // ── Print L2 book state ───────────────────────────────────────────────────
    // Clamp depth to MarketDataSnapshot::MAX_DEPTH
    if (depth > MarketDataSnapshot::MAX_DEPTH) {
        depth = MarketDataSnapshot::MAX_DEPTH;
    }

    std::printf("=== Book State ===\n");

    bool anyBook = false;
    // Iterate symbol IDs 0..1023; skip symbols that were never registered.
    for (SymbolId sym = 0; sym < 1024; ++sym) {
        const OrderBook* book = engine.getOrderBook(sym);
        if (!book) continue;

        MarketDataSnapshot snap = engine.getSnapshot(sym, depth);
        if (snap.bidCount == 0 && snap.askCount == 0) continue;

        anyBook = true;
        std::printf("\nSymbol %u:\n", sym);

        // Print asks in reverse order (highest ask first, descending to best ask).
        for (size_t i = snap.askCount; i > 0; --i) {
            const auto& lvl = snap.asks[i - 1];
            std::printf("  ASK %2zu: %10.4f x %llu\n",
                        i,
                        toDouble(lvl.price),
                        (unsigned long long)lvl.totalQuantity);
        }

        // Print bids in order (best bid first, descending).
        for (size_t i = 0; i < snap.bidCount; ++i) {
            const auto& lvl = snap.bids[i];
            std::printf("  BID %2zu: %10.4f x %llu\n",
                        i + 1,
                        toDouble(lvl.price),
                        (unsigned long long)lvl.totalQuantity);
        }
    }

    if (!anyBook) {
        std::printf("  (no resting orders at this point in time)\n");
    }

    engine.stop();
    return 0;
}
