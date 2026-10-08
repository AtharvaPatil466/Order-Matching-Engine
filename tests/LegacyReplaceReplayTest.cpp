// LegacyReplaceReplayTest — a CancelReplace record written before 1.5-E1
// replays with the meaning it had when it was written.
//
// Until 1.5-E1 a replace's newQty was the order's new LEAVES (under roadmap
// 0.11, a partially filled order could only be replaced with newQty ==
// remainingQty). It is now the new TOTAL. Replayed under the new meaning, the
// old price-only replace of "buy 100, 60 filled" — newQty 40 — reads as
// "total 40 <= 60 executed" and is refused: the recovered order sits at its old
// price while the live one had moved.
//
// Records written since carry REPLACE_NEWQTY_IS_TOTAL; older ones do not, and
// replay turns their leaves into a total against the order's fills at that
// point. The journal format is bumped to v3 so an older build refuses a file
// holding total-semantics records, and an older file is marked v3 before the
// first new record lands — after which it holds both kinds, which is why the
// meaning is per record rather than per file.

#include "Journal.h"
#include "JournalFollower.h"
#include "MatchingEngine.h"
#include "TempPath.h"

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <iostream>
#include <string>

using namespace OrderMatcher;

namespace {

constexpr SymbolId kSym = 0;
constexpr Price kPx = 10000;

// Rewrite the file as a v2 build left it: header version 2, and no replace
// record says its newQty is a total.
void makeV2(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "r+b");
    assert(f);
    JournalFileHeader h{};
    assert(std::fread(&h, sizeof(h), 1, f) == 1);
    h.formatVersion = 2;
    std::fseek(f, 0, SEEK_SET);
    assert(std::fwrite(&h, sizeof(h), 1, f) == 1);
    for (long at = sizeof(h);; at += static_cast<long>(sizeof(JournalEntry))) {
        JournalEntry e{};
        std::fseek(f, at, SEEK_SET);
        if (std::fread(&e, sizeof(e), 1, f) != 1) break;
        if (e.entryType != JournalEntry::Type::CancelReplace) continue;
        e.quantity = 0;
        e.checksum = computeCRC32(&e, offsetof(JournalEntry, checksum));
        std::fseek(f, at, SEEK_SET);
        assert(std::fwrite(&e, sizeof(e), 1, f) == 1);
    }
    std::fclose(f);
}

uint32_t headerVersion(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    JournalFileHeader h{};
    assert(f && std::fread(&h, sizeof(h), 1, f) == 1);
    std::fclose(f);
    return h.formatVersion;
}

void expectOrder(const OrderBook& book, Price px, Quantity leaves, const char* who) {
    const Order* o = book.getOrder(1);
    if (!o || o->price != px || o->remainingQty != leaves)
        std::cout << "  " << who << ": order 1 is "
                  << (o ? std::to_string(o->remainingQty) + " @ " + std::to_string(o->price)
                        : std::string("gone"))
                  << ", expected " << leaves << " @ " << px << std::endl;
    assert(o && o->price == px && o->remainingQty == leaves && o->cumQty == 60);
}

}  // namespace

int main() {
    const std::string path = uniqueTempPath("legacy_replace.wal");
    std::remove(path.c_str());

    {   // What a v2 build journaled: buy 100, sell 60, reprice keeping the 40 leaves.
        Journal j(path, Journal::SyncPolicy::Immediate);
        j.logAddOrder(1, 1, kSym, Side::Buy, kPx, 100, OrderType::Limit);
        j.logAddOrder(2, 2, kSym, Side::Sell, kPx, 60, OrderType::Limit);
        j.logCancelReplace(1, kSym, kPx - 1, 40);
        j.flush();
    }
    makeV2(path);

    {
        MatchingEngine e;
        e.addSymbol(kSym);
        assert(e.enableJournal(path) && "a version-2 journal was refused");
        e.replayJournal();
        expectOrder(*e.getOrderBook(kSym), kPx - 1, 40, "restart replay");

        // A backup fed the same records.
        MatchingEngine backup;
        backup.addSymbol(kSym);
        backup.start();
        backup.setReplayModeAllBooks(true);
        {
            Journal reader(path, Journal::SyncPolicy::Immediate);
            for (const JournalEntry& r : reader.readAll()) backup.applyReplicatedEntry(r);
        }
        expectOrder(*backup.getOrderBook(kSym), kPx - 1, 40, "backup");
        backup.stop();

        // And a follower.
        OrderBook followed(kSym);
        JournalFollower follower(path, kSym, followed);
        follower.poll();
        expectOrder(followed, kPx - 1, 40, "journal follower");

        // A new replace lands in the old file: total 100 keeps the 40 leaves.
        e.start();
        e.getOrderBook(kSym)->setCircuitBreakerThreshold(0.99);
        assert(e.cancelReplace(kSym, 1, kPx - 2, 100));
        expectOrder(*e.getOrderBook(kSym), kPx - 2, 40, "live");
        e.stop();
    }
    assert(headerVersion(path) == JOURNAL_FORMAT_CURRENT &&
           "a total-semantics record was appended to a file an older build would misread");

    {   // The file now holds a leaves record and a total record; both replay.
        MatchingEngine e;
        e.addSymbol(kSym);
        assert(e.enableJournal(path));
        e.replayJournal();
        expectOrder(*e.getOrderBook(kSym), kPx - 2, 40, "mixed-file replay");
    }

    std::remove(path.c_str());
    std::cout << "LegacyReplaceReplayTest PASSED" << std::endl;
    return 0;
}
