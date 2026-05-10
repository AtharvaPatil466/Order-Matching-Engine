// JournalChaosTest — drive the journal with random torn-writes injected via
// FaultInjector and verify recovery integrity: every returned entry must be
// CRC-valid, sequences must be strictly monotone within the recovered prefix,
// and the prefix length must be bounded by what we actually attempted.
//
// Requires the OrderMatcher build to be configured with
//   cmake -DENABLE_FAULT_INJECTION=ON ..
// Otherwise the injection points compile to constexpr-false and the test
// degenerates into a sanity check; we detect that and exit early with a note.

#include "FaultInjector.h"
#include "Journal.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>

using namespace OrderMatcher;
namespace fs = std::filesystem;

namespace {

std::string tmpJournalPath() {
    auto p = fs::temp_directory_path() / ("journal_chaos_" +
        std::to_string(::getpid()) + ".log");
    fs::remove(p);
    return p.string();
}

void drive_with_faults(const std::string& path, int numOps,
                       std::mt19937_64& rng) {
    // GroupCommit batches of 16 — small enough that many batches commit during
    // the run, giving the fault point lots of opportunities to fire.
    Journal j(path, Journal::SyncPolicy::GroupCommit, /*batchSize=*/16);
    for (int i = 0; i < numOps; ++i) {
        OrderId id = static_cast<OrderId>(i + 1);
        Quantity qty = static_cast<Quantity>(1 + (rng() % 1000));
        Price price = static_cast<Price>(10000 + (rng() % 5000));
        Side side = (rng() & 1) ? Side::Buy : Side::Sell;
        j.logAddOrder(id, /*pid=*/1, /*sym=*/1, side, price, qty,
                      OrderType::Limit);
    }
    // Disarm the torn-write fault before the Journal destructor flushes.
    // The destructor's commitBatch is the LAST chance to persist any
    // suffix held in the in-memory batch; if a fault fires there, the
    // suffix is lost without a subsequent retry. In production this
    // matches a process crash mid-final-flush — those entries are
    // genuinely gone. For *this* test we want to verify "every attempted
    // entry persists on clean shutdown", which by definition means
    // shutdown completed without a fault on the last commit.
    FaultInjector::instance().disarm("journal.commit.short_write");
    // Destructor now runs an unfaulted flush of any remaining batch.
}

void verify_recovery(const std::string& path, uint64_t maxAttempted) {
    auto laxEntries = Journal(path).readAll(/*validateCRC=*/true,
                                             /*validateSequence=*/false);
    uint64_t lastSeq = 0;
    for (const auto& e : laxEntries) {
        uint32_t expected = computeCRC32(&e, offsetof(JournalEntry, checksum));
        assert(e.checksum == expected && "torn entry leaked through CRC check");
        assert(e.sequenceNumber > lastSeq && "non-monotone sequence after recovery");
        lastSeq = e.sequenceNumber;
    }
    assert(laxEntries.size() <= maxAttempted && "more entries recovered than attempted");

    auto strictEntries = Journal(path).readAll(/*validateCRC=*/true,
                                                /*validateSequence=*/true);

    // Post-fix contract: sequence assignment happens at commit time and
    // short writes keep the unwritten tail in batch_ for retry. As long as
    // the journal is closed cleanly (destructor flushes), every attempted
    // entry reaches disk and on-disk sequences are contiguous 1..N. Strict
    // therefore returns *every* CRC-valid entry — and that count equals
    // the total attempted.
    assert(strictEntries.size() == laxEntries.size() &&
           "strict and lax recovery should match — sequences must be contiguous");
    assert(strictEntries.size() == static_cast<size_t>(maxAttempted) &&
           "every attempted entry must persist on clean shutdown");
    for (size_t i = 0; i < strictEntries.size(); ++i) {
        assert(strictEntries[i].sequenceNumber == i + 1 &&
               "strict recovery returned non-contiguous sequence");
    }

    std::printf("recovered lax=%zu strict=%zu of %llu attempted (lastSeq=%llu)\n",
                laxEntries.size(), strictEntries.size(),
                static_cast<unsigned long long>(maxAttempted),
                static_cast<unsigned long long>(lastSeq));
}

}  // namespace

int main() {
#ifndef OB_ENABLE_FAULT_INJECTION
    std::puts("JournalChaosTest: OB_ENABLE_FAULT_INJECTION not defined — "
              "running degenerate sanity pass only. Reconfigure with "
              "-DENABLE_FAULT_INJECTION=ON for full coverage.");
#endif

    auto& fi = FaultInjector::instance();

    // Run several seeds so a flaky decision in one doesn't mask issues.
    for (uint64_t seed : {uint64_t{1}, uint64_t{0xC0FFEE}, uint64_t{0xDEADBEEF}}) {
        fi.reset();
        fi.seed(seed);
        // 5% torn-write probability — frequent enough to fire many times in
        // a 5000-op run, infrequent enough that we still recover most data.
        fi.arm("journal.commit.short_write", 0.05);

        std::string path = tmpJournalPath();
        std::mt19937_64 rng(seed);
        constexpr int kOps = 5000;

        drive_with_faults(path, kOps, rng);

#ifdef OB_ENABLE_FAULT_INJECTION
        uint64_t fires = fi.activations("journal.commit.short_write");
        std::printf("seed=0x%llx: torn-write fault fired %llu times\n",
                    static_cast<unsigned long long>(seed),
                    static_cast<unsigned long long>(fires));
        // With 5% over ~313 batch commits we expect ~15 fires; lower-bound
        // generously to avoid false alarms.
        assert(fires >= 3 && "fault never fired — injector wiring broken");
#endif

        verify_recovery(path, /*maxAttempted=*/kOps);
        fs::remove(path);
    }

    std::puts("JournalChaosTest passed");
    return 0;
}
