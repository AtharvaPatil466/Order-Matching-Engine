// SnapshotContentionBenchmark — can an admin snapshot stall matching?
//
// WHY THIS EXISTS
//
// OrderBook::getSnapshot takes bookLock_ — the SAME std::mutex the whole
// matching path holds (addOrder / cancelOrder / modifyOrder / cancelReplace).
// The admin server's /book and /audit reach it through
// MatchingEngine::getSnapshot, so a dashboard polling those serialises against
// order entry. The comment on getSnapshot in src/OrderBook.cpp concluded "not
// worth a seqlock" from a measurement of P50 291 ns / max 7.25 us taken on a
// 20,000-order book.
//
// That measurement was taken on a book shape where the problem CANNOT APPEAR,
// and this file exists because that objection is correct.
//
// WHAT THE COST ACTUALLY DEPENDS ON
//
// getSnapshot stops after `maxDepth` LEVELS — forEachLevelWhile returns false
// once bidCount/askCount hits the cap (default depth 10, clamped to
// MarketDataSnapshot::MAX_DEPTH = 20). But INSIDE each level it walks EVERY
// ORDER:
//
//     for (Order* o = level.front(); o; o = o->next) { ... }
//
// So the lock hold time is O(depth x orders-per-level). It is NOT O(depth) and
// it is NOT O(book size). The obvious sweep — 10 / 100 / 1,000 / 10,000 PRICE
// LEVELS — is therefore the WRONG AXIS: levels past maxDepth are never walked,
// so that sweep is flat by construction. The axis that matters is QUEUE DEPTH
// AT THE TOUCH (orders resting at one price). The earlier probe had ~10
// orders/level, which is exactly why it came back cheap.
//
// Both axes are swept anyway, so the wrong-axis result is on the record rather
// than being asserted.
//
// THE NUMBER THAT DECIDES IT IS NOT THE SNAPSHOT'S OWN LATENCY.
//
// The question is what happens to an ORDER THAT ARRIVES WHILE A SNAPSHOT HOLDS
// THE LOCK. So the shape is: one producer thread submitting orders and
// recording per-order latency through bench::BenchLatencyRecorder, one scraper
// thread calling getSnapshot in a loop, and three arms per sweep point:
//
//   CONTROL   scraper thread exists and spins, but never takes the lock.
//             Same thread count, same burnt core — the ONLY difference from
//             the other arms is the lock acquisition.
//   SCRAPE@R  scraper paced at R requests/sec (default 1,000 — a dashboard).
//   FLAT-OUT  scraper loops with no pacing. The upper bound: what a runaway
//             poller, a retry storm or several dashboards can do.
//
// The reported result is the DELTA in the matching path's tail between CONTROL
// and the scrape arms.
//
// READING THE PERCENTILES HONESTLY (this matters more than it looks)
//
// A scrape at rate R blocks roughly (R x holdNs / 1e9) of the producer's
// orders. At 1,000 req/s against a 300 ns snapshot that is 0.03% of orders —
// which lands at P99.97, so P99 CANNOT MOVE no matter how bad the stall is.
// The same 1,000 req/s against a 1 ms snapshot blocks ~100% of them. A P99
// delta of zero is therefore only meaningful together with the duty cycle, so
// every point also carries a QUIESCED hold measurement (getSnapshot timed with
// nothing else running, which is the only way to see hold time rather than
// wait+hold) and the duty cycle it implies at the configured poll rate. The
// tail is reported out to P99.99 and max rather than stopping at P99.
//
// PRODUCER OP IS CONSTANT BY CONSTRUCTION
//
// The producer adds a non-marketable buy limit 1,000 ticks below every resting
// bid and immediately cancels it. That is an O(1) insert + O(1) remove into a
// level nothing else touches, identical work at every point of the sweep. So a
// change in producer latency ACROSS ARMS is lock contention and nothing else.
// (Across sweep POINTS it also picks up cache pressure from a bigger book,
// which is why every point carries its own control arm.)
//
// THERMAL DRIFT
//
// Arms are INTERLEAVED, not run sequentially: every round runs control, then
// paced, then flat-out, and the rounds are merged. This box drifts thermally
// over a multi-minute session and a sequential layout would charge that drift
// to whichever arm ran last.
//
// CLOCK GRANULARITY
//
// Apple Silicon's steady_clock ticks at ~41 ns. The tick is measured at startup
// and printed; any per-op figure within kQuantizationTicks of it is
// quantization, not signal, and is flagged '~'. BenchLatencyRecorder COUNTS
// sub-tick samples rather than dropping them and reports the sub-tick fraction,
// and printTable prints "<tick (below clock resolution)" for any percentile
// that falls inside the unresolvable band instead of quoting one tick as a
// measurement.
//
// THE CAPACITY QUESTION
//
// Also answered here because nothing had measured it: what a FlatPriceMap costs
// in bytes, empty and at 10 / 100 / 1,000 / 10,000 occupied levels. Measured
// with mach_task_basic_info (macOS) / /proc/self/statm (Linux) over a batch of
// maps built IN ISOLATION — not by subtracting terms from a whole OrderBook —
// so the number is the map and nothing else. RSS is page-quantized (16 KiB on
// Apple Silicon), which cannot resolve a single small map, hence the batch; the
// exact structural byte accounting is printed beside it as a cross-check.
//
// Usage:
//   SnapshotContentionBenchmark [--ops N] [--rounds N] [--scrape-rate HZ]
//                               [--budget-ms MS] [--only q|l|m]
//     --only q   queue-depth (orders-per-level) sweep only
//     --only l   price-level sweep only
//     --only m   memory sweep only

#include "BenchLatencyRecorder.h"
#include "FlatPriceMap.h"
#include "OrderBook.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

using namespace OrderMatcher;
using bench::BenchLatencyRecorder;

namespace {

// ─── Constants ───────────────────────────────────────────────────────────────

// A latency within this many clock ticks of the tick itself is quantization.
constexpr uint64_t kQuantizationTicks = 3;

// Book is centred here. FlatPriceMap anchors its band on the first price seen
// (OrderBook::ensurePriceRange centres a 200,001-tick window), so every price
// used below stays comfortably in range.
constexpr Price kMidPrice = 100000;

// The producer parks its order this far below the deepest resting bid, so its
// insert never collides with a level the snapshot walks and never crosses.
constexpr Price kProducerOffset = 1000;

constexpr ParticipantId kBookPid = 1;
constexpr ParticipantId kProducerPid = 2;
constexpr Quantity kOrderQty = 100;

// Pool must hold the deepest book this file builds (20 levels x 10,000
// orders x 2 sides = 400,000) with headroom below the 95% shed threshold.
constexpr const char* kPoolCapacity = "600000";

// Maps built per memory-sweep point. RSS is page-quantized, so one map is below
// the noise floor; a batch amortizes the quantization away.
constexpr size_t kMemMapBatch = 32;

// ─── Platform probes ─────────────────────────────────────────────────────────

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

// Resident set size. macOS uses mach_task_basic_info (resident_size), which is
// what the per-book footprint figures in OrderBook.h were taken with.
uint64_t rssBytes() {
#if defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
        return 0;
    }
    return static_cast<uint64_t>(info.resident_size);
#elif defined(__linux__)
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (f == nullptr) return 0;
    unsigned long totalPages = 0, residentPages = 0;
    const int n = std::fscanf(f, "%lu %lu", &totalPages, &residentPages);
    std::fclose(f);
    if (n != 2) return 0;
    return static_cast<uint64_t>(residentPages) *
           static_cast<uint64_t>(::getpagesize());
#else
    return 0;
#endif
}

double toMiB(uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

// ─── Book construction ───────────────────────────────────────────────────────

// `levels` price levels per side, `ordersPerLevel` resting orders on each.
// Bids descend from kMidPrice-1, asks ascend from kMidPrice+1 — nothing ever
// crosses, so the book shape this builds is exactly the shape it keeps.
OrderId buildBook(OrderBook& book, size_t levels, size_t ordersPerLevel, OrderId firstId) {
    OrderId id = firstId;
    for (size_t i = 0; i < levels; ++i) {
        const Price bid = kMidPrice - static_cast<Price>(i) - 1;
        const Price ask = kMidPrice + static_cast<Price>(i) + 1;
        for (size_t k = 0; k < ordersPerLevel; ++k) {
            book.addOrder(id++, kBookPid, Side::Buy, bid, kOrderQty, OrderType::Limit);
            book.addOrder(id++, kBookPid, Side::Sell, ask, kOrderQty, OrderType::Limit);
        }
    }
    return id;
}

// ─── Arms ────────────────────────────────────────────────────────────────────

enum class Arm { Control = 0, Paced = 1, FlatOut = 2 };
constexpr int kNumArms = 3;

const char* armName(Arm a) {
    switch (a) {
        case Arm::Control: return "control";
        case Arm::Paced:   return "paced";
        case Arm::FlatOut: return "flat-out";
    }
    return "?";
}

struct ArmStats {
    BenchLatencyRecorder match;  // producer-side addOrder latency
    BenchLatencyRecorder snap;   // scraper-side getSnapshot latency
    uint64_t ops{0};
    uint64_t scrapes{0};
    uint64_t behind{0};     // paced arm only: deadlines missed by a full period
    long double wallNs{0};  // total producer wall time
};

struct RoundConfig {
    size_t   depth{10};
    uint64_t rateHz{1000};
    uint64_t opsTarget{150000};
    uint64_t budgetMs{1000};
    Price    producerPrice{0};
};

// One interleave slot: run `arm` once against `book` and fold the result into
// `out`. Returns when the producer has done opsTarget ops OR spent budgetMs —
// the budget matters, because flat-out scraping of a 10,000-deep level starves
// the producer badly enough that a fixed op count would run for minutes.
void runRound(OrderBook& book, Arm arm, const RoundConfig& cfg,
              OrderId& nextId, ArmStats& out) {
    const bool doScrape = (arm != Arm::Control);
    const uint64_t periodNs =
        (arm == Arm::Paced && cfg.rateHz > 0) ? (1000000000ULL / cfg.rateHz) : 0;

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> sink{0};

    // Scraper-thread locals; read only after join().
    BenchLatencyRecorder snap;
    uint64_t scrapes = 0;
    uint64_t behind = 0;

    std::thread scraper([&] {
        uint64_t nextDeadline = bench::nowNs() + periodNs;
        while (!stop.load(std::memory_order_relaxed)) {
            if (periodNs > 0) {
                const uint64_t now = bench::nowNs();
                if (now < nextDeadline) continue;  // spin to the deadline
                if (now > nextDeadline + periodNs) {
                    // A whole period late: the snapshot itself is slower than
                    // the requested interval. Do not let the schedule collapse
                    // into a free-run — count it and re-anchor.
                    ++behind;
                    nextDeadline = now + periodNs;
                } else {
                    nextDeadline += periodNs;
                }
            }
            if (!doScrape) continue;  // control: identical loop, no lock taken

            const uint64_t t0 = bench::nowNs();
            const MarketDataSnapshot s = book.getSnapshot(cfg.depth);
            const uint64_t t1 = bench::nowNs();
            snap.recordInterval(t0, t1);
            ++scrapes;
            // Keep the snapshot observable so the call cannot be elided.
            sink.fetch_add(s.bidCount + s.askCount, std::memory_order_relaxed);
        }
    });

    const uint64_t startNs = bench::nowNs();
    const uint64_t deadlineNs = startNs + cfg.budgetMs * 1000000ULL;
    uint64_t done = 0;
    uint64_t lastNs = startNs;
    while (done < cfg.opsTarget) {
        const OrderId id = nextId++;
        const uint64_t a = bench::nowNs();
        book.addOrder(id, kProducerPid, Side::Buy, cfg.producerPrice,
                      kOrderQty, OrderType::Limit);
        const uint64_t b = bench::nowNs();
        out.match.recordInterval(a, b);
        book.cancelOrder(id);
        ++done;
        lastNs = b;
        if (b > deadlineNs) break;  // free check: b is already read
    }

    stop.store(true, std::memory_order_relaxed);
    scraper.join();

    out.ops += done;
    out.wallNs += static_cast<long double>(lastNs - startNs);
    out.snap.mergeFrom(snap);
    out.scrapes += scrapes;
    out.behind += behind;
}

struct PointResult {
    size_t levels{0};
    size_t ordersPerLevel{0};
    size_t depth{0};
    ArmStats arms[kNumArms];
    BenchLatencyRecorder solo;  // quiesced getSnapshot — true lock HOLD time
    uint64_t soloCount{0};
};

// THE SCRAPE ARMS CANNOT MEASURE LOCK HOLD TIME, so this does.
//
// In the loaded arms the producer runs flat out and owns bookLock_ almost
// continuously, so the interval the scraper times around getSnapshot is
// WAIT + HOLD, dominated by waiting for the producer. Reported as a hold time
// it would be wildly overstated — the first run of this produced a "lock duty"
// of 102.67%, which is how the error announced itself.
//
// Here nothing else is running, so the interval IS the hold time. That is the
// number the duty cycle needs, and the one that answers "how long does one
// admin read own the book".
void measureSoloSnapshot(OrderBook& book, size_t depth, uint64_t maxIters,
                         uint64_t budgetMs, PointResult& out) {
    std::atomic<uint64_t> sink{0};
    const uint64_t deadline = bench::nowNs() + budgetMs * 1000000ULL;
    for (uint64_t i = 0; i < maxIters; ++i) {
        const uint64_t t0 = bench::nowNs();
        const MarketDataSnapshot s = book.getSnapshot(depth);
        const uint64_t t1 = bench::nowNs();
        out.solo.recordInterval(t0, t1);
        ++out.soloCount;
        sink.fetch_add(s.bidCount + s.askCount, std::memory_order_relaxed);
        // Always take a minimum sample count, even when a single snapshot
        // already costs more than the whole budget.
        if (i >= 32 && t1 > deadline) break;
    }
}

// One point of a sweep: build the book once, then interleave the arms over it.
// The book is NOT rebuilt between arms — the producer's add/cancel pair leaves
// it exactly as it found it, so all three arms see identical state.
PointResult runPoint(size_t levels, size_t ordersPerLevel, size_t depth,
                     const RoundConfig& base, size_t rounds) {
    PointResult r;
    r.levels = levels;
    r.ordersPerLevel = ordersPerLevel;
    r.depth = depth;

    OrderBook book(0, MatchAlgorithm::PriceTime);
    OrderId nextId = 1;
    nextId = buildBook(book, levels, ordersPerLevel, nextId);

    RoundConfig cfg = base;
    cfg.depth = depth;
    cfg.producerPrice = kMidPrice - static_cast<Price>(levels) - kProducerOffset;

    // Warm the producer's own level and the snapshot path before timing.
    {
        ArmStats warm;
        RoundConfig w = cfg;
        w.opsTarget = 2000;
        w.budgetMs = 200;
        runRound(book, Arm::FlatOut, w, nextId, warm);
    }

    measureSoloSnapshot(book, depth, 20000, 250, r);

    for (size_t round = 0; round < rounds; ++round) {
        for (int a = 0; a < kNumArms; ++a) {
            runRound(book, static_cast<Arm>(a), cfg, nextId, r.arms[a]);
        }
    }
    return r;
}

// ─── Reporting ───────────────────────────────────────────────────────────────

uint64_t g_tickNs = 0;

// Mark a value that sits inside the clock's quantization band.
char quantFlag(uint64_t ns) {
    return (g_tickNs > 0 && ns <= g_tickNs * kQuantizationTicks) ? '~' : ' ';
}

void printMatchHeader(const char* axisName) {
    std::printf("\n  %14s %5s  %-9s %10s %9s %9s %9s %10s %11s %11s %9s\n",
                axisName, "depth", "arm", "ops", "P50", "P99", "P99.9", "P99.99",
                "max", "dP99 vs ctl", "thru%ctl");
    std::printf("  %s\n", std::string(120, '-').c_str());
}

// Below this many samples a P99 is a couple of observations, not a percentile.
constexpr uint64_t kThinSamples = 1000;

double opsPerSec(const ArmStats& s) {
    const double sec = static_cast<double>(s.wallNs) / 1e9;
    return sec > 0 ? static_cast<double>(s.ops) / sec : 0.0;
}

void printMatchRows(const PointResult& p, uint64_t axisValue) {
    const uint64_t ctlP99 = p.arms[0].match.getP99();
    const double ctlRate = opsPerSec(p.arms[0]);
    for (int a = 0; a < kNumArms; ++a) {
        const ArmStats& s = p.arms[a];
        const uint64_t p99 = s.match.getP99();
        char delta[24];
        char thru[24];
        if (a == 0) {
            std::snprintf(delta, sizeof(delta), "%s", "          -");
            std::snprintf(thru, sizeof(thru), "%s", "        -");
        } else {
            std::snprintf(delta, sizeof(delta), "%+11lld",
                          (long long)p99 - (long long)ctlP99);
            // THE TAIL IS NOT THE ONLY WAY A SCRAPE HURTS. When the hold time
            // is long, the producer is not slowed one order at a time — it is
            // locked out in blocks, so its P99 can sit still while its
            // THROUGHPUT collapses. This column is that effect.
            std::snprintf(thru, sizeof(thru), "%8.0f%%",
                          ctlRate > 0 ? 100.0 * opsPerSec(s) / ctlRate : 0.0);
        }
        std::printf("  %14llu %5zu  %-9s %9llu%c %8llu%c %8llu%c %8llu%c %9llu%c %11llu %s %s\n",
                    (unsigned long long)axisValue, p.depth, armName(static_cast<Arm>(a)),
                    (unsigned long long)s.ops, s.ops < kThinSamples ? '!' : ' ',
                    (unsigned long long)s.match.getP50(), quantFlag(s.match.getP50()),
                    (unsigned long long)p99, quantFlag(p99),
                    (unsigned long long)s.match.getP999(), quantFlag(s.match.getP999()),
                    (unsigned long long)s.match.getP9999(), quantFlag(s.match.getP9999()),
                    (unsigned long long)s.match.getMax(),
                    delta, thru);
    }
}

void printSoloHeader(const char* axisName) {
    std::printf("\n  %14s %5s %9s %11s %11s %11s %11s\n",
                axisName, "depth", "samples", "holdP50", "holdP99", "holdMax",
                "duty@1k/s");
    std::printf("  %s\n", std::string(106, '-').c_str());
}

void printSoloRow(const PointResult& p, uint64_t axisValue, uint64_t rateHz) {
    // What fraction of wall time a scraper at `rateHz` would own bookLock_,
    // using the QUIESCED hold time. >= 100% means the poll rate is not
    // achievable — the book cannot be snapshotted that often at all.
    const double duty = 100.0 * static_cast<double>(rateHz) *
                        static_cast<double>(p.solo.getP50()) / 1e9;
    std::printf("  %14llu %5zu %9llu %10llu%c %10llu%c %11llu %10.2f%%%s\n",
                (unsigned long long)axisValue, p.depth,
                (unsigned long long)p.soloCount,
                (unsigned long long)p.solo.getP50(), quantFlag(p.solo.getP50()),
                (unsigned long long)p.solo.getP99(), quantFlag(p.solo.getP99()),
                (unsigned long long)p.solo.getMax(),
                duty, duty >= 100.0 ? "  (poll rate unreachable)" : "");
}

void printSnapHeader(const char* axisName) {
    std::printf("\n  %14s %5s  %-9s %8s %12s %12s %10s\n",
                axisName, "depth", "arm", "scrapes", "wait+holdP50",
                "wait+holdMax", "achieved/s");
    std::printf("  %s\n", std::string(106, '-').c_str());
}

void printSnapRows(const PointResult& p, uint64_t axisValue) {
    for (int a = 1; a < kNumArms; ++a) {  // control never scrapes
        const ArmStats& s = p.arms[a];
        const double wallSec = static_cast<double>(s.wallNs) / 1e9;
        const double achieved = wallSec > 0 ? static_cast<double>(s.scrapes) / wallSec : 0.0;
        std::printf("  %14llu %5zu  %-9s %8llu %12llu %12llu %10.0f%s\n",
                    (unsigned long long)axisValue, p.depth, armName(static_cast<Arm>(a)),
                    (unsigned long long)s.scrapes,
                    (unsigned long long)s.snap.getP50(),
                    (unsigned long long)s.snap.getMax(),
                    achieved,
                    s.behind > 0 ? "  (fell behind)" : "");
    }
}

// ─── Memory sweep ────────────────────────────────────────────────────────────

// Exact structural cost of one FlatPriceMap, computed from the header rather
// than measured. Cross-check for the RSS number; also the only figure that is
// not page-quantized.
struct MapBytes {
    uint64_t directory{0};  // slots_ + bitmap0_ + bitmap1_, one allocation
    uint64_t slab{0};       // ceil(levels / 256) blocks of 256 OrderList
    uint64_t blocks{0};
};

MapBytes computeMapBytes(size_t capacity, size_t levels) {
    MapBytes m;
    const size_t bitmap0Words = (capacity + 63) / 64;
    const size_t bitmap1Words = (bitmap0Words + 63) / 64;
    const size_t alignPad = alignof(uint64_t) - 1;
    m.directory = static_cast<uint64_t>(sizeof(uint32_t) * capacity) + alignPad +
                  static_cast<uint64_t>(sizeof(uint64_t) * bitmap0Words) +
                  static_cast<uint64_t>(sizeof(uint64_t) * bitmap1Words);
    constexpr size_t kSlabBlock = 256;  // FlatPriceMap::SLAB_BLOCK (private)
    m.blocks = (levels + kSlabBlock - 1) / kSlabBlock;
    m.slab = m.blocks * kSlabBlock * sizeof(OrderList);
    return m;
}

// NOTHING ALLOCATED HERE IS EVER FREED, and that is the whole trick.
//
// The first version of this freed each batch before building the next. Every
// delta after the first read ~0 B/map — not because the maps were free, but
// because malloc had already claimed those pages from the OS and handed the
// same resident pages back. RSS measures pages the process holds, not pages the
// allocator considers live, so a free-then-reallocate is invisible to it.
// Keeping every batch alive forces each point to be fresh growth, which is the
// only thing an RSS delta can honestly report.
//
// The Order objects are allocated and touched up front, before the first
// baseline, so no order storage lands in any map's delta.
void runMemorySweep(const std::vector<size_t>& levelPoints) {
    const size_t capacity = FlatPriceMap::DEFAULT_CAPACITY;

    std::printf("\n=== FLATPRICEMAP FOOTPRINT ===\n");
    std::printf("  Measured IN ISOLATION: %zu bare FlatPriceMap objects per point,\n",
                kMemMapBatch);
    std::printf("  RSS delta / %zu. Nothing is subtracted from a whole OrderBook,\n",
                kMemMapBatch);
    std::printf("  and no batch is ever freed (a freed batch's pages stay resident\n");
    std::printf("  and would make every later point read ~0).\n");
    std::printf("  'exact' is the structural accounting from FlatPriceMap.h: RSS is\n");
    std::printf("  page-quantized (16 KiB here) and cannot resolve one small map.\n");

    size_t totalOrders = 0;
    for (size_t n : levelPoints) totalOrders += kMemMapBatch * n;
    std::vector<Order> orders(totalOrders > 0 ? totalOrders : 1);
    volatile uint64_t touch = 0;
    for (auto& o : orders) {
        o.price = 0;
        touch += static_cast<uint64_t>(o.price);
    }
    (void)touch;

    std::vector<std::unique_ptr<FlatPriceMap>> keepAlive;
    keepAlive.reserve(kMemMapBatch * levelPoints.size());

    std::printf("\n  %10s %14s %12s %14s %12s %10s %12s\n",
                "levels", "rss B/map", "rss MiB/map", "exact B/map", "directory",
                "slab", "exact B/lvl");
    std::printf("  %s\n", std::string(96, '-').c_str());

    size_t cursor = 0;
    for (size_t levels : levelPoints) {
        const uint64_t before = rssBytes();
        for (size_t m = 0; m < kMemMapBatch; ++m) {
            auto map = std::make_unique<FlatPriceMap>(Side::Buy, capacity);
            map->setRange(0, static_cast<Price>(capacity) - 1);
            for (size_t i = 0; i < levels; ++i) {
                // One order per level: the per-level cost is the slab slot, not
                // the queue behind it. A queue order costs sizeof(Order) and
                // comes out of the book's pool, not out of this map.
                map->insert(static_cast<Price>(i), &orders[cursor + i]);
            }
            cursor += levels;
            keepAlive.push_back(std::move(map));
        }
        const uint64_t after = rssBytes();
        const uint64_t deltaPerMap = (after > before) ? (after - before) / kMemMapBatch : 0;

        const MapBytes exact = computeMapBytes(capacity, levels);
        const uint64_t exactTotal = exact.directory + exact.slab;
        const double exactPerLevel =
            levels > 0 ? static_cast<double>(exact.slab) / static_cast<double>(levels) : 0.0;

        std::printf("  %10zu %14llu %12.3f %14llu %12llu %10llu %12.1f\n",
                    levels,
                    (unsigned long long)deltaPerMap,
                    toMiB(deltaPerMap),
                    (unsigned long long)exactTotal,
                    (unsigned long long)exact.directory,
                    (unsigned long long)exact.slab,
                    exactPerLevel);
    }

    // Empty-OrderBook floor for context: what a book costs before any order
    // arrives, and how much of that is the two FlatPriceMaps. Also never freed,
    // for the same reason.
    constexpr size_t kBooks = 8;
    constexpr size_t kPoolForFloor = 10000;  // the `order_pool_capacity` default
    const uint64_t before = rssBytes();
    std::vector<std::unique_ptr<OrderBook>> books;
    books.reserve(kBooks);
    for (size_t i = 0; i < kBooks; ++i) {
        books.push_back(std::make_unique<OrderBook>(
            static_cast<SymbolId>(i), MatchAlgorithm::PriceTime, kPoolForFloor));
    }
    const uint64_t after = rssBytes();
    const uint64_t perBook = (after > before) ? (after - before) / kBooks : 0;
    const MapBytes emptyMap = computeMapBytes(capacity, 0);
    std::printf("\n  Empty OrderBook floor (%zu books, pool=%zu): %.2f MiB/book\n",
                kBooks, kPoolForFloor, toMiB(perBook));
    std::printf("  of which bids_ + asks_ (2 x empty FlatPriceMap): %.2f MiB "
                "(%.0f%% of the floor)\n",
                toMiB(2 * emptyMap.directory),
                perBook > 0 ? 100.0 * static_cast<double>(2 * emptyMap.directory) /
                                  static_cast<double>(perBook)
                            : 0.0);
}

// ─── Sweeps ──────────────────────────────────────────────────────────────────

void runQueueDepthSweep(const RoundConfig& base, size_t rounds,
                        const std::vector<size_t>& depths) {
    // MAX_DEPTH levels per side, so depth=20 has a full set of levels to walk.
    const size_t levels = MarketDataSnapshot::MAX_DEPTH;
    const std::vector<size_t> queuePoints = {1, 10, 100, 1000, 10000};

    std::printf("\n=== SWEEP 1: ORDERS PER LEVEL (the axis that matters) ===\n");
    std::printf("  %zu price levels per side, queue depth swept at the touch.\n", levels);
    std::printf("  getSnapshot walks every order inside each of the first `depth`\n");
    std::printf("  levels, so cost here is O(depth x orders-per-level).\n");

    for (size_t depth : depths) {
        std::vector<PointResult> results;
        for (size_t q : queuePoints) results.push_back(runPoint(levels, q, depth, base, rounds));

        std::printf("\n-- MATCHING PATH (addOrder), depth=%zu --", depth);
        printMatchHeader("orders/level");
        for (size_t i = 0; i < results.size(); ++i) {
            printMatchRows(results[i], queuePoints[i]);
        }
        std::printf("\n-- SNAPSHOT LOCK HOLD (quiesced), depth=%zu --", depth);
        printSoloHeader("orders/level");
        for (size_t i = 0; i < results.size(); ++i) {
            printSoloRow(results[i], queuePoints[i], base.rateHz);
        }
        std::printf("\n-- SCRAPER UNDER LOAD, depth=%zu --", depth);
        printSnapHeader("orders/level");
        for (size_t i = 0; i < results.size(); ++i) {
            printSnapRows(results[i], queuePoints[i]);
        }
    }
}

void runLevelSweep(const RoundConfig& base, size_t rounds,
                   const std::vector<size_t>& depths) {
    const size_t ordersPerLevel = 10;
    const std::vector<size_t> levelPoints = {10, 100, 1000, 10000};

    std::printf("\n=== SWEEP 2: PRICE LEVELS (the wrong axis, on the record) ===\n");
    std::printf("  %zu orders per level, level COUNT swept. forEachLevelWhile\n",
                ordersPerLevel);
    std::printf("  returns false past `depth`, so levels beyond it are never\n");
    std::printf("  walked and this sweep is expected to be FLAT.\n");

    for (size_t depth : depths) {
        std::vector<PointResult> results;
        for (size_t n : levelPoints) results.push_back(runPoint(n, ordersPerLevel, depth, base, rounds));

        std::printf("\n-- MATCHING PATH (addOrder), depth=%zu --", depth);
        printMatchHeader("price levels");
        for (size_t i = 0; i < results.size(); ++i) {
            printMatchRows(results[i], levelPoints[i]);
        }
        std::printf("\n-- SNAPSHOT LOCK HOLD (quiesced), depth=%zu --", depth);
        printSoloHeader("price levels");
        for (size_t i = 0; i < results.size(); ++i) {
            printSoloRow(results[i], levelPoints[i], base.rateHz);
        }
        std::printf("\n-- SCRAPER UNDER LOAD, depth=%zu --", depth);
        printSnapHeader("price levels");
        for (size_t i = 0; i < results.size(); ++i) {
            printSnapRows(results[i], levelPoints[i]);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    RoundConfig base;
    size_t rounds = 3;
    char only = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasNext = (i + 1 < argc);
        if (arg == "--ops" && hasNext) {
            base.opsTarget = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--rounds" && hasNext) {
            rounds = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--scrape-rate" && hasNext) {
            base.rateHz = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--budget-ms" && hasNext) {
            base.budgetMs = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--only" && hasNext) {
            only = argv[++i][0];
        } else {
            std::printf("unknown argument: %s\n", arg.c_str());
            std::printf("usage: %s [--ops N] [--rounds N] [--scrape-rate HZ] "
                        "[--budget-ms MS] [--only q|l|m]\n", argv[0]);
            return 2;
        }
    }
    if (rounds == 0) rounds = 1;

    // Pin the working set: order-pool capacity is deployment config
    // (OB_ORDER_POOL_CAPACITY, default 10,000), and the deepest book built here
    // needs 400,000 slots. overwrite=0 so an explicit value from the caller
    // still wins.
    ::setenv("OB_ORDER_POOL_CAPACITY", kPoolCapacity, /*overwrite=*/0);

    g_tickNs = measureClockTickNs();

    std::printf("=======================================================\n");
    std::printf("  SNAPSHOT CONTENTION BENCHMARK\n");
    std::printf("  Does an admin /book or /audit scrape stall matching?\n");
    std::printf("=======================================================\n");
    std::printf("  clock tick:      %llu ns  (values <= %llu ns flagged '~')\n",
                (unsigned long long)g_tickNs,
                (unsigned long long)(g_tickNs * kQuantizationTicks));
    std::printf("  ops/round:       %llu (or %llu ms, whichever first)\n",
                (unsigned long long)base.opsTarget, (unsigned long long)base.budgetMs);
    std::printf("  rounds:          %zu, arms INTERLEAVED per round (thermal drift)\n", rounds);
    std::printf("  paced scrape:    %llu req/s\n", (unsigned long long)base.rateHz);
    std::printf("  order pool:      %s slots\n", ::getenv("OB_ORDER_POOL_CAPACITY"));
    std::printf("  snapshot depths: 10 (default) and %zu (MarketDataSnapshot::MAX_DEPTH)\n",
                MarketDataSnapshot::MAX_DEPTH);

    const std::vector<size_t> depths = {10, MarketDataSnapshot::MAX_DEPTH};

    // MEMORY FIRST, AND THAT ORDER IS LOAD-BEARING. The contention sweeps build
    // and destroy 400,000-order books; the pages they free stay resident and
    // get handed back to the map batches, so a memory sweep run after them
    // under-reports (measured: 0.67 MiB vs 0.80 MiB for an empty map, and a
    // 9.09 vs 10.35 MiB OrderBook floor). Run on a fresh allocator it agrees
    // with the 10.3 MiB/book already documented in OrderBook.h.
    if (only == 0 || only == 'm') runMemorySweep({0, 10, 100, 1000, 10000});
    if (only == 0 || only == 'q') runQueueDepthSweep(base, rounds, depths);
    if (only == 0 || only == 'l') runLevelSweep(base, rounds, depths);

    std::printf("\n=======================================================\n");
    std::printf("  HOW TO READ THIS\n");
    std::printf("  - dP99 is the matching path's P99 under scrape minus its P99\n");
    std::printf("    with the scraper idle. That, not the snapshot's own\n");
    std::printf("    latency, is whether monitoring can stop trading.\n");
    std::printf("  - A dP99 of 0 at a low scrape rate is NOT proof of safety on\n");
    std::printf("    its own: read it with 'duty@%llu/s' from the hold table.\n",
                (unsigned long long)base.rateHz);
    std::printf("    Duty d%% means ~d%% of arriving orders block, so the stall\n");
    std::printf("    lands at P(100-d) — which can be past P99 entirely. The\n");
    std::printf("    flat-out arm is the upper bound.\n");
    std::printf("  - holdP50 is QUIESCED: no producer running, so the interval\n");
    std::printf("    is the lock HOLD. The scraper-under-load column is\n");
    std::printf("    WAIT+HOLD and is dominated by waiting for the producer,\n");
    std::printf("    which runs flat out and owns the lock almost continuously.\n");
    std::printf("    Do not read it as a hold time.\n");
    std::printf("  - thru%%ctl is the arm's order throughput as a percentage of\n");
    std::printf("    its own control. A long hold does NOT slow the producer one\n");
    std::printf("    order at a time — it locks it out in blocks. So P99 can sit\n");
    std::printf("    still while throughput collapses; read both columns.\n");
    std::printf("  - '!' on an ops count marks a row thinned below %llu samples\n",
                (unsigned long long)kThinSamples);
    std::printf("    by the time budget — its P99 is a handful of observations,\n");
    std::printf("    not a percentile. Those rows show starvation, not a tail.\n");
    std::printf("  - '~' marks a figure inside the clock's quantization band.\n");
    std::printf("    'below clock resolution' percentiles are unresolvable, not\n");
    std::printf("    fast.\n");
    std::printf("=======================================================\n");
    return 0;
}
