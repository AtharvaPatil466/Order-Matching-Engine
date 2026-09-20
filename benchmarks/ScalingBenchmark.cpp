// ScalingBenchmark — the thread-scaling curve for the THREAD-PER-SYMBOL path.
//
// WHY THIS EXISTS
//
// Project_Overview.md claimed "horizontal scalability" and nothing in
// benchmarks/ ever measured it. ShardedBookStressTest is fixed at 4 threads and
// exercises ShardedOrderBook (price-range sharding inside ONE book) — a
// different mechanism. JournalContentionBenchmark sweeps threads but asks a
// journal question. This file asks only: as MatchingEngine async workers go
// 1 -> 2 -> 4 -> 8 -> ..., does aggregate order throughput follow?
//
// WHAT IS MEASURED
//
// MatchingEngine::startAsync(N) spawns N workers, each owning the symbols that
// hash to it (getThreadIndex = hash(symbolId) % numThreads). Every submission
// crosses one routing prologue — kill switch, running flag, a GLOBAL
// nextSubmitSequence_ fetch_add, rate/risk gates, book lookup — and then an
// enqueueSafe onto that worker's MPSC queue (another global fetch_add on
// submittedTotal_, plus a wakeup notify). That prologue is shared by every
// producer and is the obvious serialisation point.
//
// So two throughputs are reported per worker count, not one:
//
//   DISPATCH  — ops / producer-wall-time. In async mode submitOrder returns at
//               enqueue (see MatchingEngine.h:92), so this IS the route+enqueue
//               path with zero matching in it. Its latency goes through
//               bench::BenchLatencyRecorder (full P50..max).
//               CAVEAT: the workers are draining concurrently on other cores
//               while this is timed. It is the dispatch cost under load, which
//               is the number that matters, but it is not a quiesced
//               microbenchmark of the prologue alone. There is no public entry
//               point that routes without enqueuing, so a truly quiesced
//               route-only arm is not reachable without editing src/ — it is
//               deliberately not faked here.
//
//   END-TO-END — engine processedCount / total wall time including drain. This
//               is the number the "horizontal scalability" claim is about.
//               Its percentiles come from the engine's own per-worker
//               LatencyTracker (getAggregateE2ELatency), because only the
//               worker can observe completion; that tracker stops at P99.9.
//               CAVEAT: measured at SATURATION. Dispatch outruns drain by
//               design here (that is what keeps every worker busy, which is
//               what a throughput curve requires), so the queue backs up and
//               the e2e percentiles are QUEUE RESIDENCY, not service time.
//               They are reported because they show the saturation is real,
//               and they must not be quoted as a latency SLA.
//               CoordinatedOmissionBenchmark is the rate-controlled one.
//
// SYMBOL SUPPLY (the thing that decides whether this is honest)
//
// Thread-per-symbol means a worker with no symbols is an idle thread. Running
// 8 workers over 2 symbols measures idleness, not scaling. So the SCALED arm
// keeps symbols-per-worker CONSTANT (default 4) — symbol count grows with
// worker count, and the sym:worker ratio is printed on every row. The
// STARVED arm deliberately pins the symbol count at a fixed small number for
// every N; it is the pathological case, labelled as such, and its curve is
// the answer to "what happens if you add workers without adding symbols".
//
// ORDER FLOW
//
// bench::WorkloadGenerator (RealisticWorkload.h) — the same venue-shaped mix
// the honest-tail benchmarks use: power-law sizes, ~15% marketable, ~65% of
// resting orders cancelled. One stream is generated once and replayed by every
// producer against its own symbol set with its own OrderId range.
// The generator's Poisson gapNs is IGNORED on purpose: pacing arrivals at a
// fixed lambda caps throughput at lambda and would flatten every curve by
// construction. This is a saturation test, so producers run flat out. The
// mix and the size distribution are what is borrowed.
//
// The journal is OFF. Journal scaling is JournalContentionBenchmark's question;
// leaving it on here would measure fdatasync, not thread-per-symbol.
//
// CLOCK GRANULARITY
//
// Apple Silicon's steady_clock ticks at ~42 ns (README.md:114). The actual tick
// is measured at startup and printed; any latency within 3 ticks of it is
// flagged '~' in the tables — that is quantization, not signal. Throughput
// aggregates are unaffected.
//
// Usage:
//   ScalingBenchmark [--new-orders N] [--threads 1,2,4,8,11,16]
//                    [--symbols-per-worker K] [--starved-symbols K]
//                    [--seed S] [--only s|p|sp]

#include "MatchingEngine.h"
#include "BenchLatencyRecorder.h"
#include "RealisticWorkload.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace OrderMatcher;
using bench::BenchLatencyRecorder;

namespace {

// 1M slots. Deep enough that a producer never sees backpressure and the curve
// measures the engine rather than the queue's high-water mark.
constexpr size_t kQueueSize = 1u << 20;

// A latency below this many clock ticks is quantization noise, not a signal.
constexpr uint64_t kQuantizationTicks = 3;

// Mirrors MatchingEngine::getThreadIndex (private), so a producer can be
// pointed at exactly the symbols one worker owns.
size_t threadIndexFor(SymbolId sym, size_t numThreads) {
    return std::hash<SymbolId>{}(sym) % numThreads;
}

std::vector<SymbolId> symbolsForThread(size_t threadIdx, size_t numThreads, size_t wanted) {
    std::vector<SymbolId> out;
    for (SymbolId s = 1; out.size() < wanted && s < 1000000; ++s) {
        if (threadIndexFor(s, numThreads) == threadIdx) out.push_back(s);
    }
    return out;
}

// Smallest non-zero gap two back-to-back clock reads can resolve. This is the
// floor under every per-op latency in this file.
uint64_t measureClockTickNs() {
    uint64_t best = UINT64_MAX;
    for (int i = 0; i < 200000; ++i) {
        const uint64_t a = bench::nowNs();
        const uint64_t b = bench::nowNs();
        if (b > a && (b - a) < best) best = b - a;
    }
    return best == UINT64_MAX ? 0 : best;
}

struct ArmResult {
    size_t   workers{0};
    size_t   producers{0};
    size_t   symbols{0};
    uint64_t submitted{0};      // submitOrder/submitCancel calls made
    uint64_t rejected{0};       // any non-Accepted SubmitResult
    uint64_t processed{0};      // engine-side completions
    double   producerSec{0};    // go -> last producer joined
    double   totalSec{0};       // go -> waitForDrain returned
    double   dispatchRate{0};   // submitted / producerSec
    double   e2eRate{0};        // processed / totalSec
    BenchLatencyRecorder dispatch;  // merged producer-side submit latency
    uint64_t e2eP50{0}, e2eP99{0}, e2eP999{0}, e2eMax{0}, e2eCount{0};
};

// One point on the curve. `numWorkers` engine workers, one producer thread per
// worker, `symbolCount` symbols spread over the workers by the engine's own
// hash. Every producer replays `ops` against the symbols its worker owns.
ArmResult runArm(size_t numWorkers, size_t symbolCount,
                 const std::vector<bench::WorkloadOp>& ops) {
    ArmResult r;
    r.workers = numWorkers;
    r.producers = numWorkers;

    MatchingEngine engine;

    // Spread symbolCount symbols over the workers, honouring the engine's own
    // routing hash so each producer only ever feeds its own worker's queue.
    // A worker gets ceil/floor of symbolCount/numWorkers; when symbolCount <
    // numWorkers the leftover workers get NOTHING and sit idle — that is the
    // starved arm, and it is exactly the effect it is there to show.
    std::vector<std::vector<SymbolId>> perWorker(numWorkers);
    size_t assigned = 0;
    for (size_t t = 0; t < numWorkers && assigned < symbolCount; ++t) {
        const size_t remainingWorkers = numWorkers - t;
        const size_t want = (symbolCount - assigned + remainingWorkers - 1) / remainingWorkers;
        perWorker[t] = symbolsForThread(t, numWorkers, want);
        for (SymbolId s : perWorker[t]) engine.addSymbol(s);
        assigned += perWorker[t].size();
        r.symbols += perWorker[t].size();
    }

    engine.startAsync(numWorkers, kQueueSize);

    std::vector<BenchLatencyRecorder> recorders(numWorkers);
    std::atomic<uint64_t> submitted{0};
    std::atomic<uint64_t> rejected{0};
    std::atomic<bool> go{false};

    auto producer = [&](size_t t) {
        const std::vector<SymbolId>& syms = perWorker[t];
        BenchLatencyRecorder& rec = recorders[t];
        uint64_t localSubmitted = 0, localRejected = 0;
        // Disjoint OrderId range per producer so ids never collide across books.
        const OrderId idBase = static_cast<OrderId>(t + 1) * 1'000'000'000ULL;
        const ParticipantId pid = static_cast<ParticipantId>(t + 1);

        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();

        // A worker with no symbols has no producer work; it stays parked. Its
        // thread still exists and still costs a core — which is the point of
        // the starved arm.
        if (syms.empty()) return;

        for (const bench::WorkloadOp& op : ops) {
            // Symbol chosen from the ORIGINAL id so a Cancel lands on the same
            // book its New did. gapNs is ignored (saturation, see header).
            const SymbolId sym = syms[op.orderId % syms.size()];
            const OrderId id = idBase + op.orderId;

            SubmitResult sr;
            const uint64_t t0 = bench::nowNs();
            if (op.kind == bench::WorkloadOp::Kind::New) {
                sr = engine.submitOrder(sym, id, pid, op.side, op.price, op.qty,
                                        OrderType::Limit);
            } else {
                sr = engine.submitCancel(sym, id, pid);
            }
            rec.recordInterval(t0, bench::nowNs());

            ++localSubmitted;
            if (!sr.isAccepted()) ++localRejected;
        }
        submitted.fetch_add(localSubmitted, std::memory_order_relaxed);
        rejected.fetch_add(localRejected, std::memory_order_relaxed);
    };

    std::vector<std::thread> producers;
    producers.reserve(numWorkers);
    for (size_t t = 0; t < numWorkers; ++t) producers.emplace_back(producer, t);

    const auto wallStart = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);

    for (auto& p : producers) p.join();
    const auto producerEnd = std::chrono::steady_clock::now();
    engine.waitForDrain();
    const auto drainEnd = std::chrono::steady_clock::now();

    r.producerSec = std::chrono::duration<double>(producerEnd - wallStart).count();
    r.totalSec = std::chrono::duration<double>(drainEnd - wallStart).count();
    r.submitted = submitted.load();
    r.rejected = rejected.load();
    r.processed = engine.getProcessedCount();
    r.dispatchRate = r.producerSec > 0 ? static_cast<double>(r.submitted) / r.producerSec : 0.0;
    r.e2eRate = r.totalSec > 0 ? static_cast<double>(r.processed) / r.totalSec : 0.0;

    for (const auto& rec : recorders) r.dispatch.mergeFrom(rec);

    const LatencyTracker e2e = engine.getAggregateE2ELatency();
    r.e2eP50 = e2e.getP50();
    r.e2eP99 = e2e.getP99();
    r.e2eP999 = e2e.getP999();
    r.e2eMax = e2e.getMax();
    r.e2eCount = e2e.getCount();

    engine.stop();
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

uint64_t g_clockTickNs = 0;
unsigned g_cores = 0;

// '~' marks a value the clock cannot actually resolve.
char qz(uint64_t ns) {
    return (g_clockTickNs > 0 && ns < kQuantizationTicks * g_clockTickNs) ? '~' : ' ';
}

// '!' marks a row whose live thread count exceeds the core count.
const char* oversubscribed(size_t workers) {
    const size_t live = workers * 2;  // producers + workers
    if (g_cores == 0) return "";
    if (workers > g_cores) return "  [OVERSUBSCRIBED: workers > cores]";
    if (live > g_cores) return "  [OVERSUBSCRIBED: producers+workers > cores]";
    return "";
}

void printHeader() {
    std::printf("%3s | %5s | %7s | %6s | %13s | %6s | %13s | %6s | %10s | %10s | %10s | %10s\n",
                "N", "syms", "sym/wkr", "thrds", "dispatch/s", "dEff",
                "e2e ord/s", "eEff", "dP50", "dP99", "e2eP50*", "e2eP99*");
    std::printf("----+-------+---------+--------+---------------+--------+---------------"
                "+--------+------------+------------+------------+------------\n");
}

// The one caveat that decides how the e2e latency columns should be read.
void printSaturationNote() {
    std::printf("\n* e2e percentiles are measured AT SATURATION. Producers run flat out and the\n"
                "  dispatch rate exceeds the drain rate, so the request queue backs up and these\n"
                "  numbers are QUEUE RESIDENCY (backlog), not service time. That is the correct\n"
                "  regime for a THROUGHPUT curve — the worker is never starved — but it is not a\n"
                "  latency SLA and must not be quoted as one. For rate-controlled latency see\n"
                "  CoordinatedOmissionBenchmark. The dP50/dP99 columns ARE service times: they\n"
                "  time submitOrder itself, which returns at enqueue.\n");
}

void printRow(const ArmResult& r, double dispatchBase, double e2eBase) {
    const double n = static_cast<double>(r.workers);
    const double dEff = dispatchBase > 0 ? (r.dispatchRate / dispatchBase) / n : 0.0;
    const double eEff = e2eBase > 0 ? (r.e2eRate / e2eBase) / n : 0.0;
    std::printf("%3zu | %5zu | %7.2f | %6zu | %13.0f | %5.0f%% | %13.0f | %5.0f%% | "
                "%c%8lluns | %c%8lluns | %c%8lluns | %c%8lluns%s%s\n",
                r.workers, r.symbols,
                static_cast<double>(r.symbols) / n, r.workers * 2,
                r.dispatchRate, 100.0 * dEff, r.e2eRate, 100.0 * eEff,
                qz(r.dispatch.getP50()), (unsigned long long)r.dispatch.getP50(),
                qz(r.dispatch.getP99()), (unsigned long long)r.dispatch.getP99(),
                qz(r.e2eP50), (unsigned long long)r.e2eP50,
                qz(r.e2eP99), (unsigned long long)r.e2eP99,
                r.rejected ? "  [REJECTS]" : "",
                oversubscribed(r.workers));
    std::fflush(stdout);
}

void printDetail(const ArmResult& r) {
    std::printf("\n  ── N=%zu workers, %zu symbols (%.2f sym/worker), %zu live threads ──\n",
                r.workers, r.symbols, static_cast<double>(r.symbols) / static_cast<double>(r.workers),
                r.workers * 2);
    std::printf("  submitted=%llu processed=%llu rejected=%llu  producer=%.3fs total=%.3fs\n",
                (unsigned long long)r.submitted, (unsigned long long)r.processed,
                (unsigned long long)r.rejected, r.producerSec, r.totalSec);
    std::printf("  DISPATCH (route + MPSC enqueue, no matching) — bench::BenchLatencyRecorder\n");
    r.dispatch.printTable("dispatch");
    std::printf("  END-TO-END (ingress -> completion) — engine LatencyTracker, stops at P99.9\n");
    std::printf("    samples=%llu  P50=%llu ns  P99=%llu ns  P99.9=%llu ns  Max=%llu ns\n",
                (unsigned long long)r.e2eCount, (unsigned long long)r.e2eP50,
                (unsigned long long)r.e2eP99, (unsigned long long)r.e2eP999,
                (unsigned long long)r.e2eMax);
}

} // namespace

int main(int argc, char* argv[]) {
    // 200k new orders (+ ~130k generated cancels) per replayed stream keeps each
    // measured point in the hundreds-of-ms range, so thread start/join and the
    // final drain are not a material fraction of the wall time being divided.
    size_t newOrders = 200000;
    size_t symbolsPerWorker = 4;
    size_t starvedSymbols = 2;
    uint64_t seed = 12345;
    std::vector<size_t> threadCounts{1, 2, 4, 8, 11, 16};
    std::string only = "sp";
    bool detail = true;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--new-orders") == 0 && i + 1 < argc)
            newOrders = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc)
            threadCounts = parseThreadList(argv[++i]);
        else if (std::strcmp(argv[i], "--symbols-per-worker") == 0 && i + 1 < argc)
            symbolsPerWorker = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--starved-symbols") == 0 && i + 1 < argc)
            starvedSymbols = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
            seed = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--only") == 0 && i + 1 < argc)
            only = argv[++i];
        else if (std::strcmp(argv[i], "--no-detail") == 0)
            detail = false;
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("Usage: %s [--new-orders N] [--threads 1,2,4,8,11,16]\n"
                        "          [--symbols-per-worker K] [--starved-symbols K]\n"
                        "          [--seed S] [--only s|p|sp] [--no-detail]\n", argv[0]);
            return 0;
        }
    }
    if (symbolsPerWorker == 0) symbolsPerWorker = 1;

    g_cores = std::thread::hardware_concurrency();
    g_clockTickNs = measureClockTickNs();

    bench::WorkloadConfig cfg;
    cfg.seed = seed;
    cfg.newOrders = newOrders;
    const bench::WorkloadGenerator gen(cfg);
    const std::vector<bench::WorkloadOp>& ops = gen.ops();

    std::printf("=======================================================\n");
    std::printf("  Thread-Scaling Benchmark — MatchingEngine async path\n");
    std::printf("=======================================================\n\n");
    std::printf("Mechanism under test: thread-per-symbol (startAsync(N), hash(symbolId) %% N,\n");
    std::printf("                      one MPSC request queue per worker).\n");
    std::printf("                      NOT ShardedOrderBook price-range sharding.\n");
    std::printf("Journal:              OFF (journal scaling is JournalContentionBenchmark)\n\n");
    std::printf("Order flow:           bench::WorkloadGenerator (RealisticWorkload.h), seed %llu\n",
                (unsigned long long)seed);
    std::printf("  new orders/stream:  %zu   -> %zu total ops incl. cancels\n",
                newOrders, ops.size());
    std::printf("  marketable:         %zu   passive: %zu   scheduled cancels: %zu\n",
                gen.marketableCount(), gen.passiveCount(), gen.scheduledCancels());
    std::printf("  arrival pacing:     IGNORED — Poisson gaps would cap throughput at lambda.\n");
    std::printf("                      Producers run flat out; the MIX and SIZES are what is used.\n");
    std::printf("  replay:             one producer thread per worker, each replaying the same\n");
    std::printf("                      stream against its own symbols with its own OrderId range.\n\n");
    std::printf("Machine:              hardware_concurrency = %u\n", g_cores);
    std::printf("  measured clock tick = %llu ns  (README.md:114 records ~42 ns on this box)\n",
                (unsigned long long)g_clockTickNs);
    std::printf("  '~' on a latency = below %llu x the tick -> QUANTIZATION, not signal.\n",
                (unsigned long long)kQuantizationTicks);
    std::printf("  Instrumentation tax: the dispatch loop takes TWO clock reads per op, so\n");
    std::printf("  ~%llu ns/op of the dispatch RATE is the measurement, not the engine. The\n",
                (unsigned long long)(2 * g_clockTickNs));
    std::printf("  e2e throughput column is free of this — workers are the bottleneck and the\n");
    std::printf("  producers stay ahead of them even while paying it.\n");
    std::printf("  Each N runs N producer threads + N worker threads = 2N live threads, so a\n");
    std::printf("  row is flagged oversubscribed once 2N exceeds the core count, not just N.\n");
    std::printf("  Efficiency = (rate_N / rate_1) / N.  100%% = perfect linear scaling.\n\n");

    std::vector<ArmResult> scaledResults;

    if (only.find('s') != std::string::npos) {
        std::printf("─── ARM S: SCALED — symbols grow with workers (%zu per worker) ───\n",
                    symbolsPerWorker);
        std::printf("Every worker owns %zu symbols at every N, so no worker is ever idle for\n"
                    "lack of work. This is the arm the scalability claim stands or falls on.\n\n",
                    symbolsPerWorker);
        printHeader();
        double dispatchBase = 0, e2eBase = 0;
        for (size_t n : threadCounts) {
            ArmResult r = runArm(n, n * symbolsPerWorker, ops);
            if (n == threadCounts.front()) { dispatchBase = r.dispatchRate; e2eBase = r.e2eRate; }
            printRow(r, dispatchBase, e2eBase);
            scaledResults.push_back(std::move(r));
        }
        printSaturationNote();
        std::printf("\n");
    }

    if (only.find('p') != std::string::npos) {
        std::printf("─── ARM P: STARVED — %zu symbols TOTAL at every N (PATHOLOGICAL) ───\n",
                    starvedSymbols);
        std::printf("Symbol count is held fixed while workers grow, so beyond N=%zu the extra\n"
                    "workers own no symbols and park. This is NOT a scaling result; it is the\n"
                    "control that shows what under-supplying the partition costs. Any flat\n"
                    "curve here is idle threads, not a bottleneck.\n\n", starvedSymbols);
        printHeader();
        double dispatchBase = 0, e2eBase = 0;
        for (size_t n : threadCounts) {
            ArmResult r = runArm(n, starvedSymbols, ops);
            if (n == threadCounts.front()) { dispatchBase = r.dispatchRate; e2eBase = r.e2eRate; }
            printRow(r, dispatchBase, e2eBase);
        }
        printSaturationNote();
        std::printf("\n");
    }

    if (detail && !scaledResults.empty()) {
        std::printf("─── Per-point detail (SCALED arm) ───\n");
        std::printf("DISPATCH is the isolated routing cost: in async mode submitOrder returns at\n"
                    "enqueue, so nothing below is matching time. It is measured while the workers\n"
                    "drain concurrently — dispatch cost under load, not a quiesced microbenchmark.\n"
                    "No public entry point routes without enqueuing, so a quiesced route-only arm\n"
                    "is not reachable without editing src/, and is not faked here.\n");
        for (const ArmResult& r : scaledResults) printDetail(r);
        std::printf("\n");
    }

    std::printf("=======================================================\n");
    return 0;
}
