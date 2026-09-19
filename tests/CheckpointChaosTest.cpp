// CheckpointChaosTest — soak Journal::rewriteAtomically under fault
// injection and prove the atomicity contract:
//
//     After rewriteAtomically returns, the file on disk is byte-for-byte
//     either the *pre-call* state (caller saw false) or the *post-call*
//     state (caller saw true). Never partial. Never empty (unless the
//     pre-call state was empty). The on-disk log is always CRC-valid.
//
// Fault points exercised:
//   journal.checkpoint.rename_fail  — rename(2) fails; original intact
//   journal.commit.short_write       — fires inside the temp Journal's
//                                      writer callback, leaving the temp
//                                      file partial. The temp file is
//                                      cleaned up by the rename-failure
//                                      branch (no leak), and the original
//                                      stays put. Already exercised in
//                                      JournalChaosTest, here we add a
//                                      different invariant: the *original*
//                                      survives every kind of mid-rewrite
//                                      failure.

#include "FaultInjector.h"
#include "Journal.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace OrderMatcher;
namespace fs = std::filesystem;

namespace {

std::string tmpPath() {
    auto p = fs::temp_directory_path() /
        ("checkpoint_chaos_" + std::to_string(::getpid()) + ".log");
    fs::remove(p);
    fs::remove(p.string() + ".tmp");
    return p.string();
}

std::vector<uint8_t> readAllBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    return bytes;
}

// Number of CRC-valid entries readAll returns from the file. Stops at
// first corruption or EOF.
size_t countValidEntries(const std::string& path) {
    return Journal(path).readAll(/*validateCRC=*/true,
                                  /*validateSequence=*/false).size();
}

}  // namespace

int main() {
#ifndef OB_ENABLE_FAULT_INJECTION
    std::puts("CheckpointChaosTest: OB_ENABLE_FAULT_INJECTION not defined — "
              "running degenerate happy-path. Reconfigure with "
              "-DENABLE_FAULT_INJECTION=ON.");
#endif

    auto& fi = FaultInjector::instance();
    constexpr int kSeedRuns = 3;
    constexpr int kRewritesPerSeed = 30;
    constexpr int kEntriesPerRewrite = 50;

    for (uint64_t seed : {uint64_t{1}, uint64_t{0xC0FFEE}, uint64_t{0xBADF00D}}) {
        (void)kSeedRuns;
        fi.reset();
        fi.seed(seed);
        fi.arm("journal.checkpoint.rename_fail", 0.4);
        fi.arm("journal.commit.short_write",     0.05);

        std::string path = tmpPath();

        // Seed the journal with an initial known-good state so we can
        // detect any corruption on rollback.
        {
            Journal j(path, Journal::SyncPolicy::Immediate, 1);
            for (int i = 0; i < kEntriesPerRewrite; ++i) {
                j.logAddOrder(static_cast<OrderId>(i + 1), 1, 1, Side::Buy,
                              10000, 100, OrderType::Limit);
            }
        }
        std::vector<uint8_t> snapshotBefore = readAllBytes(path);
        size_t entriesBefore = countValidEntries(path);
        assert(entriesBefore == static_cast<size_t>(kEntriesPerRewrite));

        [[maybe_unused]] size_t successes = 0;
        size_t failures = 0;
        size_t curExpectedEntries = entriesBefore;

        size_t shrinkingRewrites = 0;

        for (int run = 0; run < kRewritesPerSeed; ++run) {
            // Each rewrite creates a fresh journal at the same path with a
            // record count that differs from the prior state, so a successful
            // rewrite is always distinguishable from a rolled-back one.
            //
            // THE COUNT ALTERNATES BECAUSE A REAL CHECKPOINT SHRINKS THE FILE.
            // prepareRewrite writes one Snapshot record per RESTING order,
            // which is almost always far fewer records than the history it
            // replaces — a book that has been trading all morning has orders
            // in the hundreds and a journal in the hundreds of thousands. This
            // soak previously used a monotonically increasing target, so all
            // 90 rewrites GREW the file and the shrink case — the production
            // shape — was never once exercised.
            //
            // That gap is not hypothetical. JournalFollower tracked its
            // position as an index into a re-read vector, which is correct for
            // every replacement that grows and silently freezes forever on one
            // that shrinks (fixed in 7832e89). A rewrite soak that only ever
            // grows cannot catch that class of bug in any consumer.
            int targetCount = (run % 2 == 0)
                                  ? (kEntriesPerRewrite / 4) + run  // shrink
                                  : kEntriesPerRewrite + run + 1;   // grow

            // Capture the pre-call state so we can verify rollback if
            // the call fails.
            std::vector<uint8_t> snapshotPre = readAllBytes(path);

            Journal j(path, Journal::SyncPolicy::Immediate, 1);
            bool ok = j.rewriteAtomically([&](Journal& temp) {
                for (int i = 0; i < targetCount; ++i) {
                    temp.logAddOrder(static_cast<OrderId>(i + 1), 1, 1,
                                     Side::Buy, 10000, 100, OrderType::Limit);
                }
            });

            if (ok) {
                ++successes;
                // After success the file should reflect the new content —
                // including when that content is SMALLER than what it replaced.
                size_t n = countValidEntries(path);
                assert(n == static_cast<size_t>(targetCount) &&
                       "successful rewrite must leave new content on disk");
                if (static_cast<size_t>(targetCount) < curExpectedEntries) {
                    ++shrinkingRewrites;
                }
                curExpectedEntries = static_cast<size_t>(targetCount);
            } else {
                ++failures;
                // ATOMICITY: on failure the file must be byte-identical
                // to the pre-call snapshot. No half-rewritten state.
                std::vector<uint8_t> snapshotPost = readAllBytes(path);
                if (snapshotPost != snapshotPre) {
                    std::fprintf(stderr,
                        "atomicity violated at seed=0x%llx run=%d: "
                        "pre=%zu bytes post=%zu bytes\n",
                        static_cast<unsigned long long>(seed), run,
                        snapshotPre.size(), snapshotPost.size());
                    std::abort();
                }
                // Also: the file is still CRC-valid throughout (no torn
                // entries leaked from a failed rewrite into the live file).
                size_t n = countValidEntries(path);
                assert(n == curExpectedEntries &&
                       "failed rewrite altered the live file's valid count");
            }

            // No leftover .tmp file should remain after either path.
            assert(!fs::exists(path + ".tmp") &&
                   "temp file leaked after rewriteAtomically");
        }

        // The point of the alternating target: prove the soak actually
        // produced the production shape rather than only ever growing. Without
        // this the alternation could be edited away — or defeated by an
        // unlucky failure run — and nothing would notice, which is exactly how
        // the coverage was lost the first time. Generous lower bound: under
        // fault injection a shrink only counts when the rewrite BEFORE it also
        // succeeded, so the expected count is a fraction of the even runs.
        assert(shrinkingRewrites > 0 &&
               "no rewrite ever shrank the file — the soak is not exercising "
               "the shape a real checkpoint produces");

#ifdef OB_ENABLE_FAULT_INJECTION
        uint64_t renameFires = fi.activations("journal.checkpoint.rename_fail");
        std::printf("seed=0x%llx: rewrites=%d ok=%zu failed=%zu shrank=%zu "
                    "rename_fail fires=%llu\n",
                    static_cast<unsigned long long>(seed),
                    kRewritesPerSeed, successes, failures, shrinkingRewrites,
                    static_cast<unsigned long long>(renameFires));
        // 40% × 30 trials ≈ 12 expected fires; lower-bound generously.
        assert(renameFires > 3 && "rename_fail never fired — wiring broken");
        assert(failures > 0 && "no failed rewrites — atomicity rollback path "
                                "never exercised");
#else
        // Without fault injection the contract still must hold (every
        // rewrite succeeds); just sanity-check we're not corrupting.
        assert(failures == 0);
        (void)snapshotBefore;
#endif

        fs::remove(path);
    }

    std::puts("CheckpointChaosTest passed");
    return 0;
}
