// CoordinatedOmissionBenchmark (P2-19) — CO-naive vs CO-corrected latency.
//
// LISTENER: attached (bench::TerminalTracker). Every timed op pays the book's
// listener work and the tracker's callbacks. Compare with HonestBenchmark
// Path L, not Path A.
//
// Closed-loop benchmarks measure latency = completion − actual_start, where
// actual_start is whenever the harness *got around to* issuing the op. When the
// system stalls, the harness stalls with it and simply issues the next op late —
// so the stall is never charged to any sample. This is "coordinated omission":
// the very requests that should show the worst latency are the ones omitted.
//
// The fix (open-loop load generation): pin an INTENDED start time for every op
// from an arrival process at --rate ops/sec, spin until the intended time, then
// process. Two arrival models (--arrivals), and the run prints which one ran:
//
//   fixed    intended[i] = t0 + i / rate. A deterministic cadence (D/G/1): the
//            best case for the queue, since no two arrivals ever bunch.
//   poisson  intended[i] = t0 + the workload's own exponential gaps (M/G/1).
//            Bursts queue behind each other, so below the knee its corrected
//            tail is 2-4x the fixed cadence's at the same rate (PERF-6, a
//            Lindley simulation fitted to the x86 service times). Real order
//            flow is burstier still.
//
// Report BOTH:
//
//   CO-naive     latency = completion − actual_start   (hides the backlog)
//   CO-corrected latency = completion − intended_start (charges the backlog)
//
// If the offered rate exceeds service capacity, actual_start drifts past
// intended_start and the corrected tail explodes while the naive tail stays
// flat — that gap is the coordinated-omission error.
//
// Usage:
//   CoordinatedOmissionBenchmark [--orders N] [--warmup N] [--seed S] [--rate R]
//                                [--arrivals fixed|poisson]

#include "OrderBook.h"
#include "BenchLatencyRecorder.h"
#include "RealisticWorkload.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

using namespace OrderMatcher;
using bench::BenchLatencyRecorder;
using bench::WorkloadConfig;
using bench::WorkloadGenerator;
using bench::WorkloadOp;

int main(int argc, char* argv[]) {
    WorkloadConfig cfg;
    cfg.newOrders = 50000;
    cfg.seed = 7;
    size_t warmup = 5000;
    double ratePerSec = 100000.0; // offered rate (100k/s)
    bool poisson = false;         // --arrivals: fixed cadence unless asked

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--orders") == 0 && i + 1 < argc)
            cfg.newOrders = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--warmup") == 0 && i + 1 < argc)
            warmup = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
            cfg.seed = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--rate") == 0 && i + 1 < argc)
            ratePerSec = std::stod(argv[++i]);
        else if (std::strcmp(argv[i], "--arrivals") == 0 && i + 1 < argc) {
            const std::string v = argv[++i];
            if (v != "fixed" && v != "poisson") {
                std::fprintf(stderr, "--arrivals takes fixed or poisson (got \"%s\")\n", v.c_str());
                return 2;
            }
            poisson = v == "poisson";
        }
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("Usage: CoordinatedOmissionBenchmark [--orders N] [--warmup N] "
                        "[--seed S] [--rate R] [--arrivals fixed|poisson]\n");
            return 0;
        }
    }

    // The workload's exponential gaps have mean 1/rate; --arrivals poisson
    // schedules by them, fixed ignores them.
    cfg.lambdaPerSec = ratePerSec;

    std::printf("=======================================================\n");
    std::printf("  Coordinated-Omission Benchmark (P2-19)\n");
    std::printf("=======================================================\n\n");
    std::printf("Configuration:\n");
    std::printf("  Ops:          %zu (New+Cancel realistic mix)\n", cfg.newOrders);
    std::printf("  Warmup:       %zu\n", warmup);
    std::printf("  Seed:         %llu\n", (unsigned long long)cfg.seed);
    std::printf("  Offered rate: %.0f ops/sec\n", ratePerSec);
    std::printf("  Arrivals:     %s\n", poisson
                    ? "poisson (exponential gaps from the workload, M/G/1)"
                    : "fixed cadence (deterministic, D/G/1: the queue's best case)");

    WorkloadGenerator gen(cfg);
    const auto& ops = gen.ops();
    const double periodNs = 1e9 / ratePerSec;

    std::printf("  Total ops:    %zu, period = %.1f ns/op\n", ops.size(), periodNs);
    std::printf("\nRunning open-loop (spin to intended start time)...\n");

    // Size the pool for the workload: the default (10,000 slots) sheds new
    // orders at 95% full, and a 200k-order run rests more than 9,500 at once.
    // No op can leave more orders live than there are ops.
    OrderBook book(0, MatchAlgorithm::PriceTime, ops.size());

    BenchLatencyRecorder recNaive;      // completion − actual_start
    BenchLatencyRecorder recCorrected;  // completion − intended_start
    uint64_t backlogSamples = 0;        // ops where actual_start ran late
    bench::OpAudit audit;
    bench::TerminalTracker tracker(ops.size());  // ids never exceed the op count
    book.setEventListener(&tracker);

    // Anchor intended-start schedule after warmup so warmup drift is excluded.
    // We still process warmup ops (to fill the book) but do not time them.
    uint64_t scheduleBase = 0;
    size_t measuredIndex = 0;
    uint64_t poissonOffsetNs = 0;  // sum of the measured ops' gaps after the first

    for (size_t i = 0; i < ops.size(); ++i) {
        const WorkloadOp& op = ops[i];
        const bool warm = i >= warmup;

        if (!warm) {
            // Warmup: process untimed to build resting liquidity.
            if (op.kind == WorkloadOp::Kind::New)
                book.addOrder(op.orderId, op.participantId, op.side,
                              op.price, op.qty, OrderType::Limit);
            else
                book.cancelOrder(op.orderId);
            continue;
        }

        // A cancel whose target a marketable order already consumed would time
        // a hash miss: skip it (no schedule slot, untimed, counted).
        if (op.kind == WorkloadOp::Kind::Cancel && !tracker.live(op.orderId)) {
            ++audit.skippedCancels;
            continue;
        }

        if (scheduleBase == 0) scheduleBase = bench::nowNs();
        if (poisson && measuredIndex > 0) poissonOffsetNs += op.gapNs;
        const uint64_t intended = scheduleBase +
            (poisson ? poissonOffsetNs
                     : static_cast<uint64_t>(periodNs * static_cast<double>(measuredIndex)));

        // Open-loop: spin until the intended issue time. If we are already
        // behind (service slower than the offered rate) this does not wait —
        // the lateness is charged to the CO-corrected sample below.
        uint64_t actualStart = bench::nowNs();
        while (actualStart < intended) actualStart = bench::nowNs();
        if (actualStart > intended + static_cast<uint64_t>(periodNs)) ++backlogSamples;

        const size_t liveBefore = book.poolInUse();
        AddOrderResult res = RejectReason::None;
        if (op.kind == WorkloadOp::Kind::New)
            res = book.addOrder(op.orderId, op.participantId, op.side,
                                op.price, op.qty, OrderType::Limit);
        else
            book.cancelOrder(op.orderId);

        uint64_t completion = bench::nowNs();
        recNaive.recordInterval(actualStart, completion);
        recCorrected.recordInterval(intended, completion);
        audit.note(op, &res, liveBefore, book.poolInUse());
        ++measuredIndex;
    }

    // The tables describe the New+Cancel mix only if every timed op did its job.
    if (!audit.ok("open-loop")) {
        std::fprintf(stderr, "WORKLOAD CHECK FAILED: a timed op did not do its job\n");
        return 1;
    }

    std::printf("\n── CO-NAIVE (completion − actual_start) ──\n");
    std::printf("  Hides backlog: late issue never charged.\n");
    recNaive.printTable("co-naive");

    std::printf("\n── CO-CORRECTED (completion − intended_start) ──\n");
    std::printf("  Charges backlog: each op owes from when it *should* have started.\n");
    recCorrected.printTable("co-corrected");

    std::printf("\n── Delta (corrected − naive) ──\n");
    std::printf("  Backlogged samples: %llu / %llu (%.1f%%)\n",
                (unsigned long long)backlogSamples, (unsigned long long)measuredIndex,
                measuredIndex ? 100.0 * (double)backlogSamples / (double)measuredIndex : 0.0);
    for (const auto& [name, p] : {std::pair{"P50", 0.50}, {"P99", 0.99}, {"P99.9", 0.999}, {"P99.99", 0.9999}})
        std::printf("  %-6s: %s\n", name,
                    BenchLatencyRecorder::deltaText(recCorrected, recNaive, p).c_str());
    std::printf("\n  If the offered rate is below service capacity the two tables\n");
    std::printf("  nearly coincide (little backlog). Raise --rate past what the box\n");
    std::printf("  can serve and the CO-corrected tail diverges upward: that gap is\n");
    std::printf("  the latency a closed-loop harness would have silently omitted.\n");
    std::printf("  (Apple-Silicon absolute numbers are NOT authoritative — x86 CI is.)\n");
    std::printf("=======================================================\n");
    return 0;
}
