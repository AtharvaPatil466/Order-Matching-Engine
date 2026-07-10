// CoordinatedOmissionBenchmark (P2-19) — CO-naive vs CO-corrected latency.
//
// Closed-loop benchmarks measure latency = completion − actual_start, where
// actual_start is whenever the harness *got around to* issuing the op. When the
// system stalls, the harness stalls with it and simply issues the next op late —
// so the stall is never charged to any sample. This is "coordinated omission":
// the very requests that should show the worst latency are the ones omitted.
//
// The fix (open-loop load generation): pin an INTENDED start time for every op
// at a fixed rate (--rate ops/sec → intended[i] = t0 + i / rate). Spin until the
// intended time, then process. Report BOTH:
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

#include "OrderBook.h"
#include "BenchLatencyRecorder.h"
#include "RealisticWorkload.h"

#include <cstdio>
#include <cstring>
#include <string>

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
    double ratePerSec = 100000.0; // fixed offered rate (100k/s)

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--orders") == 0 && i + 1 < argc)
            cfg.newOrders = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--warmup") == 0 && i + 1 < argc)
            warmup = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
            cfg.seed = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--rate") == 0 && i + 1 < argc)
            ratePerSec = std::stod(argv[++i]);
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("Usage: CoordinatedOmissionBenchmark [--orders N] [--warmup N] "
                        "[--seed S] [--rate R]\n");
            return 0;
        }
    }

    // The workload's own Poisson gaps are irrelevant here — CO correction needs
    // a FIXED cadence — so we override arrivals with the fixed rate below.
    cfg.lambdaPerSec = ratePerSec;

    std::printf("=======================================================\n");
    std::printf("  Coordinated-Omission Benchmark (P2-19)\n");
    std::printf("=======================================================\n\n");
    std::printf("Configuration:\n");
    std::printf("  Ops:          %zu (New+Cancel realistic mix)\n", cfg.newOrders);
    std::printf("  Warmup:       %zu\n", warmup);
    std::printf("  Seed:         %llu\n", (unsigned long long)cfg.seed);
    std::printf("  Offered rate: %.0f ops/sec (fixed cadence)\n", ratePerSec);

    WorkloadGenerator gen(cfg);
    const auto& ops = gen.ops();
    const double periodNs = 1e9 / ratePerSec;

    std::printf("  Total ops:    %zu, period = %.1f ns/op\n", ops.size(), periodNs);
    std::printf("\nRunning open-loop (spin to intended start time)...\n");

    OrderBook book(0, MatchAlgorithm::PriceTime);

    BenchLatencyRecorder recNaive;      // completion − actual_start
    BenchLatencyRecorder recCorrected;  // completion − intended_start
    uint64_t backlogSamples = 0;        // ops where actual_start ran late

    // Anchor intended-start schedule after warmup so warmup drift is excluded.
    // We still process warmup ops (to fill the book) but do not time them.
    uint64_t scheduleBase = 0;
    size_t measuredIndex = 0;

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

        if (scheduleBase == 0) scheduleBase = bench::nowNs();
        uint64_t intended =
            scheduleBase + static_cast<uint64_t>(periodNs * static_cast<double>(measuredIndex));

        // Open-loop: spin until the intended issue time. If we are already
        // behind (service slower than the offered rate) this does not wait —
        // the lateness is charged to the CO-corrected sample below.
        uint64_t actualStart = bench::nowNs();
        while (actualStart < intended) actualStart = bench::nowNs();
        if (actualStart > intended + static_cast<uint64_t>(periodNs)) ++backlogSamples;

        if (op.kind == WorkloadOp::Kind::New)
            book.addOrder(op.orderId, op.participantId, op.side,
                          op.price, op.qty, OrderType::Limit);
        else
            book.cancelOrder(op.orderId);

        uint64_t completion = bench::nowNs();
        recNaive.recordInterval(actualStart, completion);
        recCorrected.recordInterval(intended, completion);
        ++measuredIndex;
    }

    std::printf("\n── CO-NAIVE (completion − actual_start) ──\n");
    std::printf("  Hides backlog: late issue never charged.\n");
    recNaive.printTable("co-naive");

    std::printf("\n── CO-CORRECTED (completion − intended_start) ──\n");
    std::printf("  Charges backlog: each op owes from when it *should* have started.\n");
    recCorrected.printTable("co-corrected");

    std::printf("\n── Delta (corrected − naive) ──\n");
    auto delta = [](uint64_t c, uint64_t n) {
        return (long long)c - (long long)n;
    };
    std::printf("  Backlogged samples: %llu / %llu (%.1f%%)\n",
                (unsigned long long)backlogSamples, (unsigned long long)measuredIndex,
                measuredIndex ? 100.0 * (double)backlogSamples / (double)measuredIndex : 0.0);
    std::printf("  P50   : %+lld ns\n", delta(recCorrected.getP50(),   recNaive.getP50()));
    std::printf("  P99   : %+lld ns\n", delta(recCorrected.getP99(),   recNaive.getP99()));
    std::printf("  P99.9 : %+lld ns\n", delta(recCorrected.getP999(),  recNaive.getP999()));
    std::printf("  P99.99: %+lld ns\n", delta(recCorrected.getP9999(), recNaive.getP9999()));
    std::printf("\n  If the offered rate is below service capacity the two tables\n");
    std::printf("  nearly coincide (little backlog). Raise --rate past what the box\n");
    std::printf("  can serve and the CO-corrected tail diverges upward: that gap is\n");
    std::printf("  the latency a closed-loop harness would have silently omitted.\n");
    std::printf("  (Apple-Silicon absolute numbers are NOT authoritative — x86 CI is.)\n");
    std::printf("=======================================================\n");
    return 0;
}
