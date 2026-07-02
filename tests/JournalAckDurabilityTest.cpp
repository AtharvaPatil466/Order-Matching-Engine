// JournalAckDurabilityTest — proves the on-commit replication ack is gated on
// durability. The onCommit_ callback ships committed bytes to the replication
// backup; it must fire ONLY AFTER the durable sync (fdatasync / F_FULLFSYNC)
// has actually happened. If it fired before — or when the sync was skipped /
// failed — the backup would ack entries the primary never durably wrote. On a
// primary crash between flush and fsync those entries vanish on restart and the
// backup diverges (the classic ack-before-fsync hazard).
//
// Mechanism: the FaultInjector key "journal.commit.fsync_fail" makes commitBatch
// skip the durability barrier (modelling an fsync that did not make data
// durable). With the fault armed, NO bytes may be acked to the backup. With the
// fault disarmed, exactly one ack must fire per commit, strictly after the sync.
//
// Requires: cmake -DENABLE_FAULT_INJECTION=ON .. (otherwise the injection point
// compiles to constexpr-false and there is nothing to falsify — we skip).

#include "FaultInjector.h"
#include "Journal.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace OrderMatcher;
namespace fs = std::filesystem;

namespace {

std::string tmpJournalPath(const char* tag) {
    auto p = fs::temp_directory_path() /
        ("journal_ack_durability_" + std::string(tag) + "_" +
         std::to_string(::getpid()) + ".log");
    fs::remove(p);
    return p.string();
}

// A tiny stand-in for the replication coordinator: it records every byte the
// journal acks as "durable" to the backup.
struct AckSink {
    uint64_t acks = 0;       // number of onCommit invocations
    uint64_t entries = 0;    // total entries acked across all invocations
    void operator()(const JournalEntry*, size_t count) {
        ++acks;
        entries += count;
    }
};

// ---- Success path: a real sync happens → exactly one ack fires -------------
// [[maybe_unused]]: only called from main() under OB_ENABLE_FAULT_INJECTION;
// without it the file must still compile clean (-Werror=unused-function).
[[maybe_unused]] void testAckFiresAfterSuccessfulSync() {
    std::printf("Running testAckFiresAfterSuccessfulSync...\n");
    auto& fi = FaultInjector::instance();
    fi.reset();  // no faults armed — syncFile() runs and succeeds

    AckSink sink;
    std::string path = tmpJournalPath("ok");
    {
        // Immediate policy + batchSize 1: every log triggers a full commit.
        Journal j(path, Journal::SyncPolicy::Immediate, /*batchSize=*/1);
        j.setOnCommit([&sink](const JournalEntry* e, size_t n) { sink(e, n); });

        j.logAddOrder(1, 1, 1, Side::Buy, 10000, 100, OrderType::Limit);
        j.flush();
        // Async path: flush() submits + drains, but make the completion
        // dependency explicit — block until the reaper has reaped the
        // write→fdatasync chain and fired onCommit before observing `sink`.
        j.quiesce();

        // The durable sync succeeded, so the ack must have fired exactly once
        // for exactly one entry.
        assert(sink.acks == 1 && "onCommit must fire exactly once after a durable sync");
        assert(sink.entries == 1 && "onCommit must ack exactly the committed entry");
    }
    fs::remove(path);
    std::printf("  ok: acks=%llu entries=%llu\n",
                static_cast<unsigned long long>(sink.acks),
                static_cast<unsigned long long>(sink.entries));
    std::printf("testAckFiresAfterSuccessfulSync PASSED\n");
}

// ---- Hazard path: the durable sync is skipped → NO ack may fire ------------
[[maybe_unused]] void testNoAckWhenSyncFails() {
    std::printf("Running testNoAckWhenSyncFails...\n");
    auto& fi = FaultInjector::instance();
    fi.reset();
    fi.seed(0xACDC);
    // Probability 1.0: the durability barrier is skipped on every commit.
    fi.arm("journal.commit.fsync_fail", 1.0);

    AckSink sink;
    std::string path = tmpJournalPath("fsyncfail");
    {
        Journal j(path, Journal::SyncPolicy::Immediate, /*batchSize=*/1);
        j.setOnCommit([&sink](const JournalEntry* e, size_t n) { sink(e, n); });

        j.logAddOrder(1, 1, 1, Side::Buy, 10000, 100, OrderType::Limit);
        j.flush();
        // Async path: drain the (write-only, no-ack) chain so the assertion
        // below observes the reaper's final decision — which must be NO ack.
        j.quiesce();

        // The fault must actually have fired — otherwise the test proves nothing.
        uint64_t fires = fi.activations("journal.commit.fsync_fail");
        std::printf("  fsync_fail fired %llu times\n",
                    static_cast<unsigned long long>(fires));
        assert(fires > 0 && "fsync_fail never fired — injector wiring broken");

        // THE CONTRACT: nothing was durably synced, so nothing may be acked to
        // the backup. A non-zero ack count here is the ack-before-fsync hazard.
        assert(sink.acks == 0 &&
               "ack-before-fsync hazard: backup acked an entry the primary did "
               "not durably sync");
        assert(sink.entries == 0 && "no entries may be acked when sync is skipped");
    }

    // The write itself still reached the page cache (fwrite + fflush), so on a
    // *clean* process exit the data is readable — we only withhold the ACK, we
    // do not drop the write. This confirms the fix gates the ack, not the I/O.
    fi.disarm();
    auto entries = Journal(path).readAll(/*validateCRC=*/true, /*validateSequence=*/false);
    assert(entries.size() == 1 &&
           "data must still be on disk after clean exit — only the ack is gated");

    fs::remove(path);
    std::printf("testNoAckWhenSyncFails PASSED\n");
}

}  // namespace

int main() {
#ifndef OB_ENABLE_FAULT_INJECTION
    std::puts("JournalAckDurabilityTest: OB_ENABLE_FAULT_INJECTION not defined — "
              "skipping. Reconfigure with -DENABLE_FAULT_INJECTION=ON.");
    return 0;
#else
    std::printf("\n=== Journal Ack-Durability Tests ===\n");
    testAckFiresAfterSuccessfulSync();
    testNoAckWhenSyncFails();
    std::puts("\nJournalAckDurabilityTest passed");
    return 0;
#endif
}
