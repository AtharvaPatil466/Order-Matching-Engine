// ColdCacheBenchmark (P2-20) — warm-cache vs cold-cache latency.
//
// Steady-state benchmarks keep the order book's hot structures resident in L1/L2,
// so they report the *warm* path. But the first order after an idle gap, a cache
// eviction, or a context switch pays to pull those lines back from LLC/DRAM. This
// benchmark measures that cold path explicitly.
//
// Before each timed op it evicts the working set by streaming through a dummy
// buffer larger than any last-level cache (portable — no CLFLUSH, so it also
// runs on Apple Silicon). On x86 one could instead _mm_clflush the specific
// lines; the bulk-thrash approach is architecture-neutral and evicts the whole
// book. The eviction walk is NOT timed — only the op is.
//
// Reports warm and cold P50/P90/P99/P99.9/P99.99/max side by side via the
// vendored BenchLatencyRecorder.
//
// Usage:
//   ColdCacheBenchmark [--orders N] [--seed S] [--evict-mb M]

#include "OrderBook.h"
#include "BenchLatencyRecorder.h"
#include "RealisticWorkload.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace OrderMatcher;
using bench::BenchLatencyRecorder;
using bench::WorkloadConfig;
using bench::WorkloadGenerator;
using bench::WorkloadOp;

// A cache thrasher sized above the LLC. Streaming through it with a dependent
// read-modify-write evicts the book's working set and defeats prefetch/DCE.
class CacheThrasher {
public:
    explicit CacheThrasher(size_t bytes) : buf_(bytes / sizeof(uint64_t), 1) {}

    // Touch every line; carry a data dependency so the compiler cannot elide it.
    void evict() {
        uint64_t acc = sink_;
        // 8 bytes/elem, 64-byte lines → step 8 elements to hit each line once.
        for (size_t i = 0; i < buf_.size(); i += 8) {
            buf_[i] += acc;
            acc ^= buf_[i];
        }
        sink_ = acc;
    }

    uint64_t sink() const { return sink_; }

private:
    std::vector<uint64_t> buf_;
    volatile uint64_t sink_{0};
};

// Run the op stream once, timing each op. If `thrasher` is non-null, evict
// before every op (cold path); the eviction is outside the timed region.
static void runPass(OrderBook& book, const std::vector<WorkloadOp>& ops,
                    size_t warmup, BenchLatencyRecorder& rec,
                    CacheThrasher* thrasher) {
    for (size_t i = 0; i < ops.size(); ++i) {
        const WorkloadOp& op = ops[i];
        const bool measure = i >= warmup;

        if (thrasher) thrasher->evict();  // NOT timed

        uint64_t t0 = bench::nowNs();
        if (op.kind == WorkloadOp::Kind::New)
            book.addOrder(op.orderId, op.participantId, op.side,
                          op.price, op.qty, OrderType::Limit);
        else
            book.cancelOrder(op.orderId);
        uint64_t t1 = bench::nowNs();

        if (measure) rec.recordInterval(t0, t1);
    }
}

int main(int argc, char* argv[]) {
    WorkloadConfig cfg;
    cfg.newOrders = 20000;
    cfg.seed = 7;
    size_t warmup = 2000;
    size_t evictMb = 64; // > any current LLC

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--orders") == 0 && i + 1 < argc)
            cfg.newOrders = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
            cfg.seed = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--evict-mb") == 0 && i + 1 < argc)
            evictMb = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--warmup") == 0 && i + 1 < argc)
            warmup = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("Usage: ColdCacheBenchmark [--orders N] [--seed S] "
                        "[--evict-mb M] [--warmup N]\n");
            return 0;
        }
    }

    std::printf("=======================================================\n");
    std::printf("  Cold-Cache Benchmark (P2-20)\n");
    std::printf("=======================================================\n\n");
    std::printf("Configuration:\n");
    std::printf("  Ops:        %zu (realistic New+Cancel mix)\n", cfg.newOrders);
    std::printf("  Warmup:     %zu\n", warmup);
    std::printf("  Seed:       %llu\n", (unsigned long long)cfg.seed);
    std::printf("  Evict buf:  %zu MB (portable bulk thrash, no CLFLUSH)\n", evictMb);

    WorkloadGenerator gen(cfg);
    const auto& ops = gen.ops();
    std::printf("  Total ops:  %zu\n", ops.size());

    CacheThrasher thrasher(evictMb * 1024 * 1024);

    // WARM pass — structures stay resident between ops.
    std::printf("\nRunning WARM pass...\n");
    OrderBook warmBook(0, MatchAlgorithm::PriceTime);
    BenchLatencyRecorder recWarm;
    runPass(warmBook, ops, warmup, recWarm, nullptr);

    // COLD pass — evict the working set before each op.
    std::printf("Running COLD pass (evict before each op)...\n");
    OrderBook coldBook(0, MatchAlgorithm::PriceTime);
    BenchLatencyRecorder recCold;
    runPass(coldBook, ops, warmup, recCold, &thrasher);

    std::printf("\n── WARM cache ──\n");
    recWarm.printTable("warm");
    std::printf("\n── COLD cache (working set evicted before each op) ──\n");
    recCold.printTable("cold");

    std::printf("\n── Cold penalty (cold − warm) ──\n");
    auto d = [](uint64_t c, uint64_t w) { return (long long)c - (long long)w; };
    std::printf("  P50   : %+lld ns\n", d(recCold.getP50(),   recWarm.getP50()));
    std::printf("  P90   : %+lld ns\n", d(recCold.getP90(),   recWarm.getP90()));
    std::printf("  P99   : %+lld ns\n", d(recCold.getP99(),   recWarm.getP99()));
    std::printf("  P99.9 : %+lld ns\n", d(recCold.getP999(),  recWarm.getP999()));
    std::printf("\n  Cold P50 is the honest first-touch latency after an idle gap or\n");
    std::printf("  context switch; warm P50 is the steady-state hot path. Production\n");
    std::printf("  tail SLAs must budget for the cold number, not just the warm one.\n");
    std::printf("  (Apple-Silicon absolute numbers are NOT authoritative — x86 CI is.)\n");
    // Keep the thrasher result observable so the eviction is not optimized away.
    std::printf("  (thrash sink=%llu)\n", (unsigned long long)thrasher.sink());
    std::printf("=======================================================\n");
    return 0;
}
