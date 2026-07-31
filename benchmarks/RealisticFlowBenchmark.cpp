// RealisticFlowBenchmark (P2-18) — latency on a realistic, cancel-heavy order flow.
//
// The PGO headline is a seed=42 stream where ~100% of submissions fill and there
// is no cancel traffic. Real venues are cancel-dominated churn. This benchmark
// drives the core matching path with the venue-shaped event mix:
//
//   • 44% CANCEL   — cancel a randomly selected resting order (no-op if the book
//                    pool is empty)
//   • 46% NEW      — passive limit that rests, priced away from the mid so it is
//                    never immediately aggressive
//   •  8% IOC      — aggressive immediate-or-cancel that crosses and may sweep
//                    multiple levels; unfilled remainder is cancelled (does not rest)
//   •  2% MODIFY   — cancel a resting order + resubmit at a new price
//
// Order sizes are power-law (Pareto): most small, occasional large. The mid price
// is a bounded random walk of +/- 5 ticks per event. Orders are spread across a
// handful of participants so IOCs cross OTHERS' liquidity (a single participant
// would self-trade and STP-cancel instead of filling).
//
// Timing uses the SAME infrastructure as HonestBenchmark: bench::nowNs() around
// each engine call and bench::BenchLatencyRecorder for the percentile tables
// (P50/P90/P99/P99.9/P99.99/max). Only the engine operation is timed — RNG draws
// and resting-pool bookkeeping happen outside the timed region.
//
// The mix is net-BUILDING (cancel < new) by construction. An earlier revision
// used the venue tape's 65/25 directly; because you cannot cancel more orders
// than you create, the book drained to empty within a few hundred events and
// the run measured an empty book — mean depth 0.6, ~2 of 3 cancels no-op'ing,
// realized mix collapsed to 42.6/42.6. Latencies gathered in that regime say
// nothing about a venue: an empty book has no price levels to miss on and no
// resting orders to look up. Cancel is now held just below new so depth builds
// and the price map is actually exercised. Realized mix, no-op counts, IOC fill
// rate and mean depth are all reported — if depth ever collapses toward 0 the
// numbers below are meaningless and the mix is the first thing to check.
//
// instructions/order: NOT measurable on macOS/Apple Silicon (no perf, no HW
// counters). Run the printed perf command on the x86 CI box for that metric.
//
// Usage:
//   RealisticFlowBenchmark [--events N] [--orders N] [--warmup N] [--seed S]

#include "OrderBook.h"
#include "BenchLatencyRecorder.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace OrderMatcher;
using bench::BenchLatencyRecorder;

namespace {

// Event mix (cumulative thresholds on a U[0,1) draw): 44% / 46% / 8% / 2%.
//
// A real venue's tape is ~65% cancel / 25% new, but that ratio cannot be
// replayed directly: you cannot cancel more orders than you create. At 65/25
// the resting pool drained within a few hundred events and never refilled —
// ~2 of every 3 cancels no-op'd against an empty book, the realized mix
// collapsed to 42.6/42.6, and mean depth sat at 0.6 orders. The run measured
// an empty book wearing venue-shaped labels.
//
// So cancel is held just below new. IOC fills also retire resting orders
// (~1 per IOC at these Pareto sizes); the balance is closed by cancels that
// land on ids an IOC already consumed, which pop the tracking pool without
// retiring a live order. Net: the book fills and keeps growing across the run.
// Do NOT "restore" 65/25 — it re-empties the book. Check the reported mean
// resting depth after any change here.
constexpr double kCancelCum = 0.44;
constexpr double kNewCum    = 0.90;  // 0.44..0.90 -> 46% new
constexpr double kIocCum    = 0.98;  // 0.90..0.98 -> 8% IOC; remainder -> 2% modify

constexpr Price     kStartMid    = 100000;  // mid in ticks
constexpr int       kWalkTicks   = 5;       // +/- ticks of random walk per event
constexpr Price     kMidFloor    = 1000;    // keep the mid (and prices) positive
constexpr Quantity  kMaxQty      = 10000;   // power-law tail clamp
constexpr double    kParetoAlpha = 1.5;     // size exponent (most small, some large)
constexpr uint64_t  kParticipants = 8;      // so IOCs cross others, not self-trade

}  // namespace

int main(int argc, char* argv[]) {
    size_t   events = 500000;   // total events
    uint64_t seed   = 42;
    size_t   warmup = 5000;

    for (int i = 1; i < argc; ++i) {
        if ((std::strcmp(argv[i], "--events") == 0 ||
             std::strcmp(argv[i], "--orders") == 0) && i + 1 < argc)
            events = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--warmup") == 0 && i + 1 < argc)
            warmup = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
            seed = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("Usage: RealisticFlowBenchmark [--events N] [--warmup N] [--seed S]\n");
            return 0;
        }
    }
    if (warmup >= events) warmup = events / 10;

    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    std::uniform_int_distribution<int>     walk(-kWalkTicks, kWalkTicks);
    std::uniform_int_distribution<Price>   passiveOff(1, 20);   // rest away from mid
    std::uniform_int_distribution<Price>   iocAggr(20, 60);     // cross deep enough to sweep
    std::uniform_int_distribution<uint64_t> pidPick(1, kParticipants);

    // Pareto(alpha) size: q = floor((1-U)^(-1/alpha)), clamped to [1, kMaxQty].
    auto drawQty = [&]() -> Quantity {
        double v = std::pow(1.0 - u01(rng), -1.0 / kParetoAlpha);
        if (v < 1.0) v = 1.0;
        Quantity q = static_cast<Quantity>(v);
        return q > kMaxQty ? kMaxQty : q;
    };

    OrderBook book(0, MatchAlgorithm::PriceTime);
    book.setTradingState(TradingState::Continuous);

    // Approximate resting-order pool. IOC fills consume resting orders without us
    // knowing which, so entries can go stale; cancelOrder() on a stale id is a
    // safe no-op, and we drop it from the pool regardless. Superset of the true
    // resting set — never causes a wrong cancel, only occasional no-op cancels.
    std::vector<OrderId> resting;
    resting.reserve(events / 4);

    BenchLatencyRecorder recCancel, recNew, recIoc, recModify, recAll;
    uint64_t nCancel = 0, nNew = 0, nIoc = 0, nModify = 0;
    uint64_t noopCancel = 0, noopModify = 0, iocFilled = 0;
    uint64_t depthSum = 0, poolSum = 0, depthSamples = 0;

    OrderId  nextId = 1;
    Price    mid = kStartMid;

    for (size_t i = 0; i < events; ++i) {
        const bool warm = i >= warmup;
        mid += walk(rng);
        if (mid < kMidFloor) mid = kMidFloor;
        // True live depth is the pool the book actually holds. `resting` is only
        // our tracking superset — it never shrinks when an IOC consumes an order,
        // so reporting its size would overstate depth. Both are printed below.
        if (warm) { depthSum += book.poolInUse(); poolSum += resting.size(); ++depthSamples; }

        const double r = u01(rng);

        if (r < kCancelCum) {
            // ── 65% CANCEL a randomly selected resting order ──────────────────
            if (resting.empty()) { if (warm) ++noopCancel; continue; }
            const size_t idx = static_cast<size_t>(rng() % resting.size());
            const OrderId id = resting[idx];
            resting[idx] = resting.back();
            resting.pop_back();

            const uint64_t t0 = bench::nowNs();
            book.cancelOrder(id);
            const uint64_t t1 = bench::nowNs();
            if (warm) { recCancel.recordInterval(t0, t1); recAll.recordInterval(t0, t1); ++nCancel; }

        } else if (r < kNewCum) {
            // ── 25% NEW passive limit that rests (never aggressive) ───────────
            const Side side = (u01(rng) < 0.5) ? Side::Buy : Side::Sell;
            const Price off = passiveOff(rng);
            Price px = (side == Side::Buy) ? mid - off : mid + off;
            if (px < 1) px = 1;
            const OrderId id = nextId++;
            const Quantity q = drawQty();
            const ParticipantId pid = pidPick(rng);

            const uint64_t t0 = bench::nowNs();
            book.addOrder(id, pid, side, px, q, OrderType::Limit);
            const uint64_t t1 = bench::nowNs();
            resting.push_back(id);
            if (warm) { recNew.recordInterval(t0, t1); recAll.recordInterval(t0, t1); ++nNew; }

        } else if (r < kIocCum) {
            // ── 8% aggressive IOC that may sweep multiple levels ──────────────
            const Side side = (u01(rng) < 0.5) ? Side::Buy : Side::Sell;
            const Price aggr = iocAggr(rng);
            Price px = (side == Side::Buy) ? mid + aggr : mid - aggr;
            if (px < 1) px = 1;
            const OrderId id = nextId++;
            const Quantity q = drawQty();
            const ParticipantId pid = pidPick(rng);
            const uint64_t tradesBefore = book.getTradeCount();

            const uint64_t t0 = bench::nowNs();
            book.addOrder(id, pid, side, px, q, OrderType::IOC);
            const uint64_t t1 = bench::nowNs();
            // IOC never rests — nothing added to the pool.
            if (warm) {
                recIoc.recordInterval(t0, t1);
                recAll.recordInterval(t0, t1);
                ++nIoc;
                if (book.getTradeCount() > tradesBefore) ++iocFilled;
            }

        } else {
            // ── 2% MODIFY = cancel a resting order + resubmit at a new price ──
            if (resting.empty()) { if (warm) ++noopModify; continue; }
            const size_t idx = static_cast<size_t>(rng() % resting.size());
            const OrderId oldId = resting[idx];
            const Side side = (u01(rng) < 0.5) ? Side::Buy : Side::Sell;
            const Price off = passiveOff(rng);
            Price px = (side == Side::Buy) ? mid - off : mid + off;
            if (px < 1) px = 1;
            const OrderId newId = nextId++;
            const Quantity q = drawQty();
            const ParticipantId pid = pidPick(rng);

            const uint64_t t0 = bench::nowNs();
            book.cancelOrder(oldId);
            book.addOrder(newId, pid, side, px, q, OrderType::Limit);
            const uint64_t t1 = bench::nowNs();
            resting[idx] = newId;
            if (warm) { recModify.recordInterval(t0, t1); recAll.recordInterval(t0, t1); ++nModify; }
        }
    }

    const uint64_t timed = nCancel + nNew + nIoc + nModify;
    auto pct = [&](uint64_t n) { return timed ? 100.0 * (double)n / (double)timed : 0.0; };

    std::printf("=======================================================\n");
    std::printf("  Realistic Flow Benchmark (P2-18)\n");
    std::printf("=======================================================\n\n");
    std::printf("Configuration:\n");
    std::printf("  Events:            %zu (warmup %zu, seed %llu)\n",
                events, warmup, (unsigned long long)seed);
    std::printf("  Target mix:        44%% cancel / 46%% new / 8%% IOC / 2%% modify\n");
    std::printf("                     (cancel held below new so the book sustains;\n");
    std::printf("                      a venue tape reads ~65/25 but cannot be replayed)\n");
    std::printf("  Order sizes:       Pareto(alpha=%.2f), clamp %llu\n",
                kParetoAlpha, (unsigned long long)kMaxQty);
    std::printf("  Mid random walk:   +/-%d ticks/event from %lld\n",
                kWalkTicks, (long long)kStartMid);
    std::printf("  Participants:      %llu\n", (unsigned long long)kParticipants);

    std::printf("\n── Realized workload (warm ops only) ──\n");
    std::printf("  Timed ops:         %llu\n", (unsigned long long)timed);
    std::printf("  Cancel:  %8llu (%.1f%%)   [no-op empty-book: %llu]\n",
                (unsigned long long)nCancel, pct(nCancel), (unsigned long long)noopCancel);
    std::printf("  New:     %8llu (%.1f%%)\n", (unsigned long long)nNew, pct(nNew));
    std::printf("  IOC:     %8llu (%.1f%%)   [filled: %llu (%.1f%%)]\n",
                (unsigned long long)nIoc, pct(nIoc), (unsigned long long)iocFilled,
                nIoc ? 100.0 * (double)iocFilled / (double)nIoc : 0.0);
    std::printf("  Modify:  %8llu (%.1f%%)   [no-op empty-book: %llu]\n",
                (unsigned long long)nModify, pct(nModify), (unsigned long long)noopModify);
    std::printf("  Mean resting depth: %.1f orders (tracked-id pool: %.1f)\n",
                depthSamples ? (double)depthSum / (double)depthSamples : 0.0,
                depthSamples ? (double)poolSum  / (double)depthSamples : 0.0);

    std::printf("\n── Latency: CANCEL ──\n");   recCancel.printTable("cancel");
    std::printf("\n── Latency: NEW ──\n");      recNew.printTable("new");
    std::printf("\n── Latency: IOC ──\n");      recIoc.printTable("ioc");
    std::printf("\n── Latency: MODIFY (cancel+resubmit) ──\n"); recModify.printTable("modify");
    std::printf("\n── Latency: COMBINED flow ──\n"); recAll.printTable("combined");

    std::printf("\n── instructions/order ──\n");
    std::printf("  NOT measured here: this is Apple Silicon (no perf / HW counters).\n");
    std::printf("  Run on the x86 CI box:\n");
    std::printf("    numactl --cpunodebind=0 --membind=0 \\\n");
    std::printf("      perf stat -e instructions,cycles,branch-misses,L1-dcache-load-misses \\\n");
    std::printf("      -r 5 ./build/benchmarks/RealisticFlowBenchmark --events 500000 --seed 42\n");
    std::printf("  instructions/order = <instructions> / %llu (timed ops).\n",
                (unsigned long long)timed);
    std::printf("  (Apple-Silicon absolute latencies above are NOT authoritative —\n");
    std::printf("   trust the x86 CI baseline for magnitudes.)\n");
    std::printf("=======================================================\n");
    return 0;
}
