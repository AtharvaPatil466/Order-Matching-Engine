// HonestBenchmark — the same deterministic order flow through four paths,
// from leanest to fullest stack:
//
//   Path A: OrderBook::addOrder() only
//           Core matching engine with STP + WashTrade + LULD
//
//   Path B: MatchingEngine::submitOrder() (sync mode, no journal)
//           Adds: sequence allocation, rate limiter check, risk validation
//
//   Path C: Path B + journal, SyncPolicy::GroupCommit (batch=64)
//           The ack PRECEDES durability: up to 63 acknowledged orders are not
//           yet synced, and on Linux+io_uring the sync itself is reaped
//           asynchronously. This is "submit to ack", not "submit to durable".
//
//   Path D: Path B + journal, SyncPolicy::Immediate
//           Every append is synced before submitOrder returns, so on the sync
//           path the ack is durable. This is "submit to durable ack" (BENCH-4,
//           PERF-6), and it is fsync-bound: read it next to the filesystem
//           type the run prints.
//
// The journal goes to --journal PATH (default ./honest_benchmark.journal, NOT
// /tmp, which is tmpfs on many hosts). The run prints that path's filesystem
// type; a tmpfs result says nothing about disk durability.
//
// ORDER FLOW:
//   Every path processes the IDENTICAL order stream: same IDs, prices,
//   quantities and sequence. 50% buys, 50% sells, priced around a midpoint
//   that random-walks by walkDist(-200, 200) per order, with an offset of
//   spreadDist(-5000, 5000). No cancels.
//
//   Every order is ACCEPTED, but only some TRADE: at seed 42 about 44% of
//   orders trade on arrival and the rest rest. Earlier revisions printed
//   "Fills: 50,000 (100.0% fill rate)" and called this a 100%-crossing flow;
//   that counter counted accepted orders (GTPERF-3). The run now prints the
//   share that traded and the trade count, and fails (exit 1) if any order is
//   rejected or the paths disagree on the trades — identical input must
//   produce identical matching.
//
//   It is a reproducible FLOOR on matching latency under favourable
//   conditions, not an expected operating latency.
//
// METHODOLOGY:
//   - Warm-up: the first --warmup orders (default 5,000) run through each path
//     untimed; the next --orders (default 50,000) are timed.
//   - Clock: steady_clock (bench::nowNs) — monotonic on every platform.
//   - Each order timed individually: start = nowNs(), end = nowNs()
//   - Statistics: min, P50, P90, P99, P99.9, P99.99, max, mean
//
// WHAT IS NOT MEASURED:
//   - Network I/O (FIX parsing, TCP accept)
//   - Async queue delay (all paths are synchronous)
//   - OS scheduling jitter (no core pinning in this benchmark)
//
// Usage:
//   HonestBenchmark [--orders N] [--warmup N] [--seed S] [--journal PATH]
//                   [--no-journal] [--no-immediate] [--only a|b|c|d]

#include "MatchingEngine.h"
#include "BenchLatencyRecorder.h"  // vendored recorder — full P50..P99.99 tail (P2-21)
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <sys/mount.h>
#else
#include <sys/vfs.h>
#endif

using namespace OrderMatcher;

// ─── Deterministic Order Generator ──────────────────────────────────────────

struct TestOrder {
    OrderId orderId;
    ParticipantId participantId;
    Side side;
    Price price;
    Quantity qty;
};

class OrderGenerator {
public:
    OrderGenerator(uint64_t seed, size_t count, Price startMid = 10000000)
        : rng_(seed), midPrice_(startMid) {
        orders_.reserve(count);
        std::uniform_int_distribution<Quantity> qtyDist(1, 100);
        std::uniform_int_distribution<int64_t> walkDist(-200, 200);
        std::uniform_int_distribution<int64_t> spreadDist(-5000, 5000);
        std::uniform_int_distribution<ParticipantId> pidDist(1, 20);

        for (size_t i = 0; i < count; ++i) {
            midPrice_ = static_cast<Price>(
                std::max(static_cast<int64_t>(100000),
                         static_cast<int64_t>(midPrice_) + walkDist(rng_)));

            TestOrder ord;
            ord.orderId = static_cast<OrderId>(i + 1);
            ord.participantId = pidDist(rng_);
            ord.side = (rng_() & 1) ? Side::Buy : Side::Sell;
            const int64_t offset = spreadDist(rng_);
            ord.price = static_cast<Price>(
                std::max(static_cast<int64_t>(100), static_cast<int64_t>(midPrice_) + offset));
            ord.qty = qtyDist(rng_);
            orders_.push_back(ord);
        }
    }

    const std::vector<TestOrder>& orders() const { return orders_; }

private:
    std::mt19937_64 rng_;
    Price midPrice_;
    std::vector<TestOrder> orders_;
};

// ─── Result Reporting ───────────────────────────────────────────────────────

struct PathResult {
    const char* name = "";
    bool ran = false;
    bench::BenchLatencyRecorder tracker;  // vendored recorder (P2-21)
    uint64_t rejected{0};  // over warmup + timed orders
    uint64_t traded{0};    // timed orders that traded on arrival
    uint64_t trades{0};    // fills over warmup + timed orders
    double durationSec{0};
};

static void printResult(const PathResult& r) {
    const uint64_t n = r.tracker.getCount();
    std::printf("\n── %s ──\n", r.name);
    std::printf("  Orders:     %llu timed, %llu rejected\n", (unsigned long long)n,
                (unsigned long long)r.rejected);
    std::printf("  Traded:     %llu of the timed orders (%.1f%%); %llu fills in all\n",
                (unsigned long long)r.traded,
                n > 0 ? 100.0 * static_cast<double>(r.traded) / static_cast<double>(n) : 0.0,
                (unsigned long long)r.trades);
    std::printf("  Throughput: %.0f orders/sec\n",
                r.durationSec > 0 ? static_cast<double>(n) / r.durationSec : 0);
    std::printf("  ─────────────────────────\n");
    std::printf("  Min:     %6llu ns\n", (unsigned long long)r.tracker.getMin());
    std::printf("  P50:     %6llu ns\n", (unsigned long long)r.tracker.getP50());
    std::printf("  P90:     %6llu ns\n", (unsigned long long)r.tracker.getP90());
    std::printf("  P99:     %6llu ns\n", (unsigned long long)r.tracker.getP99());
    std::printf("  P99.9:   %6llu ns\n", (unsigned long long)r.tracker.getP999());
    std::printf("  P99.99:  %6llu ns\n", (unsigned long long)r.tracker.getP9999());
    std::printf("  Max:     %6llu ns\n", (unsigned long long)r.tracker.getMax());
    std::printf("  Mean:    %6.0f ns\n", r.tracker.getMean());
}

// The filesystem the journal actually lands on. A Path C/D number means
// nothing without it: tmpfs syncs are free, network block storage costs ms.
// Exits when the journal file is not there: enableJournal() returns true for a
// path it could not create, and the run would time a journal that writes
// nothing.
static std::string filesystemType(const std::string& path) {
    struct statfs st {};
    if (statfs(path.c_str(), &st) != 0) {
        std::fprintf(stderr, "journal %s is not usable: %s\n", path.c_str(), std::strerror(errno));
        std::exit(1);
    }
#if defined(__APPLE__)
    return st.f_fstypename;
#else
    switch (static_cast<unsigned long>(st.f_type)) {
        case 0xEF53:     return "ext2/3/4";
        case 0x58465342: return "xfs";
        case 0x9123683E: return "btrfs";
        case 0x2FC12FC1: return "zfs";
        case 0x794C7630: return "overlayfs";
        case 0x6969:     return "nfs";
        case 0x01021994: return "tmpfs (RAM: says nothing about disk durability)";
        case 0x65735546: return "fuse (userspace: a sync may not reach a disk)";
        case 0x6A656A63: return "virtiofs (host passthrough: a sync may not reach a disk)";
        default: {
            char buf[48];
            std::snprintf(buf, sizeof buf, "unrecognised (f_type 0x%lx)",
                          static_cast<unsigned long>(st.f_type));
            return buf;
        }
    }
#endif
}

// One loop for every path: run `orders` through `submit` (true = accepted),
// timing only the orders after `warmup`. `tradeCount` reads the book's fill
// counter, untimed, to tell an order that traded from one that rested.
template <typename Submit, typename TradeCount>
static void runOrders(PathResult& r, const std::vector<TestOrder>& orders, size_t warmup,
                      Submit submit, TradeCount tradeCount) {
    r.ran = true;
    const uint64_t tradesAtStart = tradeCount();
    auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < orders.size(); ++i) {
        if (i == warmup) start = std::chrono::steady_clock::now();
        const uint64_t before = tradeCount();
        const uint64_t t0 = bench::nowNs();
        const bool accepted = submit(orders[i]);
        const uint64_t t1 = bench::nowNs();
        r.rejected += !accepted;
        if (i < warmup) continue;
        r.tracker.recordInterval(t0, t1);
        r.traded += tradeCount() > before;
    }
    r.durationSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    r.trades = tradeCount() - tradesAtStart;
}

// ─── Path A: OrderBook::addOrder() only ─────────────────────────────────────

static PathResult runPathA(const std::vector<TestOrder>& orders, size_t warmup) {
    PathResult result;
    result.name = "Path A: OrderBook::addOrder() [core matching + STP/WashTrade/LULD]";
    OrderBook book(0, MatchAlgorithm::PriceTime);
    runOrders(result, orders, warmup,
              [&](const TestOrder& o) {
                  return std::holds_alternative<OrderId>(book.addOrder(
                      o.orderId, o.participantId, o.side, o.price, o.qty, OrderType::Limit));
              },
              [&] { return book.getTradeCount(); });
    return result;
}

// ─── Paths B, C, D: MatchingEngine::submitOrder() sync ──────────────────────

static PathResult runEnginePath(const char* name, const std::vector<TestOrder>& orders,
                                size_t warmup, const std::string* journalPath,
                                Journal::SyncPolicy policy) {
    PathResult result;
    result.name = name;

    auto engine = std::make_unique<MatchingEngine>();
    engine->addSymbol(0);
    if (journalPath) {
        std::remove(journalPath->c_str());  // clean slate
        if (!engine->enableJournal(*journalPath, policy)) {
            std::fprintf(stderr, "cannot open journal at %s\n", journalPath->c_str());
            return result;
        }
        std::printf("\n  journal %s on %s\n", journalPath->c_str(),
                    filesystemType(*journalPath).c_str());
    }
    engine->start();
    const OrderBook* book = engine->getOrderBook(0);
    runOrders(result, orders, warmup,
              [&](const TestOrder& o) {
                  return engine->submitOrder(0, o.orderId, o.participantId, o.side, o.price,
                                             o.qty, OrderType::Limit)
                      .isAccepted();
              },
              [&] { return book->getTradeCount(); });
    engine->stop();
    if (journalPath) std::remove(journalPath->c_str());
    return result;
}

// ─── Main ───────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    size_t orderCount = 50000;
    size_t warmup = 5000;
    uint64_t seed = 42;
    bool runJournal = true;
    bool runImmediate = true;
    std::string journalPath = "honest_benchmark.journal";

    // --only <a|b|c|d>: run exactly one path.
    //
    // THIS EXISTS SO `perf stat` CAN BE SCOPED. Wrapping perf around the whole
    // process and dividing by the order count charges every path's work to
    // whichever one you happen to label the output — which is how this
    // benchmark's published counters came to attribute Path C's fdatasync
    // traffic to Path A core matching (see BENCHMARKS.md). A counter gathered
    // over several paths cannot be attributed to one of them after the fact,
    // so the isolation has to happen at run time.
    char onlyPath = 0;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--orders") == 0 && i + 1 < argc)
            orderCount = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--warmup") == 0 && i + 1 < argc)
            warmup = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
            seed = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--journal") == 0 && i + 1 < argc)
            journalPath = argv[++i];
        else if (std::strcmp(argv[i], "--no-journal") == 0)
            runJournal = false;
        else if (std::strcmp(argv[i], "--no-immediate") == 0)
            runImmediate = false;
        else if (std::strcmp(argv[i], "--only") == 0 && i + 1 < argc) {
            const char* v = argv[++i];
            if (v[0] == '\0' || v[1] != '\0' || v[0] < 'a' || v[0] > 'd') {
                std::fprintf(stderr, "--only takes exactly one of: a, b, c, d (got \"%s\")\n", v);
                return 2;
            }
            onlyPath = v[0];
        }
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("Usage: HonestBenchmark [--orders N] [--warmup N] [--seed S]\n"
                        "                       [--journal PATH] [--no-journal]\n"
                        "                       [--no-immediate] [--only a|b|c|d]\n\n"
                        "  --journal PATH  journal file for Paths C and D (default\n"
                        "                  ./honest_benchmark.journal); its filesystem\n"
                        "                  type is printed. Put it on the storage you\n"
                        "                  would deploy on.\n"
                        "  --no-journal    skip Paths C and D\n"
                        "  --no-immediate  skip Path D (one fsync per order)\n"
                        "  --only a|b|c|d  Run exactly ONE path. Use this when wrapping the\n"
                        "                  run in `perf stat`: counters taken over several\n"
                        "                  paths cannot afterwards be attributed to one.\n");
            return 0;
        }
        else {
            std::fprintf(stderr, "unknown argument: \"%s\" (try --help)\n", argv[i]);
            return 2;
        }
    }
    auto wanted = [&](char p) { return onlyPath == 0 || onlyPath == p; };

    std::printf("=======================================================\n");
    std::printf("  Honest Benchmark — Four-Path Latency Measurement\n");
    std::printf("=======================================================\n\n");

    std::printf("Configuration:\n");
    std::printf("  Orders:      %zu timed after %zu warm-up\n", orderCount, warmup);
    std::printf("  Seed:        %llu\n", (unsigned long long)seed);
    std::printf("  Journal:     %s\n", runJournal ? journalPath.c_str() : "disabled");
    std::printf("  Platform:    %s\n",
#if defined(__aarch64__)
    "ARM64"
#elif defined(__x86_64__)
    "x86_64"
#else
    "unknown"
#endif
    );

    std::printf("\nNOT MEASURED: network I/O, async queue delay, OS scheduling\n");

    OrderGenerator gen(seed, warmup + orderCount);
    const auto& orders = gen.orders();

    PathResult a, b, c, d;
    if (wanted('a')) { a = runPathA(orders, warmup); printResult(a); }
    if (wanted('b')) {
        b = runEnginePath("Path B: MatchingEngine::submitOrder() [+ sequence alloc, rate limiter]",
                          orders, warmup, nullptr, Journal::SyncPolicy::GroupCommit);
        printResult(b);
    }
    if (runJournal && wanted('c')) {
        c = runEnginePath("Path C: + Journal GroupCommit/64 [ack BEFORE durability]",
                          orders, warmup, &journalPath, Journal::SyncPolicy::GroupCommit);
        printResult(c);
    }
    if (runJournal && runImmediate && wanted('d')) {
        d = runEnginePath("Path D: + Journal Immediate [submit to DURABLE ack]",
                          orders, warmup, &journalPath, Journal::SyncPolicy::Immediate);
        printResult(d);
    }

    // Workload check: the numbers above describe this flow only if every order
    // was accepted, and identical input must give identical matching.
    bool workloadOk = true;
    const PathResult* ran[] = {&a, &b, &c, &d};
    const PathResult* first = nullptr;
    for (const PathResult* r : ran) {
        if (!r->ran) continue;
        if (r->rejected != 0 || r->tracker.getCount() != orderCount) workloadOk = false;
        if (!first) first = r;
        else if (r->trades != first->trades) workloadOk = false;
    }
    if (!workloadOk) {
        std::fprintf(stderr, "\nWORKLOAD CHECK FAILED: a path rejected orders, timed fewer than "
                             "%zu, or the paths disagree on the trades\n", orderCount);
        return 1;
    }

    std::printf("\n── Overhead Breakdown (P50) ──\n");
    // Every line is a DIFFERENCE between two paths, so it needs both. Under
    // --only the skipped path is empty, and a subtraction from it would print a
    // confident number computed from a zero.
    auto diff = [](const PathResult& x, const PathResult& y) {
        return static_cast<long long>(x.tracker.getP50()) -
               static_cast<long long>(y.tracker.getP50());
    };
    if (a.ran) std::printf("  Core matching + compliance:   %lld ns (Path A P50)\n",
                           static_cast<long long>(a.tracker.getP50()));
    if (a.ran && b.ran) std::printf("  Engine overhead (seq+rate):    %+lld ns (Path B - A)\n", diff(b, a));
    if (b.ran && c.ran) std::printf("  Journal, ack before durable:   %+lld ns (Path C - B)\n", diff(c, b));
    if (b.ran && d.ran) std::printf("  Journal, durable ack:          %+lld ns (Path D - B)\n", diff(d, b));
    if (onlyPath) std::printf("  (--only %c: differences between paths suppressed)\n", onlyPath);

    std::printf("\n=======================================================\n");
    return 0;
}
