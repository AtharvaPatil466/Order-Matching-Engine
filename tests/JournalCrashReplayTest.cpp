// JournalCrashReplayTest — crash-during-journal-write recovery, driven by the
// FaultInjector short-write point.
//
// The scenario a real deployment must survive: the primary journals a stream of
// orders, is killed (kill -9 / power loss) part-way through writing the next
// record, and restarts. On restart it must recover EXACTLY the entries that were
// durably committed before the crash — no lost order, no duplicate, no phantom
// sequence number — and resume numbering with no gap across the crash boundary.
//
// Two crash models, both portable (they exercise the fdatasync / F_FULLFSYNC
// path — the io_uring path is Linux-only and not built here):
//
//   testTornTailCrash()        kill -9 mid-fwrite of the (N+1)th record: the file
//                              ends with a torn partial entry. Recovery must stop
//                              at it, recover 1..N, and resume at N.
//   testInMemoryTailDropped()  crash before the not-yet-committed in-memory tail
//                              was ever fsync'd: only durable records survive the
//                              crash, and the restarted journal continues at M+1.
//
// FaultInjector usage: "journal.commit.short_write" is armed during the pre-crash
// write phase so committed content is produced through the torn-write-and-retry
// path (when built with -DENABLE_FAULT_INJECTION=ON); the injector's seeded PRNG
// (nextU64) chooses the byte offset of the torn tail. With fault injection OFF
// (the default Release build) shouldFail() is constexpr-false and nextU64() is 0,
// so the crash is modelled deterministically and the test still builds and passes
// on the portable path.

#include "FaultInjector.h"
#include "Journal.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace OrderMatcher;
namespace fs = std::filesystem;

namespace {

std::string tmpJournalPath(const char* tag) {
    auto p = fs::temp_directory_path() /
        ("journal_crash_replay_" + std::string(tag) + "_" +
         std::to_string(::getpid()) + ".log");
    fs::remove(p);
    fs::remove(fs::path(p.string() + ".tmp"));
    return p.string();
}

// The "state" we compare across the crash boundary: the ordered list of
// recovered orders plus their assigned sequence numbers. Two recoveries are
// equal iff they hold the same orders, in the same order, with the same
// sequence numbers — i.e. no lost order, no duplicate, no reordering.
struct RecoveredState {
    std::vector<JournalEntry> entries;

    bool operator==(const RecoveredState& other) const {
        if (entries.size() != other.entries.size()) return false;
        for (size_t i = 0; i < entries.size(); ++i) {
            const JournalEntry& a = entries[i];
            const JournalEntry& b = other.entries[i];
            if (a.sequenceNumber != b.sequenceNumber) return false;
            if (a.orderId != b.orderId) return false;
            if (a.side != b.side) return false;
            if (a.price != b.price) return false;
            if (a.quantity != b.quantity) return false;
        }
        return true;
    }
};

RecoveredState recover(const std::string& path) {
    // CRC-validated, lax on sequence: returns the CRC-valid prefix, which is
    // exactly what a real recovery reads back after a crash.
    return RecoveredState{
        Journal(path).readAll(/*validateCRC=*/true, /*validateSequence=*/false)};
}

// Assert the recovered sequence numbers are a contiguous run 1..N — no gap, no
// duplicate, no phantom — which is the "monotonically continuous across the
// crash boundary" property the recovery contract promises.
void assertContiguousSequences(const RecoveredState& s) {
    for (size_t i = 0; i < s.entries.size(); ++i) {
        assert(s.entries[i].sequenceNumber == static_cast<uint64_t>(i + 1) &&
               "recovered sequence numbers must be contiguous 1..N (no gap/dup)");
    }
}

void writeOrders(Journal& j, uint64_t first, uint64_t count) {
    for (uint64_t i = 0; i < count; ++i) {
        OrderId id = first + i;
        Side side = (id & 1) ? Side::Buy : Side::Sell;
        Price price = toPrice(100.0) + static_cast<Price>(id);
        Quantity qty = static_cast<Quantity>(10 + id);
        j.logAddOrder(id, /*pid=*/1, /*sym=*/1, side, price, qty, OrderType::Limit);
    }
}

// ── Crash model 1: torn tail (kill -9 mid-fwrite of the next record) ─────────
void testTornTailCrash() {
    std::printf("Running testTornTailCrash...\n");
    auto& fi = FaultInjector::instance();
    fi.reset();
    fi.seed(0xC0FFEEu);
    // Exercise the torn-write-and-retry path while producing committed content.
    // Disarmed before the clean flush below so all N entries persist (the
    // journal's short-write contract keeps the unwritten tail for retry).
    fi.arm("journal.commit.short_write", 0.3);

    const uint64_t kN = 200;
    const std::string path = tmpJournalPath("torn");

    RecoveredState s1;
    uint64_t committedSeq = 0;
    {
        Journal j(path, Journal::SyncPolicy::GroupCommit, /*batchSize=*/8);
        writeOrders(j, /*first=*/1, /*count=*/kN);
        // Clean shutdown of the pre-crash phase: disarm, then flush so every
        // attempted entry is durably committed. This is the "last committed
        // checkpoint" the restart must recover.
        fi.disarm("journal.commit.short_write");
        j.flush();
        s1 = RecoveredState{j.readAll(/*validateCRC=*/true, /*validateSequence=*/false)};
        committedSeq = j.getSequence();
    }
    assert(s1.entries.size() == kN && "all attempted entries must persist on clean flush");
    assert(committedSeq == kN);
    assertContiguousSequences(s1);

    // ── The crash: kill -9 while the (N+1)th record was being written. Only a
    // prefix of that record reached stable storage. Append a torn partial entry
    // whose length is chosen by the injector's seeded PRNG (deterministic, and
    // == a fixed non-zero prefix when fault injection is compiled out).
    {
        uint64_t r = fi.nextU64();
        size_t tornLen = 1 + static_cast<size_t>(r % (sizeof(JournalEntry) - 1));
        std::vector<uint8_t> torn(tornLen, 0xAB);
        FILE* f = std::fopen(path.c_str(), "ab");
        assert(f != nullptr);
        size_t w = std::fwrite(torn.data(), 1, tornLen, f);
        assert(w == tornLen);
        std::fclose(f);
    }

    // ── Restart from the last committed checkpoint & replay.
    RecoveredState s2 = recover(path);

    // (5) recovered state matches — no lost order, no duplicate.
    assert(s2.entries.size() == kN && "torn tail must not add or drop a committed entry");
    assert(s2 == s1 && "recovered state S2 must equal pre-crash committed state S1");
    // (6) sequence numbers monotonically continuous across the crash boundary.
    assertContiguousSequences(s2);

    // Restart resumes numbering exactly at the last committed sequence: the next
    // record would be N+1 (no phantom N+1 leaked by the torn write, no gap).
    uint64_t resumedSeq = Journal(path).getSequence();
    assert(resumedSeq == committedSeq &&
           "restart must resume at the last committed sequence (continuity)");

    fs::remove(path);
    std::printf("  recovered %zu of %llu committed, resumeSeq=%llu\n",
                s2.entries.size(), static_cast<unsigned long long>(kN),
                static_cast<unsigned long long>(resumedSeq));
    std::printf("testTornTailCrash PASSED\n");
}

// ── Crash model 2: uncommitted in-memory tail dropped by the crash ───────────
// Models a crash BEFORE the batched-but-not-yet-fsync'd tail was made durable.
// Only records that were fsync'd survive; the restarted journal must recover
// exactly those and then resume numbering with no gap.
void testInMemoryTailDropped() {
    std::printf("Running testInMemoryTailDropped...\n");
    auto& fi = FaultInjector::instance();
    fi.reset();
    fi.seed(0xBEEFu);
    fi.arm("journal.commit.short_write", 0.3);

    const uint64_t kCommitted = 40;   // durably flushed before the crash
    const uint64_t kUncommitted = 5;  // in batch_, never fsync'd → lost on crash
    const std::string livePath = tmpJournalPath("mem_live");
    const std::string crashPath = tmpJournalPath("mem_crash");

    RecoveredState s1;
    uint64_t committedSeq = 0;
    {
        // batchSize larger than the uncommitted tail so those log calls stay in
        // batch_ (not auto-committed) — they are the records the crash drops.
        Journal j(livePath, Journal::SyncPolicy::GroupCommit, /*batchSize=*/64);
        writeOrders(j, /*first=*/1, /*count=*/kCommitted);
        fi.disarm("journal.commit.short_write");
        j.flush();  // kCommitted now durable
        s1 = RecoveredState{j.readAll(/*validateCRC=*/true, /*validateSequence=*/false)};
        committedSeq = j.getSequence();

        // Log the tail but DO NOT flush. Snapshot the on-disk file now: this is
        // the crash image — only fsync'd bytes survive kill -9. The subsequent
        // destructor WILL flush the tail to livePath, but crashPath (captured
        // here, before that flush) models the process dying first.
        writeOrders(j, /*first=*/kCommitted + 1, /*count=*/kUncommitted);
        fs::copy_file(livePath, crashPath, fs::copy_options::overwrite_existing);
    }
    assert(s1.entries.size() == kCommitted);
    assert(committedSeq == kCommitted);
    assertContiguousSequences(s1);

    // ── Restart on the crash image & replay.
    RecoveredState s2 = recover(crashPath);
    assert(s2.entries.size() == kCommitted &&
           "uncommitted tail must not survive the crash (no phantom entries)");
    assert(s2 == s1 && "recovered state S2 must equal pre-crash committed state S1");
    assertContiguousSequences(s2);

    // The crash image ends on a whole-entry boundary, so the restarted journal
    // can cleanly continue. The next commit must get sequence M+1 — monotonically
    // continuous across the crash boundary, no gap, no reuse.
    {
        Journal j2(crashPath, Journal::SyncPolicy::GroupCommit, /*batchSize=*/1);
        assert(j2.getSequence() == committedSeq);
        writeOrders(j2, /*first=*/kCommitted + 1, /*count=*/1);
        j2.flush();
        assert(j2.getSequence() == committedSeq + 1 &&
               "post-restart commit must resume numbering at M+1 (continuity)");
    }
    RecoveredState s3 = recover(crashPath);
    assert(s3.entries.size() == kCommitted + 1 && "continuation must append exactly one entry");
    assertContiguousSequences(s3);

    fs::remove(livePath);
    fs::remove(crashPath);
    std::printf("  committed=%llu dropped=%llu, resumed at %llu\n",
                static_cast<unsigned long long>(kCommitted),
                static_cast<unsigned long long>(kUncommitted),
                static_cast<unsigned long long>(committedSeq + 1));
    std::printf("testInMemoryTailDropped PASSED\n");
}

}  // namespace

int main() {
#ifndef OB_ENABLE_FAULT_INJECTION
    std::puts("JournalCrashReplayTest: OB_ENABLE_FAULT_INJECTION not defined — the "
              "short-write fault compiles to constexpr-false, so the crash is "
              "modelled deterministically (portable path). Reconfigure with "
              "-DENABLE_FAULT_INJECTION=ON to also exercise the torn-write-retry "
              "path during the pre-crash write phase.");
#endif
    std::printf("\n=== Journal Crash-Replay Tests ===\n");

    testTornTailCrash();
    testInMemoryTailDropped();

    std::puts("\nJournalCrashReplayTest passed");
    return 0;
}
