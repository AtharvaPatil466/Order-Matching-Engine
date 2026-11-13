// UncrossOracleTest — the uncross against an independent reference, on many
// random books. Adapted from the round-8 review's uncross_fuzz.cpp.
//
// The reference is written from docs/MatchingRules.md, not from the code:
//   Rule 4.4  self-cross plan: a same-owner pair is resolved only when both are
//             executable at the clearing price; fixed point; owner's STP mode;
//             "incoming" is the later arrival, ties to the buyer.
//   Rule 7.2  discovery: max volume, min imbalance, nearest reference, then the
//             incumbent's surplus side over an ascending scan (B7).
//   Rule 7.3  one price, strict price-time: parked markets first in arrival
//             order, then limits by price, then arrival.
// Books mix limits and parked market orders over 7 prices (so ties are
// common), three owners with random STP modes, random cancels before the cross
// (which once reordered the parked list, MIDS-4), and a reference price in
// half the runs. Checked per book: cross or not, price (exact, and in the
// tied set), volume, every order's fill, the CancelledBySTP set, no
// self-trade, and book integrity after the cross.

#include "EventListener.h"
#include "OrderBook.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace OrderMatcher;

namespace {

struct In {
    OrderId id; ParticipantId pid; Side side; bool mkt; Price px; Quantity qty; int seq;
    uint64_t age;  // the engine's age key for "which is newer" (see ageKey)
};

// The one place this test reads an order's age: the per-book arrival
// sequence, as the engine's arrivedNoEarlierThan does (MIDS-1).
uint64_t ageKey(const Order& o) { return o.arrivalSeq; }

struct Cap : EventListener {
    std::vector<Trade> trades;
    std::set<OrderId> stp;
    void onTrade(const Trade& t) override { trades.push_back(t); }
    void onOrderUpdate(const OrderUpdate& u) override {
        if (u.status == OrderStatus::CancelledBySTP) stp.insert(u.orderId);
    }
};

struct Discovery { bool cross = false; Price px = 0; Quantity vol = 0; std::set<Price> tied; };

bool executable(const In& o, Price p) {
    return o.mkt || (o.side == Side::Buy ? o.px >= p : o.px <= p);
}

// Most aggressive first: markets by arrival, then limits by price, then arrival.
void priority(std::vector<const In*>& v, Side side) {
    std::stable_sort(v.begin(), v.end(), [side](const In* x, const In* y) {
        if (x->mkt != y->mkt) return x->mkt;
        if (!x->mkt && x->px != y->px) return side == Side::Buy ? x->px > y->px : x->px < y->px;
        return x->seq < y->seq;
    });
}

Discovery discover(const std::vector<const In*>& v, Price ref) {
    Discovery d;
    Quantity mb = 0, ms = 0;
    std::set<Price> prices;
    for (const In* o : v) {
        if (o->mkt) (o->side == Side::Buy ? mb : ms) += o->qty;
        else prices.insert(o->px);
    }
    if (prices.empty()) return d;  // no limit price, no last trade: nothing to cross at
    struct C { Price p; Quantity vol, imb; bool buySurplus; };
    std::vector<C> cs;
    for (Price p : prices) {
        Quantity cb = mb, cl = ms;
        for (const In* o : v) {
            if (o->mkt) continue;
            if (o->side == Side::Buy && o->px >= p) cb += o->qty;
            if (o->side == Side::Sell && o->px <= p) cl += o->qty;
        }
        cs.push_back({p, std::min(cb, cl), cb >= cl ? cb - cl : cl - cb, cb >= cl});
    }
    Quantity bv = 0;
    for (const C& c : cs) bv = std::max(bv, c.vol);
    if (bv == 0) return d;
    Quantity bi = ~Quantity{0};
    for (const C& c : cs) if (c.vol == bv) bi = std::min(bi, c.imb);
    for (const C& c : cs) if (c.vol == bv && c.imb == bi) d.tied.insert(c.p);
    // Steps 3-4 over the ascending scan, against the incumbent (B7).
    const C* best = nullptr;
    for (const C& c : cs) {
        if (c.vol != bv || c.imb != bi) continue;
        if (!best) { best = &c; continue; }
        const Price dc = ref ? std::abs(c.p - ref) : 0, db = ref ? std::abs(best->p - ref) : 0;
        if (dc != db) { if (dc < db) best = &c; continue; }
        if (best->buySurplus ? c.p > best->p : c.p < best->p) best = &c;
    }
    d.cross = true; d.px = best->p; d.vol = bv;
    return d;
}

std::vector<const In*> without(const std::vector<In>& v, const std::set<OrderId>& gone) {
    std::vector<const In*> out;
    for (const In& o : v) if (!gone.count(o.id)) out.push_back(&o);
    return out;
}

std::set<OrderId> selfCrossPlan(const std::vector<In>& v, const std::map<ParticipantId, STPMode>& modes,
                                Price ref) {
    std::set<OrderId> cut;
    for (;;) {
        const auto live = without(v, cut);
        const Discovery d = discover(live, ref);
        if (!d.cross) return cut;
        const size_t before = cut.size();
        for (const auto& [pid, mode] : modes) {
            std::vector<const In*> b, s;
            for (const In* o : live)
                if (o->pid == pid && executable(*o, d.px)) (o->side == Side::Buy ? b : s).push_back(o);
            priority(b, Side::Buy);
            priority(s, Side::Sell);
            for (size_t i = 0, j = 0; i < b.size() && j < s.size();) {
                const bool buyNewer = b[i]->age >= s[j]->age;
                bool cutB = false, cutS = false;
                switch (mode) {
                case STPMode::DefaultCancelIncoming:
                case STPMode::CancelIncoming:    (buyNewer ? cutB : cutS) = true; break;
                case STPMode::CancelResting:
                case STPMode::DecreaseAndCancel: (buyNewer ? cutS : cutB) = true; break;
                case STPMode::CancelBoth:        cutB = cutS = true; break;
                }
                if (cutB) cut.insert(b[i++]->id);
                if (cutS) cut.insert(s[j++]->id);
            }
        }
        if (cut.size() == before) return cut;
    }
}

std::map<OrderId, Quantity> allocate(const std::vector<const In*>& live, Price p, Quantity vol) {
    std::vector<const In*> b, s;
    for (const In* o : live)
        if (executable(*o, p)) (o->side == Side::Buy ? b : s).push_back(o);
    priority(b, Side::Buy);
    priority(s, Side::Sell);
    std::map<OrderId, Quantity> fills, left;
    for (const In* o : live) left[o->id] = o->qty;
    size_t i = 0, j = 0;
    while (vol > 0 && i < b.size() && j < s.size()) {
        const Quantity q = std::min({left[b[i]->id], left[s[j]->id], vol});
        fills[b[i]->id] += q; fills[s[j]->id] += q;
        left[b[i]->id] -= q; left[s[j]->id] -= q; vol -= q;
        if (left[b[i]->id] == 0) ++i;
        if (left[s[j]->id] == 0) ++j;
    }
    return fills;
}

std::string describe(const std::vector<In>& v) {
    std::string s;
    for (const In& o : v)
        s += "#" + std::to_string(o.id) + " p" + std::to_string(o.pid) +
             (o.side == Side::Buy ? " B " : " S ") + (o.mkt ? "MKT" : std::to_string(o.px)) +
             " x" + std::to_string(o.qty) + "\n";
    return s;
}

void runOracle(bool selfCrossing, uint64_t seed, int runs) {
    std::mt19937_64 rng(seed);
    const STPMode kModes[] = {STPMode::DefaultCancelIncoming, STPMode::CancelIncoming,
                              STPMode::CancelResting, STPMode::CancelBoth, STPMode::DecreaseAndCancel};
    int crosses = 0, stpCuts = 0;
    for (int it = 0; it < runs; ++it) {
        OrderBook book(0);
        Cap cap;
        book.setEventListener(&cap);
        book.setCircuitBreakerThreshold(1e9);
        book.setTradingState(TradingState::AuctionOpen);
        const Price ref = rng() % 2 ? 1000 + static_cast<Price>(rng() % 7) : 0;
        if (ref) book.setReferencePrice(ref);
        std::map<ParticipantId, STPMode> modes;

        std::vector<In> all;
        const int n = 1 + static_cast<int>(rng() % 12);
        for (int k = 0; k < n; ++k) {
            In o{};
            o.id = static_cast<OrderId>(k + 1);
            o.seq = k;
            o.pid = selfCrossing ? static_cast<ParticipantId>(1 + rng() % 3) : static_cast<ParticipantId>(k + 1);
            o.side = rng() % 2 ? Side::Buy : Side::Sell;
            o.mkt = rng() % 6 == 0;
            o.px = o.mkt ? 0 : 1000 + static_cast<Price>(rng() % 7);
            o.qty = 1 + rng() % 20;
            if (!modes.count(o.pid)) {
                modes[o.pid] = selfCrossing ? kModes[rng() % 5] : STPMode::DefaultCancelIncoming;
                book.setSTPMode(o.pid, modes[o.pid]);
            }
            const auto res = book.addOrder(o.id, o.pid, o.side, o.px, o.qty,
                                           o.mkt ? OrderType::Market : OrderType::Limit);
            ASSERT_TRUE(std::holds_alternative<OrderId>(res)) << "add refused, run " << it;
            const Order* live = book.getOrder(o.id);
            ASSERT_NE(live, nullptr);
            o.age = ageKey(*live);
            all.push_back(o);
        }
        std::vector<In> v;
        for (const In& o : all) {
            if (rng() % 8 == 0) { book.cancelOrder(o.id); continue; }
            v.push_back(o);
        }

        const std::set<OrderId> cut = selfCrossing ? selfCrossPlan(v, modes, ref) : std::set<OrderId>{};
        const auto live = without(v, cut);
        const Discovery d = discover(live, ref);
        book.uncross();

        std::set<Price> printed;
        Quantity vol = 0;
        std::map<OrderId, Quantity> fills;
        for (const Trade& t : cap.trades) {
            printed.insert(t.price);
            vol += t.quantity;
            fills[t.buyOrderId] += t.quantity;
            fills[t.sellOrderId] += t.quantity;
            ASSERT_NE(t.buyerId, t.sellerId) << "self-trade, run " << it << "\n" << describe(v);
        }
        const std::string ctx = "run " + std::to_string(it) + " ref " + std::to_string(ref) + "\n" + describe(v);
        ASSERT_EQ(cap.stp, cut) << ctx;
        ASSERT_EQ(d.cross, !cap.trades.empty()) << ctx;
        stpCuts += static_cast<int>(cut.size());
        if (!d.cross) continue;
        ++crosses;
        ASSERT_EQ(printed.size(), 1u) << ctx;
        ASSERT_EQ(vol, d.vol) << ctx;
        ASSERT_TRUE(d.tied.count(*printed.begin())) << ctx;
        ASSERT_EQ(*printed.begin(), d.px) << ctx;
        ASSERT_EQ(fills, allocate(live, d.px, d.vol)) << ctx;
        std::string err;
        ASSERT_TRUE(book.validateIntegrity(&err)) << err << "\n" << ctx;
    }
    // The generator must actually reach the cases it is here for.
    EXPECT_GT(crosses, runs / 3);
    if (selfCrossing) EXPECT_GT(stpCuts, runs / 4);
}

}  // namespace

TEST(UncrossOracle, DistinctOwnersMatchTheReference) { runOracle(false, 12345, 20000); }

TEST(UncrossOracle, SelfCrossingOwnersMatchTheReference) { runOracle(true, 12346, 20000); }
