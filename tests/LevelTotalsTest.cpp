// A price level's displayed totals (visible quantity, displayed order count)
// are maintained on every change rather than recomputed by walking the level
// (PERF-1: with a listener attached the walk made every add, fill and cancel
// O(orders at the price)). These tests attach a listener and check every
// published level against a recomputation from the orders themselves, on a
// random flow that reaches every path that changes a resting order's display:
// fills (both algorithms), iceberg refreshes, hidden orders, modifies,
// replaces, the four STP actions, the uncross and restore.
//
// Draws use rng() % n (libc++ and libstdc++ agree on mt19937_64's output, not
// on std::uniform_int_distribution's).

#include "OrderBook.h"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace OrderMatcher;

namespace {

using LevelKey = std::pair<int, Price>;                 // (side, price)
using LevelTotals = std::pair<Quantity, uint32_t>;      // (visible qty, displayed orders)

// The L2 updates one operation published, last one per level.
struct LevelRecorder : EventListener {
    std::map<LevelKey, LevelTotals> published;
    void onMarketData(const MarketDataUpdate& u) override {
        published[{static_cast<int>(u.side), u.level.price}] = {u.level.totalQuantity,
                                                                u.level.orderCount};
    }
};

// Ground truth: every displayed resting order, summed per level.
std::map<LevelKey, LevelTotals> walkLevels(const OrderBook& book) {
    std::map<LevelKey, LevelTotals> out;
    book.forEachOrderLocked([&](const Order& o) {
        if (!o.inBook || o.isHidden) return;
        auto& t = out[{static_cast<int>(o.side), o.price}];
        t.first += displayQuantity(o);
        ++t.second;
    });
    return out;
}

LevelTotals at(const std::map<LevelKey, LevelTotals>& m, const LevelKey& k) {
    const auto it = m.find(k);
    return it == m.end() ? LevelTotals{0, 0} : it->second;
}

std::string describe(const LevelKey& k, const LevelTotals& t) {
    return std::string(k.first == static_cast<int>(Side::Buy) ? "bid " : "ask ") +
           std::to_string(k.second) + " = " + std::to_string(t.first) + " in " +
           std::to_string(t.second);
}

class LevelTotalsFlow : public ::testing::TestWithParam<MatchAlgorithm> {};

// After every operation: the book's own invariant (maintained totals equal a
// walk), every level it published equals the walk, and every level whose
// displayed totals changed was published.
TEST_P(LevelTotalsFlow, EveryPublishedLevelEqualsARecomputation) {
    for (uint64_t seed = 1; seed <= 6; ++seed) {
        std::mt19937_64 rng(seed);
        LevelRecorder rec;
        auto book = std::make_unique<OrderBook>(0, GetParam());
        book->setEventListener(&rec);
        book->setTradingState(TradingState::Continuous);
        book->setSTPMode(2, STPMode::CancelResting);
        book->setSTPMode(3, STPMode::DecreaseAndCancel);
        book->setSTPMode(4, STPMode::CancelBoth);

        std::vector<OrderId> ids;
        OrderId nextId = 1;
        auto before = walkLevels(*book);
        bool inAuction = false;

        for (int op = 0; op < 3000; ++op) {
            rec.published.clear();
            const uint64_t r = rng() % 100;
            const auto pickId = [&]() { return ids.empty() ? OrderId{0} : ids[rng() % ids.size()]; };
            std::string what;

            if (r < 50) {
                const OrderId id = nextId++;
                const Side side = (rng() % 2) ? Side::Buy : Side::Sell;
                const Price price = 9990 + static_cast<Price>(rng() % 21);
                const Quantity qty = 1 + rng() % 60;
                const ParticipantId pid = 1 + static_cast<ParticipantId>(rng() % 4);
                const uint64_t kind = rng() % 10;
                what = "add #" + std::to_string(id) + " kind " + std::to_string(kind) + " p" +
                       std::to_string(pid) + (side == Side::Buy ? " buy " : " sell ") +
                       std::to_string(qty) + "@" + std::to_string(price);
                if (kind < 5) {
                    book->addOrder(id, pid, side, price, qty, OrderType::Limit);
                } else if (kind < 7) {
                    book->addOrder(id, pid, side, price, qty, OrderType::Iceberg, 0,
                                   1 + rng() % 15);
                } else if (kind < 8) {
                    book->addOrder(id, pid, side, price, qty, OrderType::Hidden);
                } else if (kind < 9) {
                    // A hidden iceberg displays nothing at all.
                    book->addOrder(id, pid, side, price, qty, OrderType::Iceberg, 0,
                                   1 + rng() % 15, TimeInForce::GTC, 0, 0, PegType::None,
                                   0, 0, 0, /*hidden=*/true);
                } else {
                    book->addOrder(id, pid, side, price, qty, OrderType::IOC);
                }
                ids.push_back(id);
            } else if (r < 62) {
                const OrderId id = pickId();
                what = "cancel #" + std::to_string(id);
                book->cancelOrder(id);
            } else if (r < 74) {
                const OrderId id = pickId();
                what = "modify #" + std::to_string(id);
                if (const Order* o = book->getOrder(id); o && o->remainingQty > 1)
                    book->modifyOrder(id, 1 + rng() % (o->remainingQty - 1));
            } else if (r < 92) {
                const OrderId id = pickId();
                what = "replace #" + std::to_string(id);
                if (const Order* o = book->getOrder(id)) {
                    const bool samePrice = rng() % 2;
                    const Price px = samePrice ? o->price : 9990 + static_cast<Price>(rng() % 21);
                    book->cancelReplace(id, px, o->cumQty + 1 + rng() % 60);
                }
            } else if (r < 96) {
                // The opening auction: crossing orders accumulate, and the
                // return to Continuous runs the uncross.
                inAuction = !inAuction;
                what = inAuction ? "enter auction" : "uncross";
                book->setTradingState(inAuction ? TradingState::AuctionOpen
                                                : TradingState::Continuous);
            } else {
                // A large sweep through several levels.
                const OrderId id = nextId++;
                what = "sweep #" + std::to_string(id);
                const Side side = (rng() % 2) ? Side::Buy : Side::Sell;
                book->addOrder(id, 1 + static_cast<ParticipantId>(rng() % 4), side,
                               side == Side::Buy ? 10010 : 9990, 200 + rng() % 200,
                               OrderType::IOC);
            }

            const std::string where = "seed " + std::to_string(seed) + " op " +
                                      std::to_string(op) + " (" + what + ")";
            std::string err;
            ASSERT_TRUE(book->validateIntegrity(&err)) << where << ": " << err;

            const auto after = walkLevels(*book);
            for (const auto& [key, got] : rec.published) {
                const LevelTotals want = at(after, key);
                ASSERT_EQ(got, want) << where << ": published " << describe(key, got)
                                     << ", the orders show " << describe(key, want);
            }
            // Completeness: a level whose displayed totals changed was published.
            auto changed = [&](const std::map<LevelKey, LevelTotals>& a,
                               const std::map<LevelKey, LevelTotals>& b) {
                for (const auto& [key, t] : a) {
                    if (at(b, key) != t && !rec.published.count(key)) {
                        ADD_FAILURE() << where << ": " << describe(key, at(after, key))
                                      << " (was " << describe(key, at(before, key))
                                      << ") changed and was not published";
                        return true;
                    }
                }
                return false;
            };
            if (changed(after, before) || changed(before, after)) return;
            before = after;
        }
        book->setEventListener(nullptr);
    }
}

INSTANTIATE_TEST_SUITE_P(Algorithms, LevelTotalsFlow,
                         ::testing::Values(MatchAlgorithm::PriceTime, MatchAlgorithm::ProRata));

// A restored iceberg's slice is put back after the order rests; the level's
// displayed total must follow it, or a restarted book publishes the full
// first slice instead of the one it had.
TEST(LevelTotals, ARestoredIcebergSliceIsWhatTheLevelShows) {
    auto book = std::make_unique<OrderBook>(0, MatchAlgorithm::PriceTime);
    LevelRecorder rec;
    book->setEventListener(&rec);
    ASSERT_EQ(book->restoreOrder(1, 1, Side::Sell, 10000, 100, OrderType::Iceberg, 0, 20,
                                 TimeInForce::GTC, 0, 0, PegType::None, 0, 0, 0, false, 0),
              RejectReason::None);
    ASSERT_EQ(book->restoreOrder(2, 2, Side::Sell, 10000, 30, OrderType::Limit, 0, 0,
                                 TimeInForce::GTC, 0, 0, PegType::None, 0, 0, 0, false, 0),
              RejectReason::None);
    book->restoreIcebergSlice(1, 7);

    std::string err;
    EXPECT_TRUE(book->validateIntegrity(&err)) << err;
    const MarketDataSnapshot snap = book->getSnapshot(1);
    ASSERT_EQ(snap.askCount, 1u);
    EXPECT_EQ(snap.asks[0].totalQuantity, 37u);
    EXPECT_EQ(snap.asks[0].orderCount, 2u);
    book->setEventListener(nullptr);
}

// The cost of a level's update does not depend on its depth: 20,000 orders on
// one price, every add publishes the exact level, and the totals read are the
// maintained ones (validateIntegrity proves they equal a walk). Counted, not
// timed: the update carries the level's totals for every one of them.
TEST(LevelTotals, ADeepLevelPublishesExactTotalsOnEveryAdd) {
    constexpr int kOrders = 20000;
    auto book = std::make_unique<OrderBook>(0, MatchAlgorithm::PriceTime, 2 * kOrders);
    struct Counter : EventListener {
        uint64_t updates = 0;
        bool exact = true;
        void onMarketData(const MarketDataUpdate& u) override {
            ++updates;
            exact = exact && u.level.orderCount == updates && u.level.totalQuantity == 2 * updates;
        }
    } counter;
    book->setEventListener(&counter);
    for (int i = 0; i < kOrders; ++i)
        book->addOrder(static_cast<OrderId>(i + 1), 1 + (i % 8), Side::Buy, 10000, 2,
                       OrderType::Limit);
    EXPECT_EQ(counter.updates, static_cast<uint64_t>(kOrders));
    EXPECT_TRUE(counter.exact);
    std::string err;
    EXPECT_TRUE(book->validateIntegrity(&err)) << err;
    book->setEventListener(nullptr);
}

// PERF-9: without a listener no update is built and no clock is read, but
// each would-be update still draws its sequence number, so the numbers the
// trades carry do not depend on whether anyone listens.
TEST(ListenerFreePath, TradeSequenceNumbersDoNotDependOnAListener) {
    auto run = [](bool withListener) {
        struct Noop : EventListener {} noop;
        auto book = std::make_unique<OrderBook>(0, MatchAlgorithm::PriceTime);
        if (withListener) book->setEventListener(&noop);
        std::mt19937_64 rng(7);
        for (OrderId id = 1; id <= 2000; ++id) {
            if (rng() % 5 == 0) {
                book->cancelOrder(1 + rng() % id);
                continue;
            }
            book->addOrder(id, 1 + static_cast<ParticipantId>(rng() % 5),
                           (rng() % 2) ? Side::Buy : Side::Sell,
                           9995 + static_cast<Price>(rng() % 11), 1 + rng() % 40,
                           (rng() % 4) ? OrderType::Limit : OrderType::Iceberg, 0,
                           1 + rng() % 10);
        }
        book->setEventListener(nullptr);
        // The ring is the only record a listener-free book keeps of its trades.
        auto& ring = const_cast<RingBuffer<Trade>&>(book->getTradeRingBuffer());
        std::vector<uint64_t> seqs;
        for (Trade t{}; ring.pop(t);) seqs.push_back(t.sequenceNumber);
        return seqs;
    };
    const std::vector<uint64_t> without = run(false);
    ASSERT_FALSE(without.empty());
    EXPECT_EQ(without, run(true));
}

}  // namespace
