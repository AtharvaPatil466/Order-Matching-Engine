// JournalCorruptionTest — drive the journal with random bit-flip and
// fsync-fail faults injected, then verify recovery never returns a corrupted
// entry. The CRC field in each JournalEntry is the only line of defense
// against in-flight memory corruption between log time and disk landing; this
// test fails closed if it stops being effective.

#include "FaultInjector.h"
#include "Journal.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace OrderMatcher;
namespace fs = std::filesystem;

namespace {

std::string tmpJournalPath(const char* tag) {
    auto p = fs::temp_directory_path() /
        ("journal_corruption_" + std::string(tag) + "_" +
         std::to_string(::getpid()) + ".log");
    fs::remove(p);
    return p.string();
}

void drive(const std::string& path, int numOps) {
    Journal j(path, Journal::SyncPolicy::GroupCommit, /*batchSize=*/16);
    for (int i = 0; i < numOps; ++i) {
        j.logAddOrder(static_cast<OrderId>(i + 1), 1, 1, Side::Buy,
                      10000, 100, OrderType::Limit);
    }
}

void verify_no_corrupt_entry_returned(const std::string& path,
                                      uint64_t maxAttempted) {
    auto entries = Journal(path).readAll(/*validateCRC=*/true,
                                          /*validateSequence=*/false);
    uint64_t lastSeq = 0;
    for (const auto& e : entries) {
        uint32_t expected = computeCRC32(&e, offsetof(JournalEntry, checksum));
        assert(e.checksum == expected && "corrupt entry slipped past CRC");
        assert(e.sequenceNumber > lastSeq && "non-monotone sequence");
        lastSeq = e.sequenceNumber;
    }
    assert(entries.size() <= maxAttempted);
    std::printf("  recovered %zu of %llu (lastSeq=%llu)\n",
                entries.size(),
                static_cast<unsigned long long>(maxAttempted),
                static_cast<unsigned long long>(lastSeq));
}

}  // namespace

int main() {
#ifndef OB_ENABLE_FAULT_INJECTION
    std::puts("JournalCorruptionTest: OB_ENABLE_FAULT_INJECTION not defined — "
              "skipping. Reconfigure with -DENABLE_FAULT_INJECTION=ON.");
    return 0;
#endif

    auto& fi = FaultInjector::instance();

    // ---- Test 1: bit-flip ---------------------------------------------------
    // High probability so most batches get one corrupt entry. The CRC must
    // catch every one — the readAll loop stops at the first invalid entry,
    // so the recovered prefix is bounded by the position of the first flip.
    for (uint64_t seed : {uint64_t{1}, uint64_t{0xC0FFEE}, uint64_t{0xDEADBEEF}}) {
        fi.reset();
        fi.seed(seed);
        fi.arm("journal.commit.bit_flip", 0.5);  // 50% per batch

        std::string path = tmpJournalPath("bitflip");
        constexpr int kOps = 5000;
        drive(path, kOps);

        uint64_t fires = fi.activations("journal.commit.bit_flip");
        std::printf("seed=0x%llx bit_flip: fired %llu times\n",
                    static_cast<unsigned long long>(seed),
                    static_cast<unsigned long long>(fires));
        // ~313 batches × 50% ≈ 156 fires expected. Lower-bound generously.
        assert(fires > 30 && "bit_flip never fired enough — wiring broken");

        verify_no_corrupt_entry_returned(path, kOps);

        // The corrupted entry stops the recovery, so the recovered prefix
        // must be strictly less than what we wrote when faults fired at all.
        auto entries = Journal(path).readAll(true, false);
        assert(entries.size() < static_cast<size_t>(kOps) &&
               "expected at least one fault to truncate recovery");

        fs::remove(path);
    }

    // ---- Test 2: fsync_fail -------------------------------------------------
    // Not falsifiable in user space (data still in buffer cache), but the
    // injector counter must show fires when armed — that's the wiring check.
    {
        fi.reset();
        fi.seed(0x5EED);
        fi.arm("journal.commit.fsync_fail", 0.3);

        std::string path = tmpJournalPath("fsync");
        constexpr int kOps = 2000;
        drive(path, kOps);

        uint64_t fires = fi.activations("journal.commit.fsync_fail");
        std::printf("fsync_fail: fired %llu times\n",
                    static_cast<unsigned long long>(fires));
        assert(fires > 10 && "fsync_fail never fired — wiring broken");

        // With fsync skipped but data still flushed and CRCs intact, the
        // file should still be readable cleanly post-clean-exit.
        auto entries = Journal(path).readAll(true, true);
        assert(entries.size() == kOps &&
               "fsync_fail must not lose data on clean exit");
        fs::remove(path);
    }

    std::puts("JournalCorruptionTest passed");
    return 0;
}
