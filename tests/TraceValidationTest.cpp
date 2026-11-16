// TraceValidationTest — what OrderBook does, checked to be a behaviour of
// spec/MatchingEngine.tla (FORMAL-4, first cut).
//
// Seeded flows of Limit orders, Cancel, Modify (reduce) and Replace (price,
// same total) over two participants, prices 100 and 200, default STP, run on
// a real OrderBook. Each request becomes the spec action it corresponds to,
// and each arrival (a submit, or a replace that re-aggresses) its fills in
// print order and how it ended (rested, or STP-cancelled). The traces go to a
// TLA+ module, and TLC runs spec/TraceMatchingEngine.tla over them: an event
// the spec cannot take stops TLC with "Deadlock reached".
//
// Needs java and spec/tla2tools.jar (spec/check.sh fetches it); without them
// the cases SKIP, so in the C++ lanes this is a no-op and in the TLA+
// workflow, which has both, it runs.

#include "EventListener.h"
#include "OrderBook.h"
#include "TempPath.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <vector>

using namespace OrderMatcher;
namespace fs = std::filesystem;

namespace {

constexpr Price kPrices[] = {100, 200};
constexpr Quantity kMaxQty = 3;
constexpr int kMaxOrders = 6;
constexpr int kMaxReplaces = 2;
constexpr int kOpsPerTrace = 16;
constexpr int kTraces = 300;

struct Ev {
    std::string op;
    OrderId o = 0;
    ParticipantId pt = 0;
    Side side = Side::Buy;
    Price price = 0;
    Quantity qty = 0;
    OrderId agg = 0;
    OrderId rest = 0;
};

struct Tap : EventListener {
    std::vector<Trade> trades;
    std::set<OrderId> stp;
    void onTrade(const Trade& t) override { trades.push_back(t); }
    void onOrderUpdate(const OrderUpdate& u) override {
        if (u.status == OrderStatus::CancelledBySTP) stp.insert(u.orderId);
    }
};

// An arrival of `id`: its fills, in print order, then how it ended.
void recordArrival(const OrderBook& book, Tap& tap, OrderId id, Side side, std::vector<Ev>& tr) {
    for (const Trade& t : tap.trades) {
        Ev e{"fill"};
        e.agg = id;
        e.rest = side == Side::Buy ? t.sellOrderId : t.buyOrderId;
        e.price = t.price;
        e.qty = t.quantity;
        tr.push_back(e);
    }
    tap.trades.clear();
    if (tap.stp.count(id)) {
        tr.push_back(Ev{"stp", id});
    } else if (const Order* o = book.getOrder(id); o && o->remainingQty > 0) {
        tr.push_back(Ev{"rest", id});
    }
    tap.stp.clear();
}

std::vector<OrderId> liveOrders(const OrderBook& book, int submitted) {
    std::vector<OrderId> live;
    for (OrderId id = 1; id <= static_cast<OrderId>(submitted); ++id) {
        if (const Order* o = book.getOrder(id); o && o->remainingQty > 0) live.push_back(id);
    }
    return live;
}

std::vector<Ev> runTrace(uint64_t seed) {
    std::mt19937_64 rng(seed);
    OrderBook book(0);
    Tap tap;
    book.setEventListener(&tap);
    book.setCircuitBreakerThreshold(1e9);  // the spec has no breaker
    std::vector<Ev> tr;
    int submitted = 0, replaced = 0;
    for (int k = 0; k < kOpsPerTrace; ++k) {
        const std::vector<OrderId> live = liveOrders(book, submitted);
        const unsigned pick = static_cast<unsigned>(rng() % 10);
        if (live.empty() || pick < 5) {
            if (submitted == kMaxOrders) continue;
            Ev e{"submit"};
            e.o = static_cast<OrderId>(++submitted);
            e.pt = static_cast<ParticipantId>(1 + rng() % 2);
            e.side = rng() % 2 ? Side::Buy : Side::Sell;
            e.price = kPrices[rng() % 2];
            e.qty = 1 + rng() % kMaxQty;
            const auto res = book.addOrder(e.o, e.pt, e.side, e.price, e.qty, OrderType::Limit);
            EXPECT_TRUE(std::holds_alternative<OrderId>(res)) << "seed " << seed;
            tr.push_back(e);
            recordArrival(book, tap, e.o, e.side, tr);
            continue;
        }
        const OrderId id = live[rng() % live.size()];
        const Order* o = book.getOrder(id);
        if (pick < 6) {
            book.cancelOrder(id);
            tr.push_back(Ev{"cancel", id});
        } else if (pick < 8) {
            if (o->remainingQty < 2) continue;
            Ev e{"modify", id};
            e.qty = 1 + rng() % (o->remainingQty - 1);
            EXPECT_TRUE(book.modifyOrder(id, e.qty)) << "seed " << seed;
            tr.push_back(e);
        } else {
            if (replaced == kMaxReplaces) continue;
            ++replaced;
            Ev e{"replace", id};
            e.price = kPrices[rng() % 2];
            const Side side = o->side;
            EXPECT_TRUE(book.cancelReplace(id, e.price, o->cumQty + o->remainingQty)) << "seed " << seed;
            tr.push_back(e);
            recordArrival(book, tap, id, side, tr);
        }
    }
    return tr;
}

std::string tlaRecord(const Ev& e) {
    std::ostringstream s;
    s << "[op |-> \"" << e.op << "\", o |-> " << e.o << ", pt |-> " << e.pt
      << ", side |-> \"" << (e.side == Side::Buy ? "Buy" : "Sell") << "\", price |-> " << e.price
      << ", qty |-> " << e.qty << ", agg |-> " << e.agg << ", rest |-> " << e.rest << "]";
    return s.str();
}

struct TlcRun { int exitCode = -1; std::string log; };

// Writes the traces next to copies of the two specs and runs TLC on them.
TlcRun checkTraces(const std::vector<std::vector<Ev>>& traces, const std::string& tag) {
    const fs::path dir = uniqueTempPath("trace_" + tag);
    fs::remove_all(dir);
    fs::create_directories(dir);
    for (const char* f : {"MatchingEngine.tla", "TraceMatchingEngine.tla"}) {
        fs::copy_file(fs::path(SPEC_DIR) / f, dir / f);
    }
    {
        std::ofstream m(dir / "TraceData.tla");
        m << "---- MODULE TraceData ----\nEXTENDS Sequences\nTraces == <<\n";
        for (size_t k = 0; k < traces.size(); ++k) {
            m << "  <<";
            for (size_t j = 0; j < traces[k].size(); ++j) m << (j ? ",\n    " : "") << tlaRecord(traces[k][j]);
            m << ">>" << (k + 1 < traces.size() ? ",\n" : "\n");
        }
        m << ">>\n====\n";
        std::ofstream c(dir / "Trace.cfg");
        c << "INIT TInit\nNEXT TNext\nCONSTANTS\n  MaxOrders = " << kMaxOrders
          << "\n  MaxReplaces = " << kMaxReplaces << "\n  Participants = {1, 2}\n"
          << "  Prices = {100, 200}\n  MaxQty = " << kMaxQty << "\n  BUG = \"none\"\n"
          << "INVARIANTS\n  NoNegativeQuantity\n  NoEmptyRestingOrder\n  FIFO_Preservation\n"
          << "  MatchingConservation\n  NoCrossedBookAfterStep\n  NoSelfTrade\n";
    }
    const std::string cmd = "cd '" + dir.string() + "' && java -XX:+UseParallelGC -cp '" +
                            (fs::path(SPEC_DIR) / "tla2tools.jar").string() +
                            "' tlc2.TLC -workers 1 -metadir states -config Trace.cfg "
                            "TraceMatchingEngine > tlc.log 2>&1";
    const int status = std::system(cmd.c_str());
    TlcRun run;
    run.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    std::ifstream in(dir / "tlc.log");
    std::stringstream ss;
    ss << in.rdbuf();
    run.log = ss.str();
    fs::remove_all(dir);
    return run;
}

bool tlcAvailable() {
    return fs::exists(fs::path(SPEC_DIR) / "tla2tools.jar") &&
           std::system("java -version > /dev/null 2>&1") == 0;
}

std::vector<std::vector<Ev>> allTraces() {
    std::vector<std::vector<Ev>> traces;
    for (int k = 0; k < kTraces; ++k) traces.push_back(runTrace(1000 + static_cast<uint64_t>(k)));
    return traces;
}

}  // namespace

TEST(TraceValidation, EveryEngineTraceIsABehaviourOfTheSpec) {
    if (!tlcAvailable()) GTEST_SKIP() << "needs java and spec/tla2tools.jar (spec/check.sh fetches it)";  // skip-ok: TLC is not installed with the build; tla.yml fails if this skips
    const auto traces = allTraces();
    // The generator must reach what the trace check is for.
    int fills = 0, stp = 0, replaces = 0, modifies = 0, cancels = 0, multiFillSweeps = 0;
    for (const auto& tr : traces) {
        for (size_t j = 0; j < tr.size(); ++j) {
            const std::string& op = tr[j].op;
            fills += op == "fill";
            stp += op == "stp";
            replaces += op == "replace";
            modifies += op == "modify";
            cancels += op == "cancel";
            multiFillSweeps += op == "fill" && j > 0 && tr[j - 1].op == "fill";
        }
    }
    std::cout << "fills " << fills << " stp " << stp << " replaces " << replaces << " modifies "
              << modifies << " cancels " << cancels << " multi-fill sweeps " << multiFillSweeps << "\n";
    EXPECT_GT(fills, kTraces);
    EXPECT_GT(stp, kTraces / 20);
    EXPECT_GT(replaces, kTraces / 2);
    EXPECT_GT(modifies, kTraces / 4);
    EXPECT_GT(cancels, kTraces / 2);
    EXPECT_GT(multiFillSweeps, kTraces / 20);

    const TlcRun run = checkTraces(traces, "all");
    EXPECT_EQ(run.exitCode, 0) << run.log;
    EXPECT_NE(run.log.find("No error has been found"), std::string::npos) << run.log;
}

// Non-vacuity: two fills of one sweep against different resting orders,
// reported in the other order, are not a behaviour of the spec.
TEST(TraceValidation, AReorderedSweepIsRejected) {
    if (!tlcAvailable()) GTEST_SKIP() << "needs java and spec/tla2tools.jar (spec/check.sh fetches it)";  // skip-ok: TLC is not installed with the build; tla.yml fails if this skips
    for (auto tr : allTraces()) {
        for (size_t j = 1; j < tr.size(); ++j) {
            if (tr[j - 1].op != "fill" || tr[j].op != "fill" || tr[j - 1].rest == tr[j].rest) continue;
            std::swap(tr[j - 1], tr[j]);
            const TlcRun run = checkTraces({tr}, "swapped");
            EXPECT_EQ(run.exitCode, 11) << run.log;
            EXPECT_NE(run.log.find("Deadlock reached"), std::string::npos) << run.log;
            return;
        }
    }
    FAIL() << "no trace has a sweep across two resting orders";
}
