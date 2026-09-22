// ─────────────────────────────────────────────────────────────────────────────
//  DIFFERENTIAL FUZZER: OrderBook/MatchingEngine vs an independent reference
//
//  Every other test in this repository was written by whoever wrote the code
//  under test, which means it can only check the behaviour its author already
//  had in mind. This one compares the engine against RefMatcher.h — a matcher
//  written from the DEFINITION of price-time FIFO rather than from
//  src/OrderBook.cpp — on randomly generated order flow, and reports every
//  disagreement.
//
//  WHAT IS COMPARED, AFTER EVERY SINGLE OPERATION:
//    1. the trades the operation produced, in order (ids, price, quantity);
//    2. the accept/reject decision for an add;
//    3. the full L2 ladder on both sides — every price, every total quantity,
//       every order count — plus the engine's own level counts;
//    4. every resting order by id: side, price, remaining quantity;
//    5. per-level FIFO queue order, taken from the engine's public
//       BookVisible feed, so queue position is checked and not inferred.
//
//  Comparing only fills is how a resting-state divergence stays hidden until
//  the repro is forty thousand orders long.
//
//  SCOPE: Limit, Market, IOC, FOK. Deliberately NOT icebergs, pro-rata, stops,
//  pegs, auctions or self-trade prevention — see the header of RefMatcher.h.
//
//  USAGE
//    DifferentialFuzzTest                      fixed seed, small run (what CI runs)
//    DifferentialFuzzTest --seeds N --ops M    soak
//    DifferentialFuzzTest --seed S             one specific seed
//    DifferentialFuzzTest --replay FILE        replay a recorded flow
//    DifferentialFuzzTest --corpus DIR         replay every *.flow in DIR first
//    DifferentialFuzzTest --stats              print measured generator bias
//
//  On a divergence the flow is delta-debugged to a minimal repro, printed in
//  the same text format --replay accepts, and the process exits non-zero.
// ─────────────────────────────────────────────────────────────────────────────

#include "DifferentialFuzzFlow.h"  // Op, Stats, Generator — what flow gets fed in
#include "MatchingEngine.h"
#include "OrderBook.h"
#include "RefMatcher.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace OrderMatcher;
using namespace DiffFuzz;
using RefMatch::RefBook;
using RefMatch::RefLevel;
using RefMatch::RefTrade;

namespace {

// ─── Engine tap ──────────────────────────────────────────────────────────────
// One listener per book, so order ids that collide across symbols stay apart
// (neither OrderUpdate nor BookVisibleUpdate carries a symbol id).
//
// The FIFO queue below is rebuilt from the engine's OWN public order-level
// feed: Rest means "this order is now at the BACK of the queue at this price".
// That is the engine stating its queue position, rather than this test peeking
// at engine internals or assuming a timestamp ordering.

struct EngineTap : EventListener {
    std::vector<RefTrade> trades;                          // cleared per operation
    std::map<std::pair<int, Price>, std::vector<OrderId>> queues;  // (side, price) -> FIFO

    void onTrade(const Trade& t) override {
        trades.push_back({t.buyOrderId, t.sellOrderId, t.price, t.quantity});
    }

    void onOrderUpdate(const OrderUpdate& u) override {
        // Filled / Cancelled / CancelledBySTP: that order has left the book.
        //
        // Rejected is deliberately NOT in this list even though it is terminal.
        // A rejected submission never entered the book, so it removes nothing —
        // and because order ids are reused across symbols and a same-symbol
        // duplicate is rejected BY id, treating a rejection as a removal makes
        // a refused duplicate evict the live order it collided with.
        if (u.status == OrderStatus::Rejected) return;
        if (isTerminalStatus(u.status)) erase(u.orderId);
    }

    void onBookVisible(const BookVisibleUpdate& u) override {
        const int side = (u.side == Side::Buy) ? 0 : 1;
        switch (u.action) {
            case BookVisibleUpdate::Action::Rest:
                erase(u.orderId);  // a re-rest loses its old position
                queues[{side, u.price}].push_back(u.orderId);
                break;
            case BookVisibleUpdate::Action::Remove:
                erase(u.orderId);
                break;
            case BookVisibleUpdate::Action::Reduce:
                break;  // size shrank; queue position is unchanged
        }
    }

    void erase(OrderId id) {
        for (auto it = queues.begin(); it != queues.end();) {
            auto& q = it->second;
            q.erase(std::remove(q.begin(), q.end(), id), q.end());
            it = q.empty() ? queues.erase(it) : std::next(it);
        }
    }

    std::vector<OrderId> queueAt(Side side, Price price) const {
        auto it = queues.find({side == Side::Buy ? 0 : 1, price});
        return it == queues.end() ? std::vector<OrderId>{} : it->second;
    }
};

// ─── Comparison ──────────────────────────────────────────────────────────────

struct Divergence {
    size_t opIndex = 0;
    std::string op;
    std::string what;
    std::string engine;
    std::string reference;
};

std::string renderLadder(const std::vector<RefLevel>& lv) {
    std::string s;
    for (const RefLevel& l : lv) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%lld:qty=%llu,n=%u[", (long long)l.price,
                      (unsigned long long)l.totalQty, l.orderCount);
        s += buf;
        for (size_t i = 0; i < l.queue.size(); i++) {
            if (i) s += ',';
            s += std::to_string(l.queue[i]);
        }
        s += "] ";
    }
    return s.empty() ? "<empty>" : s;
}

std::string renderTrades(const std::vector<RefTrade>& ts) {
    if (ts.empty()) return "<none>";
    std::string s;
    for (const RefTrade& t : ts) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "(buy=%llu sell=%llu px=%lld q=%llu) ",
                      (unsigned long long)t.buyId, (unsigned long long)t.sellId,
                      (long long)t.price, (unsigned long long)t.qty);
        s += buf;
    }
    return s;
}

// A whole engine + reference pair under test.
class Harness {
public:
    Harness() {
        for (size_t i = 0; i < kNumSymbols; i++) {
            engine_.addSymbol(kSymbols[i]);
            OrderBook* book = engine_.getOrderBook(kSymbols[i]);
            // The engine defaults to a 5% volatility circuit breaker. Leaving it
            // on would reject orders for a reason the reference does not model —
            // and a run where EVERY order comes back rejected looks plausible
            // until you read the reject reason. Out of scope, so: off.
            book->setCircuitBreakerThreshold(1e12);
            book->setEventListener(&taps_[i]);
        }
        engine_.start();  // must follow addSymbol: getOrderBook() is null otherwise
    }

    ~Harness() { engine_.stop(); }

    // Returns the first divergence this operation produced, if any.
    std::optional<Divergence> step(const Op& op, size_t index) {
        const size_t s = symIndex(op.sym);
        EngineTap& tap = taps_[s];
        RefBook& ref = ref_[s];
        // Clear EVERY symbol's trade buffer, not just the one being addressed.
        // An operation on one book that emits a trade on another is the exact
        // shape of the cancel-routing defect this engine has had before, and a
        // trade that moves no quantity — which this engine has been observed to
        // emit — changes no ladder, so comparing book state alone would not see
        // it. The only way to catch it is to require silence elsewhere.
        for (EngineTap& t : taps_) t.trades.clear();

        std::vector<RefTrade> refTrades;
        auto fail = [&](std::string what, std::string eng, std::string r) {
            return Divergence{index, formatOp(op), std::move(what), std::move(eng), std::move(r)};
        };

        if (op.kind == Op::Kind::Add) {
            const auto refOutcome = ref.add(op.id, op.owner, op.side, op.price, op.qty, op.type,
                                            refTrades);
            const SubmitResult res = engine_.submitOrder(op.sym, op.id, op.owner, op.side,
                                                         op.price, op.qty, op.type);
            // Anything the reference cannot express is a broken harness, not a
            // finding. Stop rather than report a divergence that is our fault.
            if (!res.isAccepted() && res.rejectReason != RejectReason::FOKInsufficientLiquidity &&
                res.rejectReason != RejectReason::DuplicateOrderId) {
                std::fprintf(stderr,
                             "\nHARNESS ERROR: engine rejected with reason %d, which this test "
                             "does not model.\nOp: %s\nIf every order rejects this way, check that "
                             "the harness TU was NOT compiled with -DNDEBUG while libOrderMatcher.a "
                             "was built with -UNDEBUG.\n",
                             (int)res.rejectReason, formatOp(op).c_str());
                std::exit(2);
            }
            const bool refAccepted = (refOutcome == RefMatch::AddOutcome::Accepted);
            if (refAccepted != res.isAccepted()) {
                return fail("add accept/reject decision",
                            res.isAccepted() ? "accepted"
                                             : "rejected(" + std::to_string((int)res.rejectReason) + ")",
                            refAccepted ? "accepted" : "rejected");
            }
        } else if (op.kind == Op::Kind::Cancel) {
            ref.cancel(op.id, op.requester);
            engine_.submitCancel(op.sym, op.id, op.requester);
        } else {
            ref.modify(op.id, op.qty, op.requester);
            engine_.modifyOrder(op.sym, op.id, op.qty, op.requester);
        }

        if (tap.trades != refTrades) {
            return fail("trades produced by this operation", renderTrades(tap.trades),
                        renderTrades(refTrades));
        }
        for (size_t i = 0; i < kNumSymbols; i++) {
            if (i == s || taps_[i].trades.empty()) continue;
            return fail("trades leaked onto symbol " + std::to_string(kSymbols[i]) +
                            ", which this operation did not address",
                        renderTrades(taps_[i].trades), "<none>");
        }

        // Every symbol is checked, not just the one the operation named: an
        // operation leaking into another book is precisely the known defect
        // class, and it is invisible if you only look where you aimed.
        for (size_t i = 0; i < kNumSymbols; i++) {
            if (auto d = compareBook(i, op, index)) return d;
        }
        return std::nullopt;
    }

private:
    MatchingEngine engine_;
    EngineTap taps_[kNumSymbols];
    RefBook ref_[kNumSymbols];

    static size_t symIndex(SymbolId s) {
        for (size_t i = 0; i < kNumSymbols; i++)
            if (kSymbols[i] == s) return i;
        return 0;
    }

    std::optional<Divergence> compareBook(size_t i, const Op& op, size_t index) {
        const SymbolId sym = kSymbols[i];
        OrderBook* book = engine_.getOrderBook(sym);
        EngineTap& tap = taps_[i];
        RefBook& ref = ref_[i];

        auto fail = [&](std::string what, std::string eng, std::string r) {
            return Divergence{index, formatOp(op),
                              std::move(what) + " (symbol " + std::to_string(sym) + ")",
                              std::move(eng), std::move(r)};
        };

        const MarketDataSnapshot snap = engine_.getSnapshot(sym, MarketDataSnapshot::MAX_DEPTH);

        for (Side side : {Side::Buy, Side::Sell}) {
            const std::vector<RefLevel> want = ref.ladder(side);
            const PriceLevel* lv = (side == Side::Buy) ? snap.bids : snap.asks;
            const size_t n = (side == Side::Buy) ? snap.bidCount : snap.askCount;
            const size_t engineLevels =
                (side == Side::Buy) ? book->getBidLevelsCount() : book->getAskLevelsCount();

            if (want.size() > MarketDataSnapshot::MAX_DEPTH) {
                std::fprintf(stderr, "HARNESS ERROR: %zu levels exceeds snapshot depth %zu\n",
                             want.size(), MarketDataSnapshot::MAX_DEPTH);
                std::exit(2);
            }
            // Build the engine's view in the same shape, queue order included.
            std::vector<RefLevel> got;
            got.reserve(n);
            for (size_t k = 0; k < n; k++)
                got.push_back({lv[k].price, lv[k].totalQuantity, lv[k].orderCount,
                               tap.queueAt(side, lv[k].price)});

            const char* sideName = (side == Side::Buy) ? "bid ladder" : "ask ladder";
            if (engineLevels != want.size() || got.size() != want.size())
                return fail(sideName, renderLadder(got) + " [levelCount=" +
                                          std::to_string(engineLevels) + "]",
                            renderLadder(want));
            for (size_t k = 0; k < want.size(); k++) {
                if (got[k].price != want[k].price || got[k].totalQty != want[k].totalQty ||
                    got[k].orderCount != want[k].orderCount)
                    return fail(sideName, renderLadder(got), renderLadder(want));
                if (got[k].queue != want[k].queue)
                    return fail(side == Side::Buy ? "bid FIFO queue order"
                                                  : "ask FIFO queue order",
                                renderLadder(got), renderLadder(want));
            }
        }

        // Order-by-order comparison, independent of the aggregated ladder: this
        // is what catches the wrong order being reduced or removed at a level
        // whose totals happen to still add up.
        std::string engineOrders, refOrders;
        std::map<OrderId, std::string> engineMap;
        bool outOfBook = false;
        book->forEachOrder([&](const Order& o) {
            if (!o.inBook) outOfBook = true;
            char buf[96];
            std::snprintf(buf, sizeof buf, "%llu:%c@%lld x%llu", (unsigned long long)o.id,
                          o.side == Side::Buy ? 'B' : 'S', (long long)o.price,
                          (unsigned long long)o.remainingQty);
            engineMap[o.id] = buf;
        });
        if (outOfBook) {
            std::fprintf(stderr, "HARNESS ERROR: engine reported a resting order with inBook=false;"
                                 " the harness cannot interpret that state.\n");
            std::exit(2);
        }
        for (const auto& [id, text] : engineMap) { engineOrders += text; engineOrders += ' '; }
        std::map<OrderId, std::string> refMap;
        const std::map<OrderId, RefMatch::RefOrder> refResting = ref.resting();
        for (Side side : {Side::Buy, Side::Sell})
            for (const RefLevel& l : ref.ladder(side))
                for (const OrderId id : l.queue) {
                    char buf[96];
                    std::snprintf(buf, sizeof buf, "%llu:%c@%lld x%llu", (unsigned long long)id,
                                  side == Side::Buy ? 'B' : 'S', (long long)l.price,
                                  (unsigned long long)refResting.at(id).qty);
                    refMap[id] = buf;
                }
        for (const auto& [id, text] : refMap) { refOrders += text; refOrders += ' '; }
        if (engineOrders != refOrders)
            return fail("resting orders", engineOrders.empty() ? "<none>" : engineOrders,
                        refOrders.empty() ? "<none>" : refOrders);

        std::string err;
        if (!book->validateIntegrity(&err))
            return fail("engine internal integrity check", err, "consistent");

        return std::nullopt;
    }

};

std::optional<Divergence> replay(const std::vector<Op>& flow) {
    Harness h;
    for (size_t i = 0; i < flow.size(); i++)
        if (auto d = h.step(flow[i], i)) return d;
    return std::nullopt;
}

// ─── Shrinking ───────────────────────────────────────────────────────────────
// Delta debugging on the operation list. Without this, every finding costs a
// day of manual bisection and the tool stops being run.

std::vector<Op> shrink(std::vector<Op> flow) {
    auto stillFails = [](const std::vector<Op>& f) { return replay(f).has_value(); };

    // Pass 1: drop contiguous chunks, halving the chunk size each round.
    for (size_t chunk = std::max<size_t>(flow.size() / 2, 1);; chunk /= 2) {
        bool progress = true;
        while (progress) {
            progress = false;
            for (size_t i = 0; i + chunk <= flow.size();) {
                std::vector<Op> candidate;
                candidate.reserve(flow.size() - chunk);
                candidate.insert(candidate.end(), flow.begin(), flow.begin() + i);
                candidate.insert(candidate.end(), flow.begin() + i + chunk, flow.end());
                if (stillFails(candidate)) {
                    flow = std::move(candidate);
                    progress = true;
                } else {
                    i += chunk;
                }
            }
        }
        if (chunk == 1) break;
    }

    // Pass 2: shrink quantities, so the repro reads in numbers a human keeps in
    // their head rather than whatever the generator happened to roll.
    for (Op& op : flow) {
        if (op.kind == Op::Kind::Cancel) continue;
        while (op.qty > 1) {
            const Quantity saved = op.qty;
            op.qty = saved / 2;
            if (!stillFails(flow)) { op.qty = saved; break; }
        }
    }
    return flow;
}

void reportDivergence(const Divergence& d, const std::vector<Op>& minimal, const char* origin) {
    std::printf("\n");
    std::printf("════════════════════════════════════════════════════════════════════\n");
    std::printf(" DIVERGENCE  (%s)\n", origin);
    std::printf("════════════════════════════════════════════════════════════════════\n");
    std::printf(" operation #%zu: %s\n", d.opIndex, d.op.c_str());
    std::printf(" disagreement: %s\n\n", d.what.c_str());
    std::printf("   engine    : %s\n", d.engine.c_str());
    std::printf("   reference : %s\n\n", d.reference.c_str());
    std::printf(" minimal reproducing flow (%zu ops) — save this under tests/fuzz_corpus/\n",
                minimal.size());
    std::printf("────────────────────────────────────────────────────────────────────\n");
    std::fputs(formatFlow(minimal).c_str(), stdout);
    std::printf("────────────────────────────────────────────────────────────────────\n");
    std::printf(" replay with: DifferentialFuzzTest --replay <file>\n\n");
}

// ─── Corpus ──────────────────────────────────────────────────────────────────
// Every divergence ever found becomes a file here and is replayed on every run.

std::vector<Op> loadFlow(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open flow file: %s\n", path.c_str());
        std::exit(2);
    }
    std::vector<Op> flow;
    std::string line;
    while (std::getline(in, line)) {
        Op op;
        if (parseOp(line, op)) flow.push_back(op);
    }
    return flow;
}

// Naming convention, so one directory carries both kinds of case:
//
//   <name>.flow        a divergence that has been FIXED. It must stay fixed;
//                      if it diverges again the run fails.
//   <name>.known.flow  a divergence that is real and NOT yet fixed. It is
//                      replayed and reported on every run, but does not fail
//                      the build — otherwise the only way to land this test
//                      would be to delete the finding. If one of these stops
//                      diverging, that is reported too: the bug was fixed and
//                      the case should be promoted by dropping ".known".
int runCorpus(const std::string& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        std::printf("corpus: %s not present, nothing to replay\n", dir.c_str());
        return 0;
    }
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        if (entry.path().extension() == ".flow") files.push_back(entry.path());
    std::sort(files.begin(), files.end());

    int failures = 0;
    for (const auto& f : files) {
        const std::string name = f.filename().string();
        const bool known = name.find(".known.") != std::string::npos;
        const std::vector<Op> flow = loadFlow(f.string());
        const auto d = replay(flow);

        if (d && known) {
            std::printf("corpus: %-52s KNOWN DIVERGENCE, still open (%zu ops)\n", name.c_str(),
                        flow.size());
            std::printf("          op #%zu %s | %s\n", d->opIndex, d->op.c_str(), d->what.c_str());
            std::printf("          engine=%s  reference=%s\n", d->engine.c_str(),
                        d->reference.c_str());
        } else if (d) {
            reportDivergence(*d, flow, ("corpus regression: " + name).c_str());
            failures++;
        } else if (known) {
            std::printf("corpus: %-52s NO LONGER DIVERGES — the engine defect appears "
                        "fixed; rename this case to drop \".known\".\n",
                        name.c_str());
        } else {
            std::printf("corpus: %-52s ok (%zu ops)\n", name.c_str(), flow.size());
        }
    }
    if (files.empty()) std::printf("corpus: %s contains no .flow cases\n", dir.c_str());
    return failures;
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    // CI defaults: fixed seed, small and fast. The soak is a flag, not
    // something every push pays for.
    // Few seeds, long flows: a book only develops queues worth testing after
    // roughly a thousand operations, so many short seeds would run the same
    // shallow-book region over and over.
    uint64_t baseSeed = 0x5EEDULL;
    size_t seeds = 10;
    size_t opsPerSeed = 1200;
    std::string corpusDir;
    std::string replayFile;
    bool showStats = false;

    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--seed") { baseSeed = std::stoull(next()); seeds = 1; }
        else if (a == "--seeds") seeds = std::stoull(next());
        else if (a == "--ops") opsPerSeed = std::stoull(next());
        else if (a == "--corpus") corpusDir = next();
        else if (a == "--replay") replayFile = next();
        else if (a == "--stats") showStats = true;
        else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); return 2; }
    }

    if (!replayFile.empty()) {
        const std::vector<Op> flow = loadFlow(replayFile);
        if (auto d = replay(flow)) {
            // Report the values belonging to the repro being printed, not the
            // ones from the unshrunk flow — a reader comparing the two would
            // otherwise be chasing numbers the listed operations cannot produce.
            const std::vector<Op> minimal = shrink(flow);
            const auto minimalDivergence = replay(minimal);
            reportDivergence(minimalDivergence ? *minimalDivergence : *d, minimal,
                             replayFile.c_str());
            return 1;
        }
        std::printf("replay %s: no divergence (%zu ops)\n", replayFile.c_str(), flow.size());
        return 0;
    }

    if (!corpusDir.empty() && runCorpus(corpusDir) != 0) return 1;

    Stats total;
    std::printf("differential fuzz: %zu seeds x %zu ops (base seed 0x%llx)\n", seeds, opsPerSeed,
                (unsigned long long)baseSeed);
    for (size_t s = 0; s < seeds; s++) {
        const uint64_t seed = baseSeed + s;
        Stats stats;
        const std::vector<Op> flow = Generator(seed).generate(opsPerSeed, stats);
        total.merge(stats);

        if (auto d = replay(flow)) {
            std::printf("\nseed 0x%llx diverged at op #%zu; shrinking %zu ops...\n",
                        (unsigned long long)seed, d->opIndex, flow.size());
            const std::vector<Op> minimal = shrink(flow);
            // Re-run the minimal flow so the printed engine/reference values
            // belong to the repro being shown, not to the original flow.
            const auto minimalDivergence = replay(minimal);
            reportDivergence(minimalDivergence ? *minimalDivergence : *d, minimal,
                             ("seed 0x" + std::to_string(seed)).c_str());
            return 1;
        }
        if (seeds > 20 && (s + 1) % (seeds / 20) == 0)
            std::printf("  %zu/%zu seeds clean (%llu ops)\n", s + 1, seeds,
                        (unsigned long long)total.ops);
    }

    std::printf("\nno divergence: %llu operations over %zu seeds, %llu trades executed\n",
                (unsigned long long)total.ops, seeds, (unsigned long long)total.trades);
    if (showStats) total.print();
    return 0;
}
