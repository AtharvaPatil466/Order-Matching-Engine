// RealisticFlowBenchmark (P2-18) — latency on a realistic order flow.
//
// The headline PGO number is measured on a seed=42 stream where ~100% of
// submissions are accepted and there is no cancel traffic. Real venues do not
// look like that. This benchmark drives the core matching path with:
//
//   • Poisson arrivals (configurable --lambda, orders/sec)
//   • power-law (Pareto) order sizes
//   • a random-walk mid price
//   • ~15% of submissions marketable (cross the book → fill)
//   • ~65% of resting orders cancelled before they fill
//
// It reports P50/P90/P99/P99.9/P99.99/max on THIS workload (via the vendored
// BenchLatencyRecorder), broken out for New / Cancel / combined, and prints the
// realized fill and cancel rates so the contrast with the 100%-fill PGO number
// is explicit.
//
// The order flow is generated up-front (deterministic for a given seed); only
// the engine operations are timed. Poisson gaps describe the arrival process
// and are reported, but this variant measures service latency (not queueing) —
// coordinated-omission correction is the job of CoordinatedOmissionBenchmark.
//
// Usage:
//   RealisticFlowBenchmark [--orders N] [--warmup N] [--seed S]
//                          [--lambda L] [--marketable F] [--cancel F]

#include "OrderBook.h"
#include "BenchLatencyRecorder.h"
#include "RealisticWorkload.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--orders") == 0 && i + 1 < argc)
            cfg.newOrders = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--warmup") == 0 && i + 1 < argc)
            warmup = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
            cfg.seed = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--lambda") == 0 && i + 1 < argc)
            cfg.lambdaPerSec = std::stod(argv[++i]);
        else if (std::strcmp(argv[i], "--marketable") == 0 && i + 1 < argc)
            cfg.marketableFrac = std::stod(argv[++i]);
        else if (std::strcmp(argv[i], "--cancel") == 0 && i + 1 < argc)
            cfg.restingCancelFrac = std::stod(argv[++i]);
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("Usage: RealisticFlowBenchmark [--orders N] [--warmup N] "
                        "[--seed S] [--lambda L] [--marketable F] [--cancel F]\n");
            return 0;
        }
    }

    std::printf("=======================================================\n");
    std::printf("  Realistic Flow Benchmark (P2-18)\n");
    std::printf("=======================================================\n\n");
    std::printf("Configuration:\n");
    std::printf("  New orders:        %zu\n", cfg.newOrders);
    std::printf("  Warmup:            %zu\n", warmup);
    std::printf("  Seed:              %llu\n", (unsigned long long)cfg.seed);
    std::printf("  Poisson lambda:    %.0f orders/sec\n", cfg.lambdaPerSec);
    std::printf("  Marketable target: %.1f%%\n", 100.0 * cfg.marketableFrac);
    std::printf("  Resting-cancel:    %.1f%%\n", 100.0 * cfg.restingCancelFrac);
    std::printf("  Order sizes:       Pareto(alpha=%.2f), clamp %llu\n",
                cfg.paretoAlpha, (unsigned long long)cfg.maxQty);

    std::printf("\nGenerating realistic order flow...\n");
    WorkloadGenerator gen(cfg);
    const auto& ops = gen.ops();
    std::printf("  Total ops:         %zu (New=%zu, Cancel=%zu)\n",
                ops.size(),
                gen.marketableCount() + gen.passiveCount(),
                ops.size() - gen.marketableCount() - gen.passiveCount());
    std::printf("  Marketable (design): %zu  Scheduled cancels: %zu\n",
                gen.marketableCount(), gen.scheduledCancels());

    OrderBook book(0, MatchAlgorithm::PriceTime);

    BenchLatencyRecorder recNew, recCancel, recAll;
    uint64_t submissions = 0, filledSubmissions = 0, cancelsIssued = 0;
    uint64_t poissonSpanNs = 0;

    auto wallStart = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < ops.size(); ++i) {
        const WorkloadOp& op = ops[i];
        const bool warm = i >= warmup;
        poissonSpanNs += op.gapNs;

        if (op.kind == WorkloadOp::Kind::New) {
            uint64_t tradesBefore = book.getTradeCount();
            uint64_t t0 = bench::nowNs();
            book.addOrder(op.orderId, op.participantId, op.side,
                          op.price, op.qty, OrderType::Limit);
            uint64_t t1 = bench::nowNs();
            if (warm) {
                recNew.recordInterval(t0, t1);
                recAll.recordInterval(t0, t1);
                ++submissions;
                if (book.getTradeCount() > tradesBefore) ++filledSubmissions;
            }
        } else {
            uint64_t t0 = bench::nowNs();
            book.cancelOrder(op.orderId);
            uint64_t t1 = bench::nowNs();
            if (warm) {
                recCancel.recordInterval(t0, t1);
                recAll.recordInterval(t0, t1);
                ++cancelsIssued;
            }
        }
    }

    auto wallEnd = std::chrono::high_resolution_clock::now();
    double wallSec = std::chrono::duration<double>(wallEnd - wallStart).count();

    std::printf("\n── Realized workload characteristics ──\n");
    std::printf("  Submissions (timed):  %llu\n", (unsigned long long)submissions);
    std::printf("  Filled submissions:   %llu (%.1f%% fill rate)\n",
                (unsigned long long)filledSubmissions,
                submissions ? 100.0 * (double)filledSubmissions / (double)submissions : 0.0);
    std::printf("  Cancels issued:       %llu\n", (unsigned long long)cancelsIssued);
    std::printf("  Modelled arrival span: %.3f s @ lambda=%.0f/s\n",
                (double)poissonSpanNs / 1e9, cfg.lambdaPerSec);
    std::printf("  Actual processing:     %.3f s (%.0f ops/sec)\n",
                wallSec, wallSec > 0 ? (double)ops.size() / wallSec : 0.0);

    std::printf("\n── Latency: NEW orders ──\n");
    recNew.printTable("new");
    std::printf("\n── Latency: CANCEL orders ──\n");
    recCancel.printTable("cancel");
    std::printf("\n── Latency: COMBINED flow ──\n");
    recAll.printTable("combined");

    std::printf("\n── Contrast with the seed=42 / 100%%-fill PGO number ──\n");
    std::printf("  The PGO headline uses a 100%%-accepted, cancel-free stream, so its\n");
    std::printf("  tail reflects a best case. This workload mixes ~%.0f%% fills with a\n",
                100.0 * cfg.marketableFrac);
    std::printf("  heavy cancel load and power-law sizes; the P99.9/P99.99 above are\n");
    std::printf("  the honest tail for realistic flow. (Apple-Silicon absolute numbers\n");
    std::printf("  are NOT authoritative — trust the x86 CI baseline for magnitudes.)\n");
    std::printf("=======================================================\n");
    return 0;
}
