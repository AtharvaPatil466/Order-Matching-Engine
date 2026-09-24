#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  FLOW: what operations the differential fuzzer feeds both matchers.
//
//  A flow is a list of operations and nothing else. It is a pure value, so the
//  same flow always produces the same comparison — which is what lets a
//  failure be shrunk automatically and checked into tests/fuzz_corpus as a
//  permanent regression case.
//
//  The generator is the other half of this test's value. Uniform random order
//  flow explores the boring region of the state space: books two deep, no
//  queues, nothing ever at a boundary. Every bias here is deliberate and every
//  one of them is COUNTED in Stats, so the claims made about coverage are
//  measurements rather than intentions.
// ─────────────────────────────────────────────────────────────────────────────

#include "RefMatcher.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace DiffFuzz {

using OrderMatcher::kAnyParticipant;
using OrderMatcher::OrderId;
using OrderMatcher::OrderType;
using OrderMatcher::ParticipantId;
using OrderMatcher::Price;
using OrderMatcher::Quantity;
using OrderMatcher::Side;
using OrderMatcher::SymbolId;
using RefMatch::RefBook;
using RefMatch::RefTrade;


// ─── Flow representation ─────────────────────────────────────────────────────
// A flow is a list of operations. It is a pure value: replaying the same flow
// always produces the same comparison, which is what makes shrinking and the
// regression corpus work.

struct Op {
    enum class Kind : uint8_t { Add, Cancel, Modify };

    Kind kind{Kind::Add};
    SymbolId sym{0};
    OrderId id{0};
    ParticipantId owner{0};       // Add: who submits it
    Side side{Side::Buy};         // Add
    OrderType type{OrderType::Limit};  // Add
    Price price{0};               // Add
    Quantity qty{0};              // Add: size. Modify: the new (reduced) size
    ParticipantId requester{kAnyParticipant};  // Cancel/Modify
};

inline char typeChar(OrderType t) {
    switch (t) {
        case OrderType::Limit: return 'L';
        case OrderType::Market: return 'M';
        case OrderType::IOC: return 'I';
        case OrderType::FOK: return 'F';
        default: return '?';
    }
}

inline OrderType charType(char c) {
    switch (c) {
        case 'L': return OrderType::Limit;
        case 'M': return OrderType::Market;
        case 'I': return OrderType::IOC;
        case 'F': return OrderType::FOK;
        default: return OrderType::Limit;
    }
}

inline std::string formatOp(const Op& o) {
    char buf[192];
    auto who = [](ParticipantId p) {
        return p == kAnyParticipant ? std::string("*") : std::to_string(p);
    };
    switch (o.kind) {
        case Op::Kind::Add:
            std::snprintf(buf, sizeof buf, "A %u %llu %llu %c %c %lld %llu", o.sym,
                          (unsigned long long)o.id, (unsigned long long)o.owner,
                          o.side == Side::Buy ? 'B' : 'S', typeChar(o.type), (long long)o.price,
                          (unsigned long long)o.qty);
            return buf;
        case Op::Kind::Cancel:
            std::snprintf(buf, sizeof buf, "C %u %llu %s", o.sym, (unsigned long long)o.id,
                          who(o.requester).c_str());
            return buf;
        case Op::Kind::Modify:
            std::snprintf(buf, sizeof buf, "M %u %llu %s %llu", o.sym, (unsigned long long)o.id,
                          who(o.requester).c_str(), (unsigned long long)o.qty);
            return buf;
    }
    return "?";
}

inline bool parseOp(const std::string& line, Op& out) {
    std::istringstream in(line);
    std::string kind;
    if (!(in >> kind) || kind.empty() || kind[0] == '#') return false;
    auto who = [](const std::string& s) -> ParticipantId {
        return s == "*" ? kAnyParticipant : static_cast<ParticipantId>(std::stoull(s));
    };
    if (kind == "A") {
        std::string sideStr, typeStr;
        unsigned sym = 0;
        if (!(in >> sym >> out.id >> out.owner >> sideStr >> typeStr >> out.price >> out.qty))
            return false;
        out.kind = Op::Kind::Add;
        out.sym = static_cast<SymbolId>(sym);
        out.side = sideStr == "B" ? Side::Buy : Side::Sell;
        out.type = charType(typeStr[0]);
        return true;
    }
    if (kind == "C") {
        std::string req;
        unsigned sym = 0;
        if (!(in >> sym >> out.id >> req)) return false;
        out.kind = Op::Kind::Cancel;
        out.sym = static_cast<SymbolId>(sym);
        out.requester = who(req);
        return true;
    }
    if (kind == "M") {
        std::string req;
        unsigned sym = 0;
        if (!(in >> sym >> out.id >> req >> out.qty)) return false;
        out.kind = Op::Kind::Modify;
        out.sym = static_cast<SymbolId>(sym);
        out.requester = who(req);
        return true;
    }
    return false;
}

inline std::string formatFlow(const std::vector<Op>& flow) {
    std::string s;
    for (const Op& o : flow) {
        s += formatOp(o);
        s += '\n';
    }
    return s;
}
struct Stats {
    uint64_t ops = 0, adds = 0, cancels = 0, modifies = 0;
    uint64_t byType[4] = {0, 0, 0, 0};  // Limit, Market, IOC, FOK
    uint64_t aggressive = 0;            // adds that produced at least one trade
    uint64_t fullyFilled = 0, partiallyFilled = 0, restedUntouched = 0;
    uint64_t fokAccepted = 0, fokRejected = 0;
    uint64_t cancelHit = 0, cancelMiss = 0, cancelDenied = 0;
    uint64_t modifyOk = 0, modifyRejected = 0;
    uint64_t boundaryPriced = 0;  // add priced exactly at best bid / best ask / mid
    uint64_t trades = 0;
    uint64_t restingSum = 0, restingSamples = 0, restingMax = 0;
    uint64_t levelMax = 0;
    uint64_t dupIdOps = 0;      // ops whose id was live on 2+ symbols at the time
    uint64_t dupIdMaxShare = 0; // most symbols simultaneously holding one live id

    void merge(const Stats& o) {
        ops += o.ops; adds += o.adds; cancels += o.cancels; modifies += o.modifies;
        for (int i = 0; i < 4; i++) byType[i] += o.byType[i];
        aggressive += o.aggressive; fullyFilled += o.fullyFilled;
        partiallyFilled += o.partiallyFilled; restedUntouched += o.restedUntouched;
        fokAccepted += o.fokAccepted; fokRejected += o.fokRejected;
        cancelHit += o.cancelHit; cancelMiss += o.cancelMiss; cancelDenied += o.cancelDenied;
        modifyOk += o.modifyOk; modifyRejected += o.modifyRejected;
        boundaryPriced += o.boundaryPriced; trades += o.trades;
        restingSum += o.restingSum; restingSamples += o.restingSamples;
        restingMax = std::max(restingMax, o.restingMax);
        levelMax = std::max(levelMax, o.levelMax);
        dupIdOps += o.dupIdOps;
        dupIdMaxShare = std::max(dupIdMaxShare, o.dupIdMaxShare);
    }

    void print() const {
        auto pct = [&](uint64_t n) { return ops ? 100.0 * double(n) / double(ops) : 0.0; };
        auto apct = [&](uint64_t n) { return adds ? 100.0 * double(n) / double(adds) : 0.0; };
        std::printf("\n─── measured generator bias (%llu ops) ───\n", (unsigned long long)ops);
        std::printf("  add %.1f%%  cancel %.1f%%  modify %.1f%%   (add+cancel+modify = 100%%)\n",
                    pct(adds), pct(cancels), pct(modifies));
        std::printf("  add types: Limit %.1f%%  Market %.1f%%  IOC %.1f%%  FOK %.1f%%\n",
                    apct(byType[0]), apct(byType[1]), apct(byType[2]), apct(byType[3]));
        std::printf("  adds that traded: %.1f%%   fully filled %.1f%%  partial %.1f%%  rested clean %.1f%%\n",
                    apct(aggressive), apct(fullyFilled), apct(partiallyFilled),
                    apct(restedUntouched));
        std::printf("  adds priced exactly at a book boundary (best bid/ask/mid): %.1f%%\n",
                    apct(boundaryPriced));
        std::printf("  FOK: %llu filled whole, %llu killed\n", (unsigned long long)fokAccepted,
                    (unsigned long long)fokRejected);
        std::printf("  cancels: %llu hit a resting order, %llu missed, %llu denied (not owner)\n",
                    (unsigned long long)cancelHit, (unsigned long long)cancelMiss,
                    (unsigned long long)cancelDenied);
        std::printf("  modifies: %llu applied, %llu refused\n", (unsigned long long)modifyOk,
                    (unsigned long long)modifyRejected);
        std::printf("  book depth: mean %.1f resting orders/symbol, max %llu; max levels/side %llu\n",
                    restingSamples ? double(restingSum) / double(restingSamples) : 0.0,
                    (unsigned long long)restingMax, (unsigned long long)levelMax);
        std::printf("  duplicate ids ACROSS symbols: %.1f%% of ops used an id live on 2+ symbols; "
                    "max %llu symbols sharing one id\n",
                    pct(dupIdOps), (unsigned long long)dupIdMaxShare);
        std::printf("  trades executed: %llu\n", (unsigned long long)trades);
    }
};

// ─── Generator ───────────────────────────────────────────────────────────────

constexpr SymbolId kSymbols[] = {1, 2, 3};
constexpr size_t kNumSymbols = sizeof(kSymbols) / sizeof(kSymbols[0]);
constexpr OrderId kIdSpace = 96;          // small on purpose: ids collide across symbols

// Participants are PARTITIONED BY SIDE: 1,2 only ever buy; 3,4 only ever sell.
//
// This engine's self-trade prevention has no off switch — STPMode's default is
// DefaultCancelIncoming, so an order crossing its own participant's resting
// order is cancelled rather than filled. STP is out of scope for this run (it
// needs the specification being written in parallel), and the only way to keep
// it dormant is to make a self-cross impossible. Partitioning by side does
// that exactly, at the cost of the fuzzer being structurally unable to reach
// self-trade prevention or wash-trade detection at all. Said plainly in the
// report rather than quietly traded away here.
constexpr ParticipantId kBuySide[] = {1, 2};
constexpr ParticipantId kSellSide[] = {3, 4};
constexpr ParticipantId kParticipants = 4;
constexpr Price kBasePrice = 1'000'000;   // 100.0000
constexpr Price kTick = 100;              // 0.0100
// Prices are crushed into a handful of ticks ON PURPOSE. Spread the same order
// flow over thirty levels and every level holds one order, so queue position
// never decides anything and half of price-time priority goes untested. Nine
// possible prices against a 96-id pool is what produces queues deep enough for
// "who is in front" to be the whole answer.
constexpr int kGridLow = -2, kGridHigh = 2;  // 5 tightly clustered price levels
constexpr Price kPriceSpan = 4;           // hard bound: 9 distinct prices, <= MAX_DEPTH levels
constexpr size_t kDepthCap = 250;         // keep books deep but inside the order pool

class Generator {
public:
    explicit Generator(uint64_t seed) : rng_(seed) {}

    std::vector<Op> generate(size_t nOps, Stats& stats) {
        std::vector<Op> flow;
        flow.reserve(nOps);
        for (size_t i = 0; i < nOps; i++) {
            Op op = nextOp(stats);
            apply(op, stats);
            stats.ops++;
            flow.push_back(op);
            sampleDepth(stats);
        }
        return flow;
    }

private:
    std::mt19937_64 rng_;
    RefBook shadow_[kNumSymbols];  // the generator's own copy, so a flow is seed-pure

    uint64_t pick(uint64_t n) { return rng_() % n; }

    size_t symIndex() { return static_cast<size_t>(pick(kNumSymbols)); }

    Price gridPrice() {
        const int k = kGridLow + static_cast<int>(pick(kGridHigh - kGridLow + 1));
        return kBasePrice + k * kTick;
    }

    // How many symbols currently hold a live order with this id.
    uint64_t idShare(OrderId id) const {
        uint64_t n = 0;
        for (const RefBook& b : shadow_) n += b.isLive(id) ? 1 : 0;
        return n;
    }

    Op nextOp(Stats& stats) {
        const size_t s = symIndex();
        RefBook& book = shadow_[s];
        Op op;
        op.sym = kSymbols[s];

        // Deep books are the interesting region, but the engine's order pool is
        // finite; past the cap, drain instead of adding.
        const bool mustDrain = book.restingCount() >= kDepthCap;
        const uint64_t roll = pick(100);

        if (mustDrain || roll >= 70) {
            const bool modify = !mustDrain && roll >= 90;
            op.kind = modify ? Op::Kind::Modify : Op::Kind::Cancel;
            op.id = pickLiveId(book);
            // 12% of the time address it as somebody who does not own it, which
            // must leave the order exactly where it is.
            op.requester = (pick(100) < 12) ? static_cast<ParticipantId>(1 + pick(kParticipants))
                                            : kAnyParticipant;
            // Aim a modify at a genuine reduction of the order it names, with a
            // steady minority of amendments the engine must REFUSE: up to the
            // same size, larger, or zero. A modify that is refused 80% of the
            // time is a modify that never tests the in-place reduction path.
            if (modify) {
                const Quantity current = book.qtyOf(op.id);
                const uint64_t edge = pick(100);
                if (current > 1 && edge < 70) op.qty = 1 + pick(current - 1);  // valid reduction
                else if (edge < 80) op.qty = current;                          // no change: refuse
                else if (edge < 90) op.qty = current + 1 + pick(5);            // increase: refuse
                else op.qty = 0;                                               // zero: refuse
                // newQty == 0 used to be suppressed here. It was a live engine
                // defect — the modify was applied, leaving the order resting at
                // zero quantity — so generating it halted every seed within a
                // couple of thousand operations and the rest of the space was
                // never reached. The engine now refuses it, as this reference
                // always has (RefMatcher::modify), so it is back in the mix and
                // every seed exercises the reject across thousands of flows.
            }
            return op;
        }

        op.kind = Op::Kind::Add;
        op.id = pickFreeId(book);
        op.side = pick(2) ? Side::Buy : Side::Sell;
        op.owner = (op.side == Side::Buy) ? kBuySide[pick(2)] : kSellSide[pick(2)];

        const uint64_t typeRoll = pick(100);
        if (typeRoll < 85) op.type = OrderType::Limit;
        else if (typeRoll < 90) op.type = OrderType::Market;
        else if (typeRoll < 96) op.type = OrderType::IOC;
        else op.type = OrderType::FOK;

        op.price = choosePrice(book, op.side, stats);
        op.qty = chooseQty(book, op.side, op.price, op.type);
        if (op.type == OrderType::Market) op.price = 0;  // market carries no limit
        return op;
    }

    // Price selection is where the whole test earns or wastes its time.
    //
    // Uniform random pricing produces a book two orders deep that never has a
    // queue, so queue position never decides anything and the interesting half
    // of price-time priority is never exercised. So most limit flow here is
    // PASSIVE — it joins its own side's touch or sits a tick or two behind it,
    // which is what builds real queues — and the aggressive minority is aimed
    // exactly AT a boundary: the opposite touch, one tick inside it, or a few
    // ticks through it.
    Price choosePrice(const RefBook& book, Side side, Stats& stats) {
        const Price sameBest = book.best(side);
        const Price oppBest = book.best(side == Side::Buy ? Side::Sell : Side::Buy);
        const int sign = (side == Side::Buy) ? 1 : -1;  // "better" direction for this side
        const uint64_t roll = pick(100);

        // Join our own queue, or fall in a tick or two behind it.
        if (roll < 65 && sameBest != 0) {
            const int back = static_cast<int>(pick(3));
            if (back == 0) stats.boundaryPriced++;
            return clamp(sameBest - sign * back * kTick);
        }
        // Exactly the opposite touch: crosses the top level and nothing more.
        if (roll < 80 && oppBest != 0) {
            stats.boundaryPriced++;
            return clamp(oppBest);
        }
        // One tick short of the opposite touch: rests inside the spread and
        // becomes the new best, which reorders both sides' boundaries.
        if (roll < 88 && oppBest != 0) {
            stats.boundaryPriced++;
            return clamp(oppBest - sign * kTick);
        }
        // Through the touch by a few ticks: sweeps more than one level.
        if (roll < 93 && oppBest != 0)
            return clamp(oppBest + sign * (1 + static_cast<int>(pick(4))) * kTick);

        return gridPrice();
    }

    // Every price stays inside a 19-tick universe, so neither side can ever
    // hold more levels than one L2 snapshot can report (MAX_DEPTH = 20). The
    // comparison aborts rather than silently checking a truncated ladder, and
    // this is what keeps that from happening.
    static Price clamp(Price p) {
        const Price lo = kBasePrice + kPriceSpan * kTick * -1;
        const Price hi = kBasePrice + kPriceSpan * kTick;
        return std::min(hi, std::max(lo, p));
    }

    // Aim a good share of aggressors exactly at the amount the book can cover,
    // and at one unit either side of it — the fill/kill edge for FOK and the
    // sweep/rest edge for everything else.
    Quantity chooseQty(const RefBook& book, Side side, Price price, OrderType type) {
        const bool isMarket = (type == OrderType::Market);
        const Quantity crossable = book.crossableQty(side, price, isMarket);
        const uint64_t roll = pick(100);
        if (crossable > 0 && roll < (type == OrderType::FOK ? 75u : 30u)) {
            const uint64_t edge = pick(3);
            if (edge == 0) return crossable;
            if (edge == 1) return crossable + 1;
            return crossable > 1 ? crossable - 1 : 1;
        }
        if (roll < 92) return 1 + pick(20);
        return 50 + pick(400);  // occasional sweeper
    }

    OrderId pickLiveId(const RefBook& book) {
        // Prefer an id that is actually resting here, but keep a steady trickle
        // of cancels for ids that are live on a DIFFERENT symbol only — that is
        // the shape of the cancel-routing defect class this test exists for.
        if (pick(100) < 22) return 1 + pick(kIdSpace);
        for (int attempt = 0; attempt < 24; attempt++) {
            const OrderId id = 1 + pick(kIdSpace);
            if (book.isLive(id)) return id;
        }
        return 1 + pick(kIdSpace);
    }

    OrderId pickFreeId(const RefBook& book) {
        // Half the time, go out of the way to pick an id that is free HERE but
        // already live on a DIFFERENT symbol. Duplicate client order ids across
        // books is the one documented correctness defect this project has had —
        // a cancel routed to the wrong book — and unit tests missed it for
        // months. Leaving that collision to chance is how a fuzzer politely
        // avoids the bug it was built to find.
        if (pick(2) == 0) {
            for (int attempt = 0; attempt < 32; attempt++) {
                const OrderId id = 1 + pick(kIdSpace);
                if (!book.isLive(id) && idShare(id) > 0) return id;
            }
        }
        for (int attempt = 0; attempt < 32; attempt++) {
            const OrderId id = 1 + pick(kIdSpace);
            if (!book.isLive(id)) return id;
        }
        return 1 + pick(kIdSpace);
    }

    void apply(const Op& op, Stats& stats) {
        RefBook& book = shadow_[symOf(op.sym)];
        const uint64_t share = idShare(op.id);
        if (share >= 2) stats.dupIdOps++;
        stats.dupIdMaxShare = std::max(stats.dupIdMaxShare, share);

        switch (op.kind) {
            case Op::Kind::Add: {
                stats.adds++;
                stats.byType[typeIndex(op.type)]++;
                std::vector<RefTrade> trades;
                const auto outcome = book.add(op.id, op.owner, op.side, op.price, op.qty,
                                              op.type, trades);
                if (op.type == OrderType::FOK) {
                    (outcome == RefMatch::AddOutcome::RejectedFOK ? stats.fokRejected
                                                                  : stats.fokAccepted)++;
                }
                Quantity filled = 0;
                for (const RefTrade& t : trades) filled += t.qty;
                stats.trades += trades.size();
                if (!trades.empty()) stats.aggressive++;
                if (filled == op.qty && outcome == RefMatch::AddOutcome::Accepted) stats.fullyFilled++;
                else if (filled > 0) stats.partiallyFilled++;
                else if (outcome == RefMatch::AddOutcome::Accepted) stats.restedUntouched++;
                break;
            }
            case Op::Kind::Cancel: {
                stats.cancels++;
                const bool wasLive = book.isLive(op.id);
                if (book.cancel(op.id, op.requester)) stats.cancelHit++;
                else if (wasLive) stats.cancelDenied++;
                else stats.cancelMiss++;
                break;
            }
            case Op::Kind::Modify:
                stats.modifies++;
                (book.modify(op.id, op.qty, op.requester) ? stats.modifyOk : stats.modifyRejected)++;
                break;
        }
    }

    void sampleDepth(Stats& stats) {
        for (const RefBook& b : shadow_) {
            const uint64_t n = b.restingCount();
            stats.restingSum += n;
            stats.restingSamples++;
            stats.restingMax = std::max(stats.restingMax, n);
            stats.levelMax = std::max({stats.levelMax, (uint64_t)b.levelCount(Side::Buy),
                                       (uint64_t)b.levelCount(Side::Sell)});
        }
    }

    static size_t symOf(SymbolId s) {
        for (size_t i = 0; i < kNumSymbols; i++)
            if (kSymbols[i] == s) return i;
        return 0;
    }

    static int typeIndex(OrderType t) {
        switch (t) {
            case OrderType::Market: return 1;
            case OrderType::IOC: return 2;
            case OrderType::FOK: return 3;
            default: return 0;
        }
    }
};

}  // namespace DiffFuzz
