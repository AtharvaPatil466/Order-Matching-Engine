// CheckpointEquivalenceTest — two engines that agree on a book make identical
// decisions from then on, however one of them got there.
//
// Per seed: a randomized flow runs into engine A, which journals it. At a
// random point the journal is copied (C's source: the plain journal), A takes a
// checkpoint, and the checkpointed journal is copied (B's source). B replays
// the checkpoint and C the plain journal, each into a fresh engine. The rest
// of the flow then runs on A, B and C, and after every operation each must
// have emitted the same trades and order updates and hold the same book:
// every resting and parked order in queue order with its age, fills, slice,
// trigger and latch, the trading state and the last trade.
//
// The flow covers continuous trading, call periods and uncrosses, halts and
// PostClose, stops, stop-limits, MIT and trailing stops, pegs, icebergs and
// hidden orders, post-only, IOC/FOK/Market, modifies and replaces, and three
// participants trading against themselves under different STP modes. Like
// DifferentialFuzzFlow.h it draws only with rng() % n, so a seed reproduces on
// libc++ and libstdc++ alike; it does not reuse that header's Op, which has no
// session, stop, peg or iceberg operations.
//
// Not covered: expiry (GTD/DAY) and the volatility auction's dwell, which
// need the expiry clock; the plain journal does not carry when a dwell
// started (docs/MatchingRules.md), so C would legitimately differ there.
//
//   CheckpointEquivalenceTest [firstSeed] [seeds] [ops] [cut]

#include "MatchingEngine.h"
#include "TempPath.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <tuple>
#include <vector>

using namespace OrderMatcher;
namespace fs = std::filesystem;

namespace {

constexpr SymbolId S = 0;
constexpr Price kMid = 10000;

struct Op {
    enum class Kind : uint8_t { Add, Cancel, Modify, Replace, State, Uncross };
    Kind kind = Kind::Add;
    OrderId id = 0;
    ParticipantId owner = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    Price price = 0, stopPrice = 0, stopLimitPrice = 0, pegOffset = 0, trail = 0;
    Quantity qty = 0, display = 0;
    PegType peg = PegType::None;
    bool hidden = false;
    TradingState state = TradingState::Continuous;
};

class Generator {
public:
    explicit Generator(uint64_t seed) : rng_(seed) {}

    std::vector<Op> flow(size_t n) {
        std::vector<Op> ops;
        ops.reserve(n);
        for (size_t i = 0; i < n; ++i) ops.push_back(next());
        return ops;
    }

private:
    uint64_t pick(uint64_t n) { return rng_() % n; }
    Price near() { return kMid - 15 + static_cast<Price>(pick(31)); }
    OrderId anyId() { return nextId_ > 1 ? 1 + pick(nextId_ - 1) : 1; }

    Op next() {
        Op op;
        const uint64_t r = pick(100);
        if (r < 3) {
            static constexpr TradingState kStates[] = {
                TradingState::Continuous, TradingState::Continuous, TradingState::AuctionOpen,
                TradingState::AuctionOpen, TradingState::Halted, TradingState::AuctionClose,
                TradingState::PreOpen, TradingState::PostClose};
            op.kind = Op::Kind::State;
            op.state = kStates[pick(sizeof(kStates) / sizeof(kStates[0]))];
            return op;
        }
        if (r < 4) { op.kind = Op::Kind::Uncross; return op; }
        if (r < 14) { op.kind = Op::Kind::Cancel; op.id = anyId(); return op; }
        if (r < 19) {
            op.kind = Op::Kind::Modify; op.id = anyId(); op.qty = 1 + pick(8); return op;
        }
        if (r < 25) {
            op.kind = Op::Kind::Replace; op.id = anyId(); op.price = near();
            op.qty = 1 + pick(12);
            return op;
        }
        op.kind = Op::Kind::Add;
        op.id = nextId_++;
        op.owner = 1 + static_cast<ParticipantId>(pick(3));
        op.side = pick(2) ? Side::Buy : Side::Sell;
        op.price = near();
        op.qty = 1 + pick(12);
        static constexpr OrderType kTypes[] = {
            OrderType::Limit, OrderType::Limit, OrderType::Limit, OrderType::Limit,
            OrderType::Market, OrderType::IOC, OrderType::FOK, OrderType::PostOnly,
            OrderType::Iceberg, OrderType::Hidden, OrderType::Stop, OrderType::StopLimit,
            OrderType::MIT, OrderType::TrailingStop, OrderType::Pegged, OrderType::MOC,
            OrderType::LOC};
        op.type = kTypes[pick(sizeof(kTypes) / sizeof(kTypes[0]))];
        const bool buy = op.side == Side::Buy;
        switch (op.type) {
        case OrderType::Iceberg: op.display = 1 + pick(4); break;
        case OrderType::Stop:
        case OrderType::StopLimit:
            op.stopPrice = buy ? kMid + static_cast<Price>(pick(10))
                               : kMid - static_cast<Price>(pick(10));
            if (op.type == OrderType::StopLimit)
                op.stopLimitPrice = op.stopPrice + (buy ? 2 : -2);
            break;
        case OrderType::MIT:
            op.stopPrice = buy ? kMid - static_cast<Price>(pick(10))
                               : kMid + static_cast<Price>(pick(10));
            break;
        case OrderType::TrailingStop: op.trail = 1 + static_cast<Price>(pick(6)); break;
        case OrderType::Pegged:
            op.peg = pick(2) ? PegType::PrimaryPeg : PegType::MidPeg;
            op.pegOffset = static_cast<Price>(pick(3));
            break;
        case OrderType::Market:
        case OrderType::MOC:
            op.price = 0;
            break;
        default: break;
        }
        op.hidden = op.type == OrderType::Limit && pick(10) == 0;
        return op;
    }

    std::mt19937_64 rng_;
    OrderId nextId_ = 1;
};

// What a book emits, in order.
struct Recorder : EventListener {
    std::vector<std::tuple<uint64_t, OrderId, OrderId, Price, Quantity>> trades;
    std::vector<std::tuple<OrderId, int, Quantity, Quantity, Price, int>> updates;
    void onTrade(const Trade& t) override {
        trades.emplace_back(t.tradeId, t.buyOrderId, t.sellOrderId, t.price, t.quantity);
    }
    void onOrderUpdate(const OrderUpdate& u) override {
        updates.emplace_back(u.orderId, static_cast<int>(u.status), u.filledQty,
                             u.remainingQty, u.lastFillPrice, static_cast<int>(u.rejectReason));
    }
    bool operator==(const Recorder& o) const {
        return trades == o.trades && updates == o.updates;
    }
    void clear() { trades.clear(); updates.clear(); }
};

// Not Order::status: nothing decides on a resting order's status, and the live
// engine leaves an aggressor that rested after a partial fill at Accepted,
// where a restore says PartiallyFilled. The statuses clients see are in the
// order updates, which are compared.
using OrderKey = std::tuple<OrderId, ParticipantId, int, int, Price, Quantity, Quantity, Quantity,
                            Price, bool, Price, uint64_t>;

struct BookState {
    std::vector<OrderKey> orders;   // forEachOrderLocked order: levels front to back, then lists
    std::vector<OrderId> pegs;      // the reprice pass's order
    TradingState state{};
    Price lastTrade = 0;
    Quantity lastTradeQty = 0;
    Price reference = 0;   // the breaker's, and the uncross's tie-break
    bool operator==(const BookState&) const = default;
};

BookState capture(MatchingEngine& e) {
    BookState s;
    OrderBook* b = e.getOrderBook(S);
    b->forEachOrderLocked([&](const Order& o) {
        // A stop's trigger fields mean nothing once it has become the order
        // it fires as; nothing reads them, so they are not compared then.
        const bool stop = o.type == OrderType::Stop || o.type == OrderType::StopLimit ||
                          o.type == OrderType::MIT || o.type == OrderType::TrailingStop;
        s.orders.emplace_back(o.id, o.participantId, static_cast<int>(o.side),
                              static_cast<int>(o.type), o.price, o.remainingQty, o.cumQty,
                              o.type == OrderType::Iceberg ? o.visibleQty : 0,
                              stop ? o.stopPrice : 0, stop && o.isStopTriggered,
                              o.type == OrderType::TrailingStop ? o.trailRefPrice : 0,
                              o.arrivalSeq);
    });
    b->forEachPeggedOrderLocked([&](const Order& o) { s.pegs.push_back(o.id); });
    s.state = b->getTradingState();
    const MarketDataSnapshot snap = b->getSnapshot();
    s.lastTrade = snap.lastTradePrice;
    s.lastTradeQty = snap.lastTradeQty;
    s.reference = b->checkpointPricesLocked().reference;
    return s;
}

// The breaker's band for the run. Off (1e9) leaves every seed in the states the
// flow drives; kTightBand trips it on this flow's few-tick moves, so stops are
// elected and latched in a volatility auction and replay has to trip at the
// same fill as live did. No expiry clock runs, so a tripped book stays in the
// auction until the flow's own state changes move it.
constexpr double kBreakerOff = 1e9;
constexpr double kTightBand = 0.0005;
double gBreakerThreshold = kBreakerOff;

void setup(MatchingEngine& e) {
    e.addSymbol(S);
    OrderBook* b = e.getOrderBook(S);
    b->setCircuitBreakerThreshold(gBreakerThreshold);
    b->setSTPMode(2, STPMode::CancelResting);
    b->setSTPMode(3, STPMode::DecreaseAndCancel);
}

void apply(MatchingEngine& e, const Op& op) {
    switch (op.kind) {
    case Op::Kind::Add:
        (void)e.submitOrder(S, op.id, op.owner, op.side, op.price, op.qty, op.type, op.stopPrice,
                            op.display, TimeInForce::GTC, 0, op.stopLimitPrice, op.peg,
                            op.pegOffset, op.trail, 0, op.hidden);
        break;
    case Op::Kind::Cancel: e.cancelOrder(S, op.id); break;
    case Op::Kind::Modify: (void)e.modifyOrder(S, op.id, op.qty); break;
    case Op::Kind::Replace: (void)e.cancelReplace(S, op.id, op.price, op.qty); break;
    case Op::Kind::State: (void)e.setTradingStateBatch({S}, op.state); break;
    case Op::Kind::Uncross: (void)e.uncrossBatch({S}); break;
    }
}

const char* kindName(Op::Kind k) {
    switch (k) {
    case Op::Kind::Add: return "add";
    case Op::Kind::Cancel: return "cancel";
    case Op::Kind::Modify: return "modify";
    case Op::Kind::Replace: return "replace";
    case Op::Kind::State: return "state";
    case Op::Kind::Uncross: return "uncross";
    }
    return "?";
}

void printBook(const char* tag, const BookState& s) {
    std::printf("      %s state=%d last=%lld/%llu ref=%lld\n", tag, static_cast<int>(s.state),
                static_cast<long long>(s.lastTrade), static_cast<unsigned long long>(s.lastTradeQty),
                static_cast<long long>(s.reference));
    for (const auto& o : s.orders)
        std::printf("        #%llu p%llu side=%d type=%d px=%lld rem=%llu cum=%llu vis=%llu stop=%lld "
                    "trig=%d ref=%lld age=%llu\n",
                    static_cast<unsigned long long>(std::get<0>(o)),
                    static_cast<unsigned long long>(std::get<1>(o)), std::get<2>(o),
                    std::get<3>(o), static_cast<long long>(std::get<4>(o)),
                    static_cast<unsigned long long>(std::get<5>(o)),
                    static_cast<unsigned long long>(std::get<6>(o)),
                    static_cast<unsigned long long>(std::get<7>(o)),
                    static_cast<long long>(std::get<8>(o)), std::get<9>(o),
                    static_cast<long long>(std::get<10>(o)),
                    static_cast<unsigned long long>(std::get<11>(o)));
}

void printEvents(const char* tag, const Recorder& r) {
    std::printf("      %s trades:", tag);
    for (const auto& [id, b, s, p, q] : r.trades)
        std::printf(" t%llu(%llu/%llu %llu@%lld)", static_cast<unsigned long long>(id),
                    static_cast<unsigned long long>(b), static_cast<unsigned long long>(s),
                    static_cast<unsigned long long>(q), static_cast<long long>(p));
    std::printf("\n      %s updates:", tag);
    for (const auto& [id, st, f, rem, px, why] : r.updates)
        std::printf(" #%llu:%d f%llu r%llu r%d", static_cast<unsigned long long>(id), st,
                    static_cast<unsigned long long>(f), static_cast<unsigned long long>(rem), why);
    std::printf("\n");
}

struct Replica {
    MatchingEngine engine;
    Recorder events;
};

void boot(Replica& r, const std::string& journal) {
    setup(r.engine);
    assert(r.engine.enableJournal(journal, Journal::SyncPolicy::GroupCommit));
    r.engine.replayJournal();
    r.engine.start();
    r.engine.getOrderBook(S)->setEventListener(&r.events);
}

// Returns true when A, B and C agree throughout.
bool runSeed(uint64_t seed, size_t nOps, size_t forcedCut) {
    Generator gen(seed);
    const std::vector<Op> flow = gen.flow(nOps);
    const size_t cut = forcedCut != 0 ? forcedCut
                                      : 1 + std::mt19937_64(seed ^ 0x9e3779b97f4a7c15ULL)() % (nOps - 1);

    const std::string base = uniqueTempPath("ckpt_equiv_" + std::to_string(seed));
    const std::string live = base + ".wal", plain = base + ".plain", ckpt = base + ".ckpt";
    for (const auto& p : {live, plain, ckpt}) fs::remove(p);

    Replica a;
    setup(a.engine);
    assert(a.engine.enableJournal(live, Journal::SyncPolicy::GroupCommit));
    a.engine.start();
    a.engine.getOrderBook(S)->setEventListener(&a.events);
    for (size_t i = 0; i < cut; ++i) apply(a.engine, flow[i]);
    // flush(), not commitJournal(): on the io_uring path a commit only
    // submits the write chain, and a copy taken then misses the tail.
    a.engine.getJournal()->flush();
    fs::copy_file(live, plain);
    a.engine.checkpoint();
    a.engine.getJournal()->flush();
    fs::copy_file(live, ckpt);

    Replica b, c;
    boot(b, ckpt);
    boot(c, plain);
    bool ok = b.engine.restoreRefusals() == 0 && b.engine.replayRefusals() == 0 &&
              c.engine.restoreRefusals() == 0 && c.engine.replayRefusals() == 0;
    if (!ok) std::printf("  seed %llu: refusals on restart (cut %zu)\n",
                         static_cast<unsigned long long>(seed), cut);

    for (size_t i = cut; ok && i <= nOps; ++i) {
        a.events.clear(); b.events.clear(); c.events.clear();
        if (i > cut) {   // i == cut compares the books as restarted
            const Op& op = flow[i - 1];
            apply(a.engine, op); apply(b.engine, op); apply(c.engine, op);
        }
        const BookState sa = capture(a.engine), sb = capture(b.engine), sc = capture(c.engine);
        const bool sameB = sa == sb && a.events == b.events;
        const bool sameC = sa == sc && a.events == c.events;
        if (sameB && sameC) continue;
        ok = false;
        std::printf("  seed %llu: %s diverged at op %zu (%s #%llu), cut %zu\n",
                    static_cast<unsigned long long>(seed),
                    !sameB ? (!sameC ? "B and C" : "B (checkpoint)") : "C (plain journal)", i,
                    i > cut ? kindName(flow[i - 1].kind) : "restart",
                    static_cast<unsigned long long>(i > cut ? flow[i - 1].id : 0), cut);
        printBook("A", sa); printEvents("A", a.events);
        if (!sameB) { printBook("B", sb); printEvents("B", b.events); }
        if (!sameC) { printBook("C", sc); printEvents("C", c.events); }
    }
    a.engine.stop(); b.engine.stop(); c.engine.stop();
    for (const auto& p : {live, plain, ckpt}) fs::remove(p);
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    const uint64_t first = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1;
    const uint64_t seeds = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 60;
    const size_t ops = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 400;
    const size_t cut = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 0;   // to bisect a seed
    size_t failed = 0;
    for (const double band : {kBreakerOff, kTightBand}) {
        gBreakerThreshold = band;
        for (uint64_t seed = first; seed < first + seeds; ++seed) {
            if (!runSeed(seed, ops, cut)) {
                std::printf("  (breaker band %g)\n", band);
                ++failed;
            }
        }
    }
    if (failed != 0) {
        std::printf("CheckpointEquivalenceTest FAILED: %zu of %llu runs diverged\n", failed,
                    static_cast<unsigned long long>(2 * seeds));
        return 1;
    }
    std::printf("CheckpointEquivalenceTest passed: %llu seeds x %zu ops, breaker off and tight\n",
                static_cast<unsigned long long>(seeds), ops);
    return 0;
}
