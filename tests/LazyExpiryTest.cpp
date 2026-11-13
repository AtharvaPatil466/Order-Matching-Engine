// LazyExpiryTest — roadmap 1.9 I9, THR-10.
//
// Expiry was only the 1 Hz sweep (made permanent in both binaries by 0.18), so an
// expired GTD or DAY order stayed on the book and traded for up to a second after
// its expiry. Now an expired maker never trades: the sweep that meets it removes
// it, with its expiry journaled like the sweep's, and the screens that count
// liquidity ahead of a sweep (FOK and minQty, the breaker's first fill) look past
// it the same way. Replay never consults the clock: it applies the journaled
// cancel.

#include "MatchingEngine.h"
#include "TempPath.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

using namespace OrderMatcher;

namespace {

constexpr SymbolId kSym = 1;
constexpr Price kPx = 1'000'000;
constexpr uint64_t kExpiry = 1'000;

struct Recorder : EventListener {
    std::vector<Trade> trades;
    std::vector<OrderUpdate> updates;
    void onTrade(const Trade& t) override { trades.push_back(t); }
    void onOrderUpdate(const OrderUpdate& u) override { updates.push_back(u); }
    Quantity filled(OrderId id) const {
        Quantity q = 0;
        for (const Trade& t : trades)
            if (t.buyOrderId == id || t.sellOrderId == id) q += t.quantity;
        return q;
    }
    bool cancelled(OrderId id) const {
        for (const OrderUpdate& u : updates)
            if (u.orderId == id && u.status == OrderStatus::Cancelled) return true;
        return false;
    }
};

struct Mode {
    bool async;
    MatchAlgorithm algo;
};

class LazyExpiry : public ::testing::TestWithParam<Mode> {
protected:
    MatchingEngine engine;
    std::atomic<uint64_t> now{100};
    Recorder rec;

    void SetUp() override {
        engine.addSymbol(kSym, GetParam().algo);
        engine.getOrderBook(kSym)->setEventListener(&rec);
        engine.setExpiryClock([this] { return now.load(); });
        if (GetParam().async) engine.startAsync(1); else engine.start();
    }
    void TearDown() override {
        if (GetParam().async) engine.stopAsync(); else engine.stop();
    }
    void submit(OrderId id, ParticipantId pid, Side side, Price px, Quantity qty,
                TimeInForce tif = TimeInForce::GTC, uint64_t expiry = 0,
                OrderType type = OrderType::Limit) {
        ASSERT_TRUE(engine.submitOrder(kSym, id, pid, side, px, qty, type, 0, 0, tif, expiry)
                        .isAccepted() || type == OrderType::FOK);
        engine.waitForDrain();
    }
    const Order* order(OrderId id) { return engine.getOrderBook(kSym)->getOrder(id); }
};

TEST_P(LazyExpiry, ExpiredMakerNeverTradesAndIsCancelledWhenMet) {
    submit(1, 1, Side::Sell, kPx, 10, TimeInForce::GTD, kExpiry);
    submit(2, 2, Side::Sell, kPx, 10);
    now = kExpiry;   // due; the sweep has not run
    submit(3, 3, Side::Buy, kPx, 10);

    EXPECT_EQ(rec.filled(1), 0u);
    EXPECT_EQ(rec.filled(2), 10u);
    EXPECT_EQ(rec.filled(3), 10u);
    EXPECT_EQ(order(1), nullptr);
    EXPECT_TRUE(rec.cancelled(1)) << "its owner hears it expired";
}

TEST_P(LazyExpiry, DayOrderWithAnExpiryToo) {
    submit(1, 1, Side::Sell, kPx, 10, TimeInForce::DAY, kExpiry);
    submit(2, 2, Side::Sell, kPx, 10);
    now = kExpiry + 1;
    submit(3, 3, Side::Buy, kPx, 10);
    EXPECT_EQ(rec.filled(1), 0u);
    EXPECT_EQ(rec.filled(2), 10u);
}

TEST_P(LazyExpiry, NotYetDueOrGtcStillTrades) {
    submit(1, 1, Side::Sell, kPx, 10, TimeInForce::GTD, kExpiry);
    submit(2, 2, Side::Sell, kPx + 1, 10, TimeInForce::GTC, kExpiry);  // GTC ignores the field
    now = kExpiry - 1;
    submit(3, 3, Side::Buy, kPx, 10);
    now = kExpiry * 10;
    submit(4, 4, Side::Buy, kPx + 1, 10);
    EXPECT_EQ(rec.filled(1), 10u);
    EXPECT_EQ(rec.filled(2), 10u);
}

// A FOK that needs the expired maker's size must not fill part of itself.
TEST_P(LazyExpiry, FokDoesNotCountAnExpiredMaker) {
    submit(1, 1, Side::Sell, kPx, 10, TimeInForce::GTD, kExpiry);
    submit(2, 2, Side::Sell, kPx, 5);
    now = kExpiry;
    submit(3, 3, Side::Buy, kPx, 10, TimeInForce::GTC, 0, OrderType::FOK);
    EXPECT_TRUE(rec.trades.empty());
    ASSERT_NE(order(2), nullptr);
    EXPECT_EQ(order(2)->remainingQty, 5u);
}

// An expired maker at the touch must not hide a first fill past the breaker.
TEST_P(LazyExpiry, BreakerJudgesTheFirstLiveMaker) {
    // An order does not seed the reference (MR-1); set it before any order,
    // so the queue orders the write ahead of the worker's reads.
    engine.getOrderBook(kSym)->setReferencePrice(kPx);
    submit(1, 1, Side::Sell, kPx, 10, TimeInForce::GTD, kExpiry);
    submit(2, 2, Side::Sell, kPx + 100'000, 10);                     // +10%, past the 5% breaker
    now = kExpiry;
    submit(3, 3, Side::Buy, kPx + 100'000, 10);
    EXPECT_TRUE(rec.trades.empty());
    EXPECT_EQ(engine.getOrderBook(kSym)->getTradingState(), TradingState::VolatilityAuction);
}

TEST_P(LazyExpiry, UncrossDoesNotTradeAnExpiredOrder) {
    engine.beginTradingSession({kSym}, 20261107);
    submit(1, 1, Side::Sell, kPx, 10, TimeInForce::GTD, kExpiry);
    submit(2, 2, Side::Buy, kPx, 10);
    now = kExpiry;
    engine.uncross(kSym);
    engine.waitForDrain();
    EXPECT_TRUE(rec.trades.empty());
    EXPECT_EQ(order(1), nullptr);
    EXPECT_NE(order(2), nullptr);
}

INSTANTIATE_TEST_SUITE_P(Modes, LazyExpiry,
                         ::testing::Values(Mode{false, MatchAlgorithm::PriceTime},
                                           Mode{true, MatchAlgorithm::PriceTime},
                                           Mode{false, MatchAlgorithm::ProRata},
                                           Mode{true, MatchAlgorithm::ProRata}),
                         [](const auto& info) {
                             return std::string(info.param.async ? "Async" : "Sync") +
                                    (info.param.algo == MatchAlgorithm::ProRata ? "ProRata"
                                                                                : "PriceTime");
                         });

// The lazy expiry is journaled ahead of the order that met it, so replay applies
// it as a cancel and never reads a clock. The replaying engine's clock says every
// GTD order is long expired: if replay consulted it, order 2 would not trade.
TEST(LazyExpiryReplay, ReplayAppliesTheJournaledExpiryNotTheClock) {
    const std::string path = uniqueTempPath("lazy_expiry_replay.journal");
    std::remove(path.c_str());
    {
        MatchingEngine engine;
        engine.addSymbol(kSym);
        ASSERT_TRUE(engine.enableJournal(path));
        uint64_t now = 100;
        engine.setExpiryClock([&] { return now; });
        engine.start();
        auto add = [&](OrderId id, Side side, Quantity qty, TimeInForce tif, uint64_t exp) {
            ASSERT_TRUE(engine.submitOrder(kSym, id, id, side, kPx, qty, OrderType::Limit, 0, 0,
                                           tif, exp).isAccepted());
        };
        add(1, Side::Sell, 10, TimeInForce::GTD, kExpiry);
        add(2, Side::Sell, 10, TimeInForce::GTD, 5 * kExpiry);
        add(3, Side::Sell, 10, TimeInForce::GTC, 0);
        now = kExpiry;
        add(4, Side::Buy, 15, TimeInForce::GTC, 0);
        const OrderBook& b = *engine.getOrderBook(kSym);
        ASSERT_EQ(b.getOrder(1), nullptr);
        ASSERT_EQ(b.getOrder(2), nullptr);
        ASSERT_NE(b.getOrder(3), nullptr);
        ASSERT_EQ(b.getOrder(3)->remainingQty, 5u);
        engine.commitJournal();
        engine.stop();
    }
    MatchingEngine replayed;
    replayed.addSymbol(kSym);
    replayed.setExpiryClock([] { return uint64_t{1} << 62; });
    ASSERT_TRUE(replayed.enableJournal(path));
    replayed.replayJournal();
    const OrderBook& b = *replayed.getOrderBook(kSym);
    EXPECT_EQ(b.getOrder(1), nullptr);
    EXPECT_EQ(b.getOrder(2), nullptr);
    ASSERT_NE(b.getOrder(3), nullptr);
    EXPECT_EQ(b.getOrder(3)->remainingQty, 5u);
    EXPECT_EQ(b.getOrder(4), nullptr);
    std::remove(path.c_str());
}

}  // namespace
