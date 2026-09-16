// JournalContentionBenchmark — measures audit finding H6:
//   "all appends still serialise on one global journalMutex_"
//
// Two experiments, both scanning worker-thread count N:
//
//   A) END-TO-END: MatchingEngine in async mode, N workers, saturating
//      multi-symbol order flow, journal ON vs journal OFF. The gap between
//      the two curves as N rises is the cost the journal path imposes.
//
//   B) CRITICAL SECTION ONLY: N threads hammering exactly the lock pattern
//      MatchingEngine::processRequest uses — lock/logAddOrder/unlock, then
//      lock/needsCheckpoint/unlock — against a real Journal. No books, no
//      queues, no allocation outside the journal. This is the ceiling of the
//      serialised region in isolation, and it reports the fraction of
//      acquisitions that found the lock already held (via try_lock, which
//      costs the same atomic exchange the lock itself does).
//
//      B runs in two flavours: with fdatasync (production) and with the
//      existing "journal.commit.fsync_fail" fault armed, which skips the
//      durability barrier. The difference separates "serialised on CPU work"
//      from "serialised on the disk".
//
// Usage: JournalContentionBenchmark [--orders N] [--threads 1,2,4,8]
//                                   [--symbols-per-thread K] [--only a|b]

#include "MatchingEngine.h"
#include "Journal.h"
#include "FaultInjector.h"
#include "BenchLatencyRecorder.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace OrderMatcher;
using bench::BenchLatencyRecorder;

namespace {

constexpr size_t kQueueSize = 1u << 20;   // 1M slots: deep enough that
                                          // producers never see backpressure
constexpr const char* kJournalDir = "/tmp";

uint64_t nowNanos() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Mirrors MatchingEngine::getThreadIndex (private), so a producer can be
// pointed at exactly one worker's symbols.
size_t threadIndexFor(SymbolId sym, size_t numThreads) {
    return std::hash<SymbolId>{}(sym) % numThreads;
}

std::vector<SymbolId> symbolsForThread(size_t threadIdx, size_t numThreads,
                                       size_t wanted) {
    std::vector<SymbolId> out;
    for (SymbolId s = 1; out.size() < wanted && s < 100000; ++s) {
        if (threadIndexFor(s, numThreads) == threadIdx) out.push_back(s);
    }
    return out;
}

struct ArmResult {
    double   throughput{0};      // orders/sec, engine-processed
    uint64_t submitP50{0};       // producer-side enqueue latency
    uint64_t submitP99{0};
    uint64_t e2eP50{0};          // engine ingress -> completion
    uint64_t e2eP99{0};
    uint64_t e2eP999{0};
    uint64_t backpressure{0};
    uint64_t processed{0};
    uint64_t journalAppends{0};   // sanity: how many orders actually journaled
};

// ─── Experiment A: end-to-end engine ────────────────────────────────────────

ArmResult runEngineArm(size_t numThreads, size_t symbolsPerThread,
                       size_t ordersPerThread, bool journalOn) {
    const std::string journalPath =
        std::string(kJournalDir) + "/h6_bench_" + std::to_string(numThreads) + ".journal";
    std::remove(journalPath.c_str());

    MatchingEngine engine;

    std::vector<std::vector<SymbolId>> perThreadSymbols(numThreads);
    for (size_t t = 0; t < numThreads; ++t) {
        perThreadSymbols[t] = symbolsForThread(t, numThreads, symbolsPerThread);
        for (SymbolId s : perThreadSymbols[t]) engine.addSymbol(s);
    }

    if (journalOn) engine.enableJournal(journalPath);
    engine.startAsync(numThreads, kQueueSize);

    std::vector<BenchLatencyRecorder> recorders(numThreads);
    std::atomic<uint64_t> backpressure{0};
    std::atomic<bool> go{false};

    auto producer = [&](size_t t) {
        const auto& syms = perThreadSymbols[t];
        std::mt19937_64 rng(1234 + t);
        // Wide price band so nearly every order RESTS rather than crossing: a
        // resting add is what journals, so this maximises journal pressure.
        std::uniform_int_distribution<Price> priceDist(1'000'00, 1'100'00);
        std::uniform_int_distribution<Quantity> qtyDist(1, 100);
        BenchLatencyRecorder& rec = recorders[t];
        OrderId nextId = static_cast<OrderId>(t + 1) * 100'000'000ULL;
        uint64_t localBp = 0;

        // Cancel ring: keeps ~kLiveDepth orders resting per producer so the
        // order pool never fills. Without this the books saturate, every
        // further add is rejected with PoolCapacityExceeded, and NOTHING
        // journals — which silently turns the "journal ON" arm into a no-op.
        constexpr size_t kLiveDepth = 2000;
        std::vector<std::pair<SymbolId, OrderId>> live(kLiveDepth, {0, 0});

        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();

        for (size_t i = 0; i < ordersPerThread; ++i) {
            const SymbolId sym = syms[i % syms.size()];
            const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
            const OrderId id = nextId++;

            const uint64_t t0 = nowNanos();
            SubmitResult sr = engine.submitOrder(sym, id,
                                                 static_cast<ParticipantId>(t + 1),
                                                 side, priceDist(rng), qtyDist(rng),
                                                 OrderType::Limit);
            rec.recordInterval(t0, nowNanos());
            if (!sr.isAccepted()) ++localBp;

            auto& slot = live[i % kLiveDepth];
            if (slot.second != 0) {
                // Evicting the oldest: cancel it so the book stays bounded.
                // A no-longer-resting order just returns early in the worker.
                engine.submitCancel(slot.first, slot.second,
                                    static_cast<ParticipantId>(t + 1));
            }
            slot = {sym, id};
        }
        backpressure.fetch_add(localBp, std::memory_order_relaxed);
    };

    std::vector<std::thread> producers;
    producers.reserve(numThreads);
    for (size_t t = 0; t < numThreads; ++t) producers.emplace_back(producer, t);

    const auto wallStart = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);

    for (auto& p : producers) p.join();
    engine.waitForDrain();
    const double wallSec =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();

    ArmResult r;
    r.processed = engine.getProcessedCount();
    r.throughput = static_cast<double>(r.processed) / wallSec;
    r.backpressure = backpressure.load();
    if (auto* j = engine.getJournal()) r.journalAppends = j->entriesAppended();

    BenchLatencyRecorder merged;
    for (auto& rec : recorders) merged.mergeFrom(rec);
    r.submitP50 = merged.getP50();
    r.submitP99 = merged.getP99();

    auto e2e = engine.getAggregateE2ELatency();
    r.e2eP50 = e2e.getP50();
    r.e2eP99 = e2e.getP99();
    r.e2eP999 = e2e.getP999();

    engine.stop();
    std::remove(journalPath.c_str());
    return r;
}

// ─── Experiment B: the serialised region, alone ─────────────────────────────

struct LockResult {
    double   appendsPerSec{0};
    double   contendedPct{0};   // acquisitions that found the lock held
    uint64_t waitP50{0};        // ns spent waiting for a contended acquire
    uint64_t waitP99{0};
};

// sharedJournal=false gives each thread its OWN Journal and its OWN mutex —
// the "what if the global lock were gone" reference. Everything else identical.
LockResult runLockArm(size_t numThreads, size_t appendsPerThread, size_t batchSize,
                      bool sharedJournal, bool doubleLock = true) {
    const size_t instances = sharedJournal ? 1 : numThreads;
    std::vector<std::string> paths;
    std::vector<std::unique_ptr<Journal>> journals;
    for (size_t i = 0; i < instances; ++i) {
        paths.push_back(std::string(kJournalDir) + "/h6_lock_" +
                        std::to_string(numThreads) + "_" + std::to_string(i) + ".journal");
        std::remove(paths.back().c_str());
        // Production settings, exactly as MatchingEngine::enableJournal builds it.
        journals.push_back(std::make_unique<Journal>(paths.back(),
                                                     Journal::SyncPolicy::GroupCommit,
                                                     batchSize));
    }
    std::vector<std::mutex> mutexes(instances);

    std::atomic<uint64_t> contended{0};
    std::atomic<bool> go{false};
    std::vector<BenchLatencyRecorder> waits(numThreads);
    double sec = 0;

    {
        auto worker = [&](size_t t) {
            const size_t slot = sharedJournal ? 0 : t;
            Journal& journal = *journals[slot];
            std::mutex& journalMutex = mutexes[slot];
            BenchLatencyRecorder& rec = waits[t];
            uint64_t localContended = 0;
            OrderId nextId = static_cast<OrderId>(t + 1) * 100'000'000ULL;

            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();

            for (size_t i = 0; i < appendsPerThread; ++i) {
                // try_lock first: on an uncontended mutex this is the same
                // atomic exchange lock() would do, so the fast path pays ~zero
                // extra. Only a genuine miss costs an extra timestamp pair.
                if (!journalMutex.try_lock()) {
                    ++localContended;
                    const uint64_t t0 = nowNanos();
                    journalMutex.lock();
                    rec.recordInterval(t0, nowNanos());
                }
                journal.logAddOrder(nextId++, static_cast<ParticipantId>(t + 1),
                                    static_cast<SymbolId>(t + 1), Side::Buy,
                                    1'000'00 + static_cast<Price>(i % 1000), 100,
                                    OrderType::Limit);
                (void)journal.entriesAppended();
                journalMutex.unlock();

                // maybeTriggerAutoCheckpoint(): a SECOND acquisition of the same
                // global mutex on every single journaled operation. doubleLock
                // =false models folding that check into the append's own
                // critical section.
                if (doubleLock) {
                    if (!journalMutex.try_lock()) {
                        ++localContended;
                        const uint64_t t0 = nowNanos();
                        journalMutex.lock();
                        rec.recordInterval(t0, nowNanos());
                    }
                    (void)journal.needsCheckpoint(250000, 64u * 1024 * 1024);
                    journalMutex.unlock();
                }
            }
            contended.fetch_add(localContended, std::memory_order_relaxed);
        };

        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        for (size_t t = 0; t < numThreads; ++t) threads.emplace_back(worker, t);

        const auto start = std::chrono::steady_clock::now();
        go.store(true, std::memory_order_release);
        for (auto& th : threads) th.join();
        sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }

    LockResult r;
    const double totalAppends = static_cast<double>(numThreads * appendsPerThread);
    r.appendsPerSec = totalAppends / sec;
    r.contendedPct = 100.0 * static_cast<double>(contended.load())
                     / (totalAppends * (doubleLock ? 2.0 : 1.0));
    BenchLatencyRecorder merged;
    for (auto& w : waits) merged.mergeFrom(w);
    r.waitP50 = merged.getP50();
    r.waitP99 = merged.getP99();

    journals.clear();   // close before unlinking
    for (const auto& p : paths) std::remove(p.c_str());
    return r;
}

std::vector<size_t> parseThreadList(const char* s) {
    std::vector<size_t> out;
    const char* p = s;
    while (*p) {
        char* end = nullptr;
        unsigned long v = std::strtoul(p, &end, 10);
        if (end == p) break;
        if (v > 0) out.push_back(static_cast<size_t>(v));
        p = (*end == ',') ? end + 1 : end;
    }
    if (out.empty()) out = {1, 2, 4, 8};
    return out;
}

} // namespace

int main(int argc, char* argv[]) {
    size_t ordersPerThread = 200000;
    size_t symbolsPerThread = 4;
    std::vector<size_t> threadCounts{1, 2, 4, 8};
    std::string only = "ab";

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--orders") == 0 && i + 1 < argc)
            ordersPerThread = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc)
            threadCounts = parseThreadList(argv[++i]);
        else if (std::strcmp(argv[i], "--symbols-per-thread") == 0 && i + 1 < argc)
            symbolsPerThread = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--only") == 0 && i + 1 < argc)
            only = argv[++i];
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("Usage: %s [--orders N] [--threads 1,2,4,8] "
                        "[--symbols-per-thread K] [--only a|b|ab]\n", argv[0]);
            return 0;
        }
    }

    std::printf("=== H6: journalMutex_ contention ===\n");
    std::printf("Orders per producer thread: %zu\n", ordersPerThread);
    std::printf("Symbols per worker thread:  %zu\n", symbolsPerThread);
    std::printf("Journal config:             GroupCommit, batch=64 "
                "(MatchingEngine::enableJournal default)\n");
    std::printf("Hardware concurrency:       %u\n\n", std::thread::hardware_concurrency());

    if (only.find('a') != std::string::npos) {
        std::printf("--- A: end-to-end engine throughput (saturating) ---\n");
        std::printf("%3s | %-7s | %12s | %8s | %8s | %8s | %10s | %10s | %s\n",
                    "N", "journal", "orders/sec", "scaling", "subP50", "subP99",
                    "e2eP50", "e2eP99", "appends/processed");
        std::printf("----+---------+--------------+----------+----------+----------"
                    "+------------+------------+------------------\n");
        double base[2] = {0, 0};
        for (size_t n : threadCounts) {
            for (int j = 0; j < 2; ++j) {
                const bool journalOn = (j == 1);
                ArmResult r = runEngineArm(n, symbolsPerThread, ordersPerThread, journalOn);
                if (n == threadCounts.front()) base[j] = r.throughput;
                std::printf("%3zu | %-7s | %12.0f | %7.2fx | %6lluns | %6lluns | "
                            "%8lluns | %8lluns | %llu/%llu%s\n",
                            n, journalOn ? "ON" : "off", r.throughput,
                            base[j] > 0 ? r.throughput / base[j] : 0.0,
                            (unsigned long long)r.submitP50,
                            (unsigned long long)r.submitP99,
                            (unsigned long long)r.e2eP50,
                            (unsigned long long)r.e2eP99,
                            (unsigned long long)r.journalAppends,
                            (unsigned long long)r.processed,
                            r.backpressure ? "  [BACKPRESSURE]" : "");
                std::fflush(stdout);
            }
        }
        std::printf("\n");
    }

    if (only.find('b') != std::string::npos) {
        std::printf("--- B: journalMutex_ critical section in isolation ---\n");
        std::printf("(batch=64 is the production default; batch=4096 amortises "
                    "fdatasync away so the\n lock itself becomes visible. "
                    "'per-thread' = one Journal+mutex each, i.e. no global lock.)\n\n");
        std::printf("%3s | %-9s | %-10s | %12s | %8s | %10s | %9s | %10s\n",
                    "N", "batch", "journal", "appends/sec", "scaling", "contended",
                    "waitP50", "waitP99");
        std::printf("----+-----------+------------+--------------+----------+------------"
                    "+-----------+-----------\n");
        struct Mode { size_t batch; bool shared; bool dbl; const char* label; };
        const Mode modes[] = {
            {64,   true,  true,  "GLOBAL x2"},
            {4096, true,  true,  "GLOBAL x2"},
            {4096, true,  false, "GLOBAL x1"},
            {4096, false, true,  "per-thread"},
        };
        for (const Mode& m : modes) {
            double base = 0;
            for (size_t n : threadCounts) {
                LockResult r = runLockArm(n, ordersPerThread, m.batch, m.shared, m.dbl);
                if (n == threadCounts.front()) base = r.appendsPerSec;
                std::printf("%3zu | %-9zu | %-10s | %12.0f | %7.2fx | %9.2f%% | %7lluns | %8lluns\n",
                            n, m.batch, m.label, r.appendsPerSec,
                            base > 0 ? r.appendsPerSec / base : 0.0,
                            r.contendedPct,
                            (unsigned long long)r.waitP50,
                            (unsigned long long)r.waitP99);
                std::fflush(stdout);
            }
            std::printf("\n");
        }
    }

    return 0;
}
