#include "OrderBook.h"
#include <cassert>
#include <iostream>
#include <random>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <algorithm>
#include <limits>
#include <map>
#include <string>

using namespace OrderMatcher;

// ─── Property-Based Test Framework ──────────────────────────────────────────
// Generates random order sequences with Zipf-distributed prices (realistic)
// and checks five properties after every operation.

// Zipf distribution for realistic price clustering
class ZipfDistribution {
public:
    ZipfDistribution(size_t n, double alpha = 1.0) : n_(n) {
        harmonics_.resize(n + 1, 0.0);
        for (size_t i = 1; i <= n; ++i)
            harmonics_[i] = harmonics_[i - 1] + 1.0 / std::pow(static_cast<double>(i), alpha);
    }

    size_t operator()(std::mt19937& rng) const {
        std::uniform_real_distribution<double> dist(0.0, harmonics_[n_]);
        double u = dist(rng);
        // Binary search for the bucket
        auto it = std::lower_bound(harmonics_.begin(), harmonics_.end(), u);
        return std::min(static_cast<size_t>(std::distance(harmonics_.begin(), it)), n_);
    }

private:
    size_t n_;
    std::vector<double> harmonics_;
};

// ─── Invariant Checkers ─────────────────────────────────────────────────────
//
// Every property is checked after EVERY operation. An every-tenth-op check
// lets a violation that the next nine operations repair go unseen.

// Property 1: Best bid < best ask (spread invariant)
bool checkSpreadInvariant(const OrderBook& book) {
    Price bestBid = book.getBestBid();
    Price bestAsk = book.getBestAsk();
    if (bestBid > 0 && bestAsk < std::numeric_limits<Price>::max()) {
        return bestBid < bestAsk;
    }
    return true; // One side empty — invariant trivially holds
}

// Property 2: Price levels are sorted (bids descending, asks ascending)
bool checkLevelsSorted(const OrderBook& book) {
    MarketDataSnapshot snap = book.getSnapshot(20);
    for (size_t i = 1; i < snap.bidCount; ++i)
        if (snap.bids[i].price > snap.bids[i - 1].price) return false;
    for (size_t i = 1; i < snap.askCount; ++i)
        if (snap.asks[i].price < snap.asks[i - 1].price) return false;
    return true;
}

struct TradeMonitor : EventListener {
    std::vector<Trade> trades;
    void onTrade(const Trade& t) override { trades.push_back(t); }
};

// Properties 3-5 need to know what the book SHOULD hold, so this keeps a model
// of every order: what it has left (its size minus its fills; a modify resets
// it) and when it joined its level's queue.
//
//   3. Trade ids strictly increase, and no trade is for zero.
//   4. Per-order conservation: an order is never filled past its size, a
//      resting order holds exactly its size minus its fills, and an order only
//      leaves the book filled or cancelled.
//   5. In-level FIFO by maker: a trade's maker is the oldest order still
//      queued at its price — nothing that joined the level earlier survives
//      a fill of something that joined later.
class OrderModel {
public:
    void submitted(OrderId id, Quantity qty) {
        orders_[id] = Expect{qty};
        touched_.push_back(id);
    }
    void modified(OrderId id, Quantity qty) {
        orders_.at(id).leaves = qty;
        touched_.push_back(id);
    }
    void cancelRequested(OrderId id) {
        cancelled_ = id;
        touched_.push_back(id);
    }

    // Applies one operation's trades and checks every order it touched.
    // Returns the first violation, or "" if there is none.
    std::string settle(const OrderBook& book, const std::vector<Trade>& trades, size_t from,
                       uint64_t op) {
        std::vector<OrderId> makers;
        std::string why = applyTrades(trades, from, makers);
        for (size_t i = 0; why.empty() && i < touched_.size(); ++i)
            why = reconcile(book, touched_[i], op);
        for (size_t i = 0; why.empty() && i < makers.size(); ++i) why = checkFifo(makers[i]);
        touched_.clear();
        cancelled_ = 0;
        return why;
    }

    // The whole book against the whole model: orders an operation did not
    // name must not have changed either.
    std::string reconcileAll(const OrderBook& book) const {
        std::string why;
        size_t resting = 0;
        book.forEachOrder([&](const Order& o) {
            ++resting;
            auto it = orders_.find(o.id);
            if (why.empty() && (it == orders_.end() || !it->second.resting ||
                                it->second.leaves != o.remainingQty))
                why = "order #" + std::to_string(o.id) + " rests with " +
                      std::to_string(o.remainingQty) + ", which the model does not hold";
        });
        size_t modelled = 0;
        for (const auto& [id, e] : orders_) modelled += e.resting ? 1 : 0;
        if (why.empty() && resting != modelled)
            why = std::to_string(resting) + " orders rest, the model holds " +
                  std::to_string(modelled);
        return why;
    }

private:
    static constexpr uint64_t kNeverRested = ~0ULL;
    struct Expect {
        Quantity leaves = 0;
        uint64_t arrival = kNeverRested;
        bool resting = false;
        Side side = Side::Buy;
        Price price = 0;
    };
    using LevelKey = std::pair<int, Price>;

    std::unordered_map<OrderId, Expect> orders_;
    std::map<LevelKey, std::map<uint64_t, OrderId>> queues_;  // resting, oldest first
    std::vector<OrderId> touched_;
    OrderId cancelled_ = 0;
    uint64_t lastTradeId_ = 0;

    static LevelKey key(const Expect& e) { return {e.side == Side::Buy ? 0 : 1, e.price}; }

    std::string applyTrades(const std::vector<Trade>& trades, size_t from,
                            std::vector<OrderId>& makers) {
        for (size_t i = from; i < trades.size(); ++i) {
            const Trade& t = trades[i];
            if (t.quantity == 0) return "zero-quantity trade #" + std::to_string(t.tradeId);
            if (t.tradeId <= lastTradeId_) return "trade id " + std::to_string(t.tradeId) +
                                                  " does not exceed " + std::to_string(lastTradeId_);
            lastTradeId_ = t.tradeId;
            for (OrderId id : {t.buyOrderId, t.sellOrderId}) {
                auto it = orders_.find(id);
                if (it == orders_.end()) return "trade names unknown order #" + std::to_string(id);
                if (t.quantity > it->second.leaves)
                    return "order #" + std::to_string(id) + " filled " + std::to_string(t.quantity) +
                           " with only " + std::to_string(it->second.leaves) + " left";
                it->second.leaves -= t.quantity;
                touched_.push_back(id);
            }
            makers.push_back(t.aggressorSide == Side::Buy ? t.sellOrderId : t.buyOrderId);
        }
        return "";
    }

    std::string reconcile(const OrderBook& book, OrderId id, uint64_t op) {
        Expect& e = orders_.at(id);
        const Order* o = book.getOrder(id);
        const std::string name = "order #" + std::to_string(id);
        if (o && o->inBook) {
            if (o->remainingQty == 0) return name + " rests with nothing left";
            if (o->remainingQty != e.leaves)
                return name + " rests with " + std::to_string(o->remainingQty) +
                       "; size minus fills is " + std::to_string(e.leaves);
            if (!e.resting) {
                e.resting = true;
                e.arrival = op;
                e.side = o->side;
                e.price = o->price;
                queues_[key(e)][op] = id;
            }
        } else if (e.resting) {
            if (e.leaves != 0 && id != cancelled_)
                return name + " left the book with " + std::to_string(e.leaves) +
                       " unfilled and was not cancelled";
            auto q = queues_.find(key(e));
            q->second.erase(e.arrival);
            if (q->second.empty()) queues_.erase(q);
            e.resting = false;
        }
        return "";
    }

    std::string checkFifo(OrderId maker) const {
        const Expect& e = orders_.at(maker);
        if (e.arrival == kNeverRested)
            return "maker #" + std::to_string(maker) + " was never resting";
        auto q = queues_.find(key(e));
        if (q == queues_.end() || q->second.begin()->first >= e.arrival) return "";
        return "FIFO: maker #" + std::to_string(maker) + " filled at " + std::to_string(e.price) +
               " while #" + std::to_string(q->second.begin()->second) +
               ", queued there earlier, is still waiting";
    }
};

void expectHolds(const std::string& why, uint64_t seed, size_t op) {
    if (why.empty()) return;
    std::cerr << "PROPERTY VIOLATION (seed " << seed << ", op " << op << "): " << why << std::endl;
    assert(false && "property violated");
}

// ─── Main Test Runner ───────────────────────────────────────────────────────

void runPropertyTest(uint64_t seed, size_t numOps) {
    OrderBook book;
    TradeMonitor monitor;
    book.setEventListener(&monitor);
    OrderModel model;

    std::mt19937 rng(seed);
    ZipfDistribution priceDist(200, 1.0); // 200 distinct price offsets, Zipf clustered
    std::uniform_int_distribution<int> actionDist(0, 99);
    std::uniform_int_distribution<ParticipantId> pidDist(1, 50);
    std::uniform_int_distribution<Quantity> qtyDist(1, 500);

    OrderId nextId = 1;
    std::vector<OrderId> activeOrders;
    activeOrders.reserve(numOps);

    Price basePrice = 100000; // Center price

    for (size_t op = 0; op < numOps; ++op) {
        int action = actionDist(rng);
        const size_t tradesBefore = monitor.trades.size();

        if (action < 55 || activeOrders.empty()) {
            // 55% Add order
            OrderId id = nextId++;
            Side side = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
            Price offset = static_cast<Price>(priceDist(rng));
            Price price = (side == Side::Buy)
                ? basePrice - offset
                : basePrice + offset;
            if (price == 0) price = 1;
            Quantity qty = qtyDist(rng);

            model.submitted(id, qty);
            book.addOrder(id, pidDist(rng), side, price, qty, OrderType::Limit);
            activeOrders.push_back(id);
        } else if (action < 80) {
            // 25% Cancel
            size_t idx = rng() % activeOrders.size();
            model.cancelRequested(activeOrders[idx]);
            book.cancelOrder(activeOrders[idx]);
            activeOrders[idx] = activeOrders.back();
            activeOrders.pop_back();
        } else if (action < 90) {
            // 10% Modify (reduce qty)
            size_t idx = rng() % activeOrders.size();
            const Quantity newQty = 1 + (rng() % 10);
            if (book.modifyOrder(activeOrders[idx], newQty)) model.modified(activeOrders[idx], newQty);
        } else {
            // 10% Aggressive cross (market-like)
            OrderId id = nextId++;
            Side side = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
            Price price = (side == Side::Buy) ? basePrice + 500 : basePrice - 500;
            const Quantity qty = qtyDist(rng);
            model.submitted(id, qty);
            book.addOrder(id, pidDist(rng), side, price, qty, OrderType::Limit);
        }

        assert(checkSpreadInvariant(book) && "PROPERTY VIOLATION: Best bid >= best ask!");
        assert(checkLevelsSorted(book) && "PROPERTY VIOLATION: Price levels not sorted!");
        expectHolds(model.settle(book, monitor.trades, tradesBefore, op), seed, op);
    }
    expectHolds(model.reconcileAll(book), seed, numOps);

    std::cout << "  Seed " << seed << ": " << numOps << " ops, " << monitor.trades.size()
              << " trades, every property checked after every op — PASSED" << std::endl;
}

// ─── Edge Case Tests ────────────────────────────────────────────────────────

void testFOKRejection() {
    std::cout << "  FOK rejection property...";
    OrderBook book;
    TradeMonitor monitor;
    book.setEventListener(&monitor);

    // Add small resting qty
    book.addOrder(1, 1, Side::Sell, 100000, 10, OrderType::Limit);

    // FOK for more than available — should be rejected entirely
    book.addOrder(2, 2, Side::Buy, 100000, 100, OrderType::FOK);
    
    assert(monitor.trades.empty() && "FOK should not partially fill!");
    assert(checkSpreadInvariant(book));
    std::cout << " PASSED" << std::endl;
}

void testIOCPartialFill() {
    std::cout << "  IOC partial fill property...";
    OrderBook book;
    TradeMonitor monitor;
    book.setEventListener(&monitor);

    book.addOrder(1, 1, Side::Sell, 100000, 10, OrderType::Limit);
    book.addOrder(2, 2, Side::Buy, 100000, 25, OrderType::IOC);

    // Should fill 10, cancel remainder — no resting order for ID 2
    assert(monitor.trades.size() == 1);
    assert(monitor.trades[0].quantity == 10);
    
    // IOC remainder should NOT be in the book
    const Order* orders[100];
    size_t count = book.getAllOrders(orders, 100);
    for (size_t i = 0; i < count; ++i) {
        assert(orders[i]->id != 2 && "IOC order should not rest in book!");
    }

    std::cout << " PASSED" << std::endl;
}

void testIcebergRefresh() {
    std::cout << "  Iceberg refresh property...";
    OrderBook book;
    TradeMonitor monitor;
    book.setEventListener(&monitor);

    // Iceberg: 100 total, display 20
    book.addOrder(1, 1, Side::Sell, 100000, 100, OrderType::Iceberg, 0, 20);

    // Aggressor buys 20 — should fill visible tranche
    book.addOrder(2, 2, Side::Buy, 100000, 20, OrderType::Limit);
    assert(monitor.trades.size() == 1);
    assert(monitor.trades[0].quantity == 20);

    // Iceberg should still be in book with 80 remaining
    MarketDataSnapshot snap = book.getSnapshot(5);
    assert(snap.askCount > 0 && "Iceberg should still have hidden qty!");

    std::cout << " PASSED" << std::endl;
}

void testSelfMatchPrevention() {
    std::cout << "  Self-match prevention property...";
    OrderBook book;
    TradeMonitor monitor;
    book.setEventListener(&monitor);

    // Same participant on both sides
    book.addOrder(1, 42, Side::Sell, 100000, 100, OrderType::Limit);
    book.addOrder(2, 42, Side::Buy, 100000, 50, OrderType::Limit);

    // SMP should prevent the trade
    assert(monitor.trades.empty() && "SMP: same participant should not self-trade!");

    std::cout << " PASSED" << std::endl;
}

int main() {
    std::cout << "\n=== Property-Based Tests ===" << std::endl;

    // Run with 10 different random seeds, 10K ops each
    std::cout << "\nRandomized sequence tests (Zipf prices, 10K ops each):" << std::endl;
    for (uint64_t seed = 1; seed <= 10; ++seed) {
        runPropertyTest(seed, 10000);
    }

    // Run one large test with 100K ops
    std::cout << "\nLarge sequence test (100K ops):" << std::endl;
    runPropertyTest(42, 100000);

    // Targeted edge-case property tests
    std::cout << "\nEdge-case property tests:" << std::endl;
    testFOKRejection();
    testIOCPartialFill();
    testIcebergRefresh();
    testSelfMatchPrevention();

    std::cout << "\nALL PROPERTY TESTS PASSED!" << std::endl;
    return 0;
}
