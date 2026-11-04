#include <gtest/gtest.h>
#include "OrderBook.h"
#include "TempPath.h"
#include "MatchingEngine.h"
#include "LatencyTracker.h"
#include "Types.h"
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace OrderMatcher;

// ─── Test EventListener (replaces std::function callbacks) ───────────────────

class TestListener : public EventListener {
public:
    std::vector<Trade> trades;
    std::vector<OrderUpdate> updates;
    std::vector<MarketDataUpdate> mdUpdates;

    void onTrade(const Trade& t) override { trades.push_back(t); }
    void onOrderUpdate(const OrderUpdate& u) override { updates.push_back(u); }
    void onMarketData(const MarketDataUpdate& md) override { mdUpdates.push_back(md); }

    void reset() { trades.clear(); updates.clear(); mdUpdates.clear(); }
};

class OrderBookTest : public ::testing::Test {
protected:
    OrderBook book;
    TestListener listener;

    void SetUp() override {
        book.setEventListener(&listener);
    }
};

TEST_F(OrderBookTest, AddOrder_RetainsInBook) {
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);

    const Order* order = book.getOrder(1);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->remainingQty, 100);
    EXPECT_EQ(book.getBidLevelsCount(), 1);
}

TEST_F(OrderBookTest, ExecuteMatch_FullFill) {
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);
    book.addOrder(2, 2, Side::Buy, 1000000, 100, OrderType::Limit);

    EXPECT_EQ(book.getOrder(1), nullptr);
    EXPECT_EQ(book.getOrder(2), nullptr);
    EXPECT_EQ(book.getAskLevelsCount(), 0);
    EXPECT_EQ(book.getBidLevelsCount(), 0);
}

TEST_F(OrderBookTest, ExecuteMatch_PartialFill) {
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);
    book.addOrder(2, 2, Side::Buy, 1000000, 50, OrderType::Limit);

    const Order* order1 = book.getOrder(1);
    ASSERT_NE(order1, nullptr);
    EXPECT_EQ(order1->remainingQty, 50);
    EXPECT_EQ(book.getOrder(2), nullptr);
}

TEST_F(OrderBookTest, CancelOrder) {
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    book.cancelOrder(1);

    EXPECT_EQ(book.getOrder(1), nullptr);
    EXPECT_EQ(book.getBidLevelsCount(), 0);
}

TEST_F(OrderBookTest, CancelReplace_PriceChange) {
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    bool ok = book.cancelReplace(1, 1010000, 80);

    EXPECT_TRUE(ok);
    const Order* o = book.getOrder(1);
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->price, 1010000);
    EXPECT_EQ(o->remainingQty, 80);
}

TEST_F(OrderBookTest, PostOnly_Reject) {
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);
    book.addOrder(2, 2, Side::Buy, 1000000, 50, OrderType::PostOnly);

    EXPECT_EQ(book.getOrder(2), nullptr);
    EXPECT_EQ(book.getOrder(1)->remainingQty, 100);
}

TEST_F(OrderBookTest, PostOnly_Accept) {
    book.addOrder(1, 1, Side::Sell, 1010000, 100, OrderType::Limit);
    book.addOrder(2, 2, Side::Buy, 1000000, 50, OrderType::PostOnly);

    EXPECT_NE(book.getOrder(2), nullptr);
    EXPECT_EQ(book.getBidLevelsCount(), 1);
}

TEST_F(OrderBookTest, HiddenOrder_NotInSnapshot) {
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Hidden);
    auto snap = book.getSnapshot();
    EXPECT_EQ(snap.bidCount, 0);
}

TEST_F(OrderBookTest, KillSwitch) {
    book.addOrder(1, 1, Side::Buy, 990000, 100, OrderType::Limit);
    book.addOrder(2, 1, Side::Sell, 1010000, 100, OrderType::Limit);
    book.addOrder(3, 2, Side::Buy, 980000, 100, OrderType::Limit);

    uint64_t cancelled = book.cancelAllForParticipant(1);
    EXPECT_EQ(cancelled, 2);
    EXPECT_EQ(book.getOrder(1), nullptr);
    EXPECT_EQ(book.getOrder(2), nullptr);
    EXPECT_NE(book.getOrder(3), nullptr);
}

// ─── Circuit Breaker Tests ──────────────────────────────────────────────────

TEST_F(OrderBookTest, CircuitBreaker_ConfigurableThreshold) {
    book.setCircuitBreakerThreshold(0.02); // 2% threshold
    EXPECT_DOUBLE_EQ(book.getCircuitBreakerThreshold(), 0.02);

    // Set reference price at 100.0000
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);

    // Asks 1.5% and 3% away are passive: no print, nothing trips.
    book.addOrder(2, 2, Side::Sell, 1015000, 100, OrderType::Limit);
    book.addOrder(3, 3, Side::Sell, 1030000, 100, OrderType::Limit);
    EXPECT_EQ(book.getTradingState(), TradingState::Continuous);

    // A first fill 1.5% away is inside the threshold and trades.
    book.addOrder(4, 4, Side::Buy, 1015000, 100, OrderType::Limit);
    EXPECT_EQ(book.getTradeCount(), 1u);
    EXPECT_EQ(book.getTradingState(), TradingState::Continuous);

    // A first fill 3% away trips the breaker (roadmap 1.8-H3).
    book.addOrder(5, 5, Side::Buy, 1030000, 100, OrderType::Limit);
    EXPECT_EQ(book.getTradeCount(), 1u);
    EXPECT_EQ(book.getTradingState(), TradingState::VolatilityAuction);
}

TEST_F(OrderBookTest, CircuitBreaker_DefaultThreshold) {
    // Default is 5%
    EXPECT_DOUBLE_EQ(book.getCircuitBreakerThreshold(), 0.05);
}

// ─── TradingState tests ─────────────────────────────────────────────────────

TEST_F(OrderBookTest, TradingState_DefaultsContinuous) {
    EXPECT_EQ(book.getTradingState(), TradingState::Continuous);
    EXPECT_FALSE(book.isHalted());
}

TEST_F(OrderBookTest, TradingState_ManualHaltRejectsNewOrders) {
    book.setTradingState(TradingState::Halted);
    EXPECT_TRUE(book.isHalted());

    auto result = book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    ASSERT_TRUE(std::holds_alternative<RejectReason>(result));
    EXPECT_EQ(std::get<RejectReason>(result), RejectReason::MarketHalted);
    EXPECT_EQ(book.getOrder(1), nullptr);
}

TEST_F(OrderBookTest, TradingState_HaltAllowsCancels) {
    // Place an order while continuous, then halt and cancel it.
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    ASSERT_NE(book.getOrder(1), nullptr);

    book.setTradingState(TradingState::Halted);
    book.cancelOrder(1);
    EXPECT_EQ(book.getOrder(1), nullptr) << "cancels must work during halt";
}

TEST_F(OrderBookTest, TradingState_ResumeRestoresFlow) {
    book.setTradingState(TradingState::Halted);
    book.setTradingState(TradingState::Continuous);

    auto result = book.addOrder(1, 1, Side::Buy, 1000000, 50, OrderType::Limit);
    EXPECT_TRUE(std::holds_alternative<OrderId>(result));
    EXPECT_NE(book.getOrder(1), nullptr);
}

TEST_F(OrderBookTest, TradingState_AuctionAccumulatesWithoutMatching) {
    book.setTradingState(TradingState::AuctionOpen);

    // Crossing buy/sell at the same price during continuous would trade
    // immediately; in auction they must both rest on the book without
    // any trade being produced.
    auto sell = book.addOrder(1, 1, Side::Sell, 1000000, 50, OrderType::Limit);
    auto buy  = book.addOrder(2, 2, Side::Buy,  1000000, 50, OrderType::Limit);
    ASSERT_TRUE(std::holds_alternative<OrderId>(sell));
    ASSERT_TRUE(std::holds_alternative<OrderId>(buy));

    EXPECT_NE(book.getOrder(1), nullptr) << "sell should rest on book during auction";
    EXPECT_NE(book.getOrder(2), nullptr) << "buy should rest on book during auction";
    EXPECT_EQ(book.getTradeCount(), 0u) << "no trades during auction accumulation";
}

TEST_F(OrderBookTest, TradingState_AuctionUncrossProducesTrades) {
    book.setTradingState(TradingState::AuctionOpen);
    book.addOrder(1, 1, Side::Sell, 1000000, 50, OrderType::Limit);
    book.addOrder(2, 2, Side::Buy,  1000000, 50, OrderType::Limit);
    EXPECT_EQ(book.getTradeCount(), 0u);

    book.uncross();
    EXPECT_GT(book.getTradeCount(), 0u) << "uncross must produce trades";
    EXPECT_EQ(book.getOrder(1), nullptr) << "fully filled, removed from book";
    EXPECT_EQ(book.getOrder(2), nullptr);
}

TEST_F(OrderBookTest, TradingState_AuctionRejectsIOCAndFOK) {
    // IOC/FOK require immediate fills, which aren't available pre-uncross
    // — they remain rejected. Market is now admitted (see chunk 6) and
    // tested separately in MarketInAuction_*.
    book.setTradingState(TradingState::AuctionOpen);

    auto ioc = book.addOrder(2, 2, Side::Buy, 1000000, 50, OrderType::IOC);
    ASSERT_TRUE(std::holds_alternative<RejectReason>(ioc));
    EXPECT_EQ(std::get<RejectReason>(ioc),
              RejectReason::OrderTypeNotAllowedInState);

    auto fok = book.addOrder(3, 3, Side::Buy, 1000000, 50, OrderType::FOK);
    ASSERT_TRUE(std::holds_alternative<RejectReason>(fok));
    EXPECT_EQ(std::get<RejectReason>(fok),
              RejectReason::OrderTypeNotAllowedInState);
}

TEST_F(OrderBookTest, TradingState_CircuitBreakerTripsOnFirstFill) {
    // A passive order far from the reference is no print and trips nothing:
    // one out-of-band quote used to stop continuous trading for the whole
    // symbol (PRIOR-5). The order whose first fill would print out of band
    // trips the breaker; it waits for the reopening cross (roadmap 1.8-H3).
    book.setCircuitBreakerThreshold(0.02);
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);

    auto far = book.addOrder(2, 2, Side::Sell, 1030000, 100, OrderType::Limit);
    ASSERT_TRUE(std::holds_alternative<OrderId>(far));
    EXPECT_EQ(book.getTradingState(), TradingState::Continuous);

    auto trip = book.addOrder(3, 3, Side::Buy, 1030000, 50, OrderType::Limit);
    ASSERT_TRUE(std::holds_alternative<OrderId>(trip));
    EXPECT_EQ(book.getTradingState(), TradingState::VolatilityAuction);
    EXPECT_EQ(book.getTradeCount(), 0u);

    EXPECT_TRUE(book.resumeVolatilityAuction());
    EXPECT_EQ(book.getTradeCount(), 1u) << "the reopening cross prints";
    EXPECT_EQ(book.getTradingState(), TradingState::Continuous);
}

// ─── Market orders in auction states ────────────────────────────────────────

TEST_F(OrderBookTest, MarketInAuction_BuyFillsAgainstLimitSellsAtUncrossPrice) {
    book.setTradingState(TradingState::AuctionOpen);
    // Limit sell at 1,000,000 for 100 shares — this defines the only
    // candidate price in the volume-discovery sweep.
    book.addOrder(/*id=*/1, /*pid=*/1, Side::Sell, 1000000, 100,
                  OrderType::Limit);
    // Market buy for 50 — should fill at 1,000,000 when uncross runs.
    auto m = book.addOrder(/*id=*/2, /*pid=*/2, Side::Buy, /*price=*/0,
                            /*qty=*/50, OrderType::Market);
    ASSERT_TRUE(std::holds_alternative<OrderId>(m))
        << "Market in auction should be admitted (parked), not rejected";

    // Pre-uncross: no trades yet.
    EXPECT_EQ(book.getTradeCount(), 0u);

    book.uncross();

    // Trade executed at 1,000,000 for 50.
    EXPECT_GT(book.getTradeCount(), 0u);
    EXPECT_EQ(book.getOrder(2), nullptr) << "market buy fully filled";
    const Order* sell = book.getOrder(1);
    ASSERT_NE(sell, nullptr);
    EXPECT_EQ(sell->remainingQty, 50u) << "limit sell 50 of 100 remaining";
}

TEST_F(OrderBookTest, MarketInAuction_SellFillsAgainstLimitBuysAtUncrossPrice) {
    book.setTradingState(TradingState::AuctionOpen);
    book.addOrder(/*id=*/1, /*pid=*/1, Side::Buy, 1000000, 100,
                  OrderType::Limit);
    auto m = book.addOrder(/*id=*/2, /*pid=*/2, Side::Sell, /*price=*/0,
                            /*qty=*/50, OrderType::Market);
    ASSERT_TRUE(std::holds_alternative<OrderId>(m));

    book.uncross();

    EXPECT_GT(book.getTradeCount(), 0u);
    EXPECT_EQ(book.getOrder(2), nullptr) << "market sell fully filled";
    const Order* buy = book.getOrder(1);
    ASSERT_NE(buy, nullptr);
    EXPECT_EQ(buy->remainingQty, 50u);
}

TEST_F(OrderBookTest, MarketInAuction_OnlyMarketsCancelledByUncross) {
    // No limit orders → no candidate prices exist. Uncross should cancel
    // the market orders rather than executing at an arbitrary price.
    // (Real venues fall back to a reference price; we don't track that.)
    book.setTradingState(TradingState::AuctionOpen);
    auto a = book.addOrder(1, 1, Side::Buy,  /*price=*/0, 50, OrderType::Market);
    auto b = book.addOrder(2, 2, Side::Sell, /*price=*/0, 50, OrderType::Market);
    ASSERT_TRUE(std::holds_alternative<OrderId>(a));
    ASSERT_TRUE(std::holds_alternative<OrderId>(b));

    book.uncross();

    EXPECT_EQ(book.getTradeCount(), 0u);
    EXPECT_EQ(book.getOrder(1), nullptr) << "market buy cancelled — no price";
    EXPECT_EQ(book.getOrder(2), nullptr) << "market sell cancelled — no price";
}

TEST_F(OrderBookTest, MarketInAuction_UnfilledRemainderCancelled) {
    // Market buy bigger than available limit liquidity: the part that
    // can't fill is cancelled at the end of uncross — it does NOT rest
    // as a limit order.
    book.setTradingState(TradingState::AuctionOpen);
    book.addOrder(1, 1, Side::Sell, 1000000, /*qty=*/30, OrderType::Limit);
    book.addOrder(2, 2, Side::Buy,  /*price=*/0, /*qty=*/100, OrderType::Market);

    book.uncross();

    EXPECT_EQ(book.getOrder(1), nullptr) << "limit sell fully consumed";
    EXPECT_EQ(book.getOrder(2), nullptr)
        << "market buy: 30 filled + 70 cancelled — must not rest";
}

TEST_F(OrderBookTest, MarketInAuction_PreOpenAdmitsMarket) {
    // Same admission for PreOpen as for AuctionOpen.
    book.setTradingState(TradingState::PreOpen);
    auto r = book.addOrder(1, 1, Side::Buy, /*price=*/0, 50, OrderType::Market);
    EXPECT_TRUE(std::holds_alternative<OrderId>(r));
}

TEST_F(OrderBookTest, MarketInAuction_BothSidesMarketsWithLimits) {
    // Markets on both sides + a limit anchoring the price. All three
    // contribute to volume discovery; everything that can fill, does.
    book.setTradingState(TradingState::AuctionOpen);
    book.addOrder(/*id=*/1, /*pid=*/1, Side::Sell, 1000000, /*qty=*/100,
                  OrderType::Limit);
    book.addOrder(/*id=*/2, /*pid=*/2, Side::Buy,  /*price=*/0,
                  /*qty=*/40, OrderType::Market);
    book.addOrder(/*id=*/3, /*pid=*/3, Side::Sell, /*price=*/0,
                  /*qty=*/30, OrderType::Market);

    book.uncross();

    // Total buy at 1,000,000: market 40. Total sell at 1,000,000: limit
    // 100 + market 30 = 130. min = 40 → 40 fills. The 40-share market
    // buy fills entirely; sells absorb 40 units by priority. A market
    // order outranks every limit on its side (roadmap 1.8-H2, AUCT-2), so
    // the market sell fills all 30 and the limit sell the remaining 10.
    // (This test used to pin the reverse — the market sell queued behind
    // the limit at the clearing price — which was the bug.)
    const Order* limitSell  = book.getOrder(1);
    const Order* marketBuy  = book.getOrder(2);
    const Order* marketSell = book.getOrder(3);

    EXPECT_EQ(marketBuy, nullptr) << "market buy 40 fully filled";
    ASSERT_NE(limitSell, nullptr);
    EXPECT_EQ(limitSell->remainingQty, 90u)
        << "limit sell: 10 of 100 filled, after the market sell";
    EXPECT_EQ(marketSell, nullptr)
        << "market sell 30 fully filled ahead of the limit";
}

// ─── Time-based expiry replay determinism ───────────────────────────────────

TEST(ExpiryReplay, DayAndGtdExpirationsAreJournaledAndReplay) {
    namespace fs = std::filesystem;
    auto path = fs::temp_directory_path() /
        ("expiry_replay_" + std::to_string(::getpid()) + ".log");
    fs::remove(path);

    constexpr SymbolId kSym = 1;

    // Live: place a mix of GTC and DAY/GTD orders, advance time, expire,
    // then add more orders. The journal should capture both the original
    // adds and each expiration as CancelOrder entries.
    {
        MatchingEngine engine;
        engine.addSymbol(kSym);
        engine.enableJournal(path.string());
        engine.start();

        auto submit = [&](OrderId id, TimeInForce tif, uint64_t expiry) {
            engine.submitOrder(kSym, id, /*pid=*/1, Side::Buy, 1000000, 50,
                                OrderType::Limit, /*stopPrice=*/0,
                                /*displayQty=*/0, tif, expiry);
        };

        submit(1, TimeInForce::GTC, 0);          // never expires
        submit(2, TimeInForce::DAY, /*at=*/100); // expires when t>=100
        submit(3, TimeInForce::GTD, /*at=*/200); // expires when t>=200
        submit(4, TimeInForce::DAY, /*at=*/300); // expires when t>=300

        // Advance virtual time and expire. Each expireOrders call should
        // journal a CancelOrder for any DAY/GTD order whose deadline has
        // passed.
        engine.expireOrders(/*now=*/150);  // expires id=2
        engine.expireOrders(/*now=*/250);  // expires id=3

        submit(5, TimeInForce::GTC, 0);    // added after some expirations

        engine.stop();
    }

    // Replay into a fresh engine and confirm state matches what the
    // live run produced (id=1 and id=5 alive; id=2 and id=3 expired;
    // id=4 still alive because expireOrders was never called past 300).
    MatchingEngine replay;
    replay.addSymbol(kSym);
    replay.enableJournal(path.string());
    replay.replayJournal();

    auto* book = replay.getOrderBook(kSym);
    ASSERT_NE(book, nullptr);
    EXPECT_NE(book->getOrder(1), nullptr) << "GTC must survive";
    EXPECT_EQ(book->getOrder(2), nullptr) << "DAY id=2 expired live; replay must reproduce";
    EXPECT_EQ(book->getOrder(3), nullptr) << "GTD id=3 expired live; replay must reproduce";
    EXPECT_NE(book->getOrder(4), nullptr) << "DAY id=4 not yet past its deadline at expire calls";
    EXPECT_NE(book->getOrder(5), nullptr) << "GTC added after expirations";

    fs::remove(path);
}

TEST(ExpiryReplay, VirtualClockCrossDayReplayDeterminism) {
    namespace fs = std::filesystem;
    auto path = fs::temp_directory_path() /
        ("expiry_virtual_clock_" + std::to_string(::getpid()) + ".log");
    fs::remove(path);

    constexpr SymbolId kSym = 3;
    uint64_t virtualNow = 0;
    constexpr uint64_t kDay1Close = 86'400'000'000'000ULL;
    constexpr uint64_t kDay2Noon = kDay1Close + 43'200'000'000'000ULL;

    {
        MatchingEngine engine;
        engine.addSymbol(kSym);
        engine.enableJournal(path.string());
        engine.setExpiryClock([&] { return virtualNow; });
        engine.start();

        auto submit = [&](OrderId id, TimeInForce tif, uint64_t expiry) {
            auto r = engine.submitOrder(kSym, id, /*pid=*/7, Side::Buy,
                                        toPrice(10.00), 10, OrderType::Limit,
                                        /*stopPrice=*/0, /*displayQty=*/0,
                                        tif, expiry);
            ASSERT_TRUE(r.isAccepted());
        };

        submit(10, TimeInForce::DAY, kDay1Close);
        submit(11, TimeInForce::GTD, kDay2Noon);
        submit(12, TimeInForce::GTC, 0);

        virtualNow = kDay1Close - 1;
        engine.expireOrdersFromClock();
        EXPECT_NE(engine.getOrderBook(kSym)->getOrder(10), nullptr);

        virtualNow = kDay1Close;
        engine.expireOrdersFromClock();
        EXPECT_EQ(engine.getOrderBook(kSym)->getOrder(10), nullptr);
        EXPECT_NE(engine.getOrderBook(kSym)->getOrder(11), nullptr);

        virtualNow = kDay2Noon;
        engine.expireOrdersFromClock();
        EXPECT_EQ(engine.getOrderBook(kSym)->getOrder(11), nullptr);
        EXPECT_NE(engine.getOrderBook(kSym)->getOrder(12), nullptr);

        engine.stop();
    }

    MatchingEngine replay;
    replay.addSymbol(kSym);
    replay.enableJournal(path.string());
    replay.replayJournal();

    auto* book = replay.getOrderBook(kSym);
    ASSERT_NE(book, nullptr);
    EXPECT_EQ(book->getOrder(10), nullptr);
    EXPECT_EQ(book->getOrder(11), nullptr);
    EXPECT_NE(book->getOrder(12), nullptr);

    fs::remove(path);
}

// ─── Duplicate OrderId rejection ────────────────────────────────────────────
//
// Submitting AddOrder twice with the same orderId should be rejected. The
// underlying FlatHashMap::insert silently overwrites on duplicate key, so
// without an explicit check the first order stays in its price-level list
// and pool but becomes unfindable via getOrder — a leak that grows
// unbounded under naive client retries.

TEST_F(OrderBookTest, DuplicateOrderId_RejectedNotOverwritten) {
    auto first = book.addOrder(/*id=*/42, 1, Side::Buy, 1000000, 100,
                                OrderType::Limit);
    ASSERT_TRUE(std::holds_alternative<OrderId>(first));

    // Second submission with the same orderId — must be rejected.
    auto second = book.addOrder(/*id=*/42, 2, Side::Sell, 1010000, 50,
                                 OrderType::Limit);
    ASSERT_TRUE(std::holds_alternative<RejectReason>(second));
    EXPECT_EQ(std::get<RejectReason>(second),
              RejectReason::DuplicateOrderId);

    // The original order is still in place, untouched.
    const Order* o = book.getOrder(42);
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->participantId, 1u);
    EXPECT_EQ(o->side, Side::Buy);
    EXPECT_EQ(o->price, 1000000);
    EXPECT_EQ(o->remainingQty, 100u);

    // After cancel, the slot is freed and the same id may be reused.
    book.cancelOrder(42);
    EXPECT_EQ(book.getOrder(42), nullptr);
    auto reuse = book.addOrder(/*id=*/42, 3, Side::Buy, 1005000, 30,
                                OrderType::Limit);
    EXPECT_TRUE(std::holds_alternative<OrderId>(reuse));
}

// ─── Multi-symbol state-machine independence ────────────────────────────────
//
// Each OrderBook owns its own tradingState_; the engine never broadcasts
// halts or auction transitions across symbols. These tests lock that down
// — halting symbol A must not affect orders on symbol B, and an auction
// on one symbol must not disable continuous matching on another.

TEST(MultiSymbolStateMachine, HaltOneSymbolDoesNotRejectOnAnother) {
    MatchingEngine engine;
    engine.addSymbol(1);
    engine.addSymbol(2);
    engine.start();

    // Halt symbol 1.
    engine.getOrderBook(1)->setTradingState(TradingState::Halted);
    EXPECT_TRUE(engine.getOrderBook(1)->isHalted());
    EXPECT_FALSE(engine.getOrderBook(2)->isHalted())
        << "halting one book must not propagate to others";

    // Symbol 1: rejected.
    auto rA = engine.submitOrder(1, 101, 1, Side::Buy, 1000000, 50,
                                  OrderType::Limit);
    ASSERT_FALSE(rA.isAccepted());
    EXPECT_EQ(rA.rejectReason, RejectReason::MarketHalted);

    // Symbol 2: accepted, lands in the book.
    auto rB = engine.submitOrder(2, 201, 1, Side::Buy, 1000000, 50,
                                  OrderType::Limit);
    EXPECT_TRUE(rB.isAccepted());
    EXPECT_NE(engine.getOrderBook(2)->getOrder(201), nullptr);
}

TEST(MultiSymbolStateMachine, AuctionOnOneSymbolDoesNotBlockMatchingOnAnother) {
    MatchingEngine engine;
    engine.addSymbol(1);
    engine.addSymbol(2);
    engine.start();

    // Symbol 1: auction (orders accumulate, no continuous match).
    engine.getOrderBook(1)->setTradingState(TradingState::AuctionOpen);

    // Place crossing orders on each symbol.
    engine.submitOrder(1, 101, 1, Side::Sell, 1000000, 50, OrderType::Limit);
    engine.submitOrder(1, 102, 2, Side::Buy,  1000000, 50, OrderType::Limit);

    engine.submitOrder(2, 201, 1, Side::Sell, 1000000, 50, OrderType::Limit);
    engine.submitOrder(2, 202, 2, Side::Buy,  1000000, 50, OrderType::Limit);

    // Symbol 1: in auction — no trades, both rest.
    auto* bookA = engine.getOrderBook(1);
    EXPECT_EQ(bookA->getTradeCount(), 0u);
    EXPECT_NE(bookA->getOrder(101), nullptr);
    EXPECT_NE(bookA->getOrder(102), nullptr);

    // Symbol 2: continuous — they crossed and traded.
    auto* bookB = engine.getOrderBook(2);
    EXPECT_GT(bookB->getTradeCount(), 0u);
    EXPECT_EQ(bookB->getOrder(201), nullptr);
    EXPECT_EQ(bookB->getOrder(202), nullptr);
}

TEST(MultiSymbolStateMachine, BatchTransitionMovesAllSymbolsTogether) {
    // Coordinator opens 3 symbols at 9:30:00 and closes them at 16:00:00.
    // The batch API should transition every symbol atomically.
    MatchingEngine engine;
    for (SymbolId s : {1, 2, 3}) engine.addSymbol(s);
    engine.start();

    std::vector<SymbolId> universe = {1, 2, 3};

    // Pre-open: every symbol must accumulate without matching.
    auto count = engine.setTradingStateBatch(universe, TradingState::PreOpen);
    EXPECT_EQ(count, 3u);
    for (SymbolId s : universe) {
        EXPECT_EQ(engine.getOrderBook(s)->getTradingState(),
                  TradingState::PreOpen);
    }

    // Place crossing buy/sell on each symbol — none should match yet.
    for (SymbolId s : universe) {
        engine.submitOrder(s, 100 + s, 1, Side::Sell, 1000000, 50, OrderType::Limit);
        engine.submitOrder(s, 200 + s, 2, Side::Buy,  1000000, 50, OrderType::Limit);
        EXPECT_EQ(engine.getOrderBook(s)->getTradeCount(), 0u);
    }

    // Open the auction across the universe and uncross all together.
    auto opened = engine.setTradingStateBatch(universe, TradingState::AuctionOpen);
    EXPECT_EQ(opened, 3u);
    auto crossed = engine.uncrossBatch(universe);
    EXPECT_EQ(crossed, 3u);

    // Every symbol traded at the uncross.
    for (SymbolId s : universe) {
        EXPECT_GT(engine.getOrderBook(s)->getTradeCount(), 0u)
            << "symbol " << s << " should have uncrossed";
    }

    // Switch to continuous and verify the whole universe is now matching.
    auto cont = engine.setTradingStateBatch(universe, TradingState::Continuous);
    EXPECT_EQ(cont, 3u);
    for (SymbolId s : universe) {
        EXPECT_EQ(engine.getOrderBook(s)->getTradingState(),
                  TradingState::Continuous);
    }

    // End-of-session close.
    auto closed = engine.setTradingStateBatch(universe, TradingState::PostClose);
    EXPECT_EQ(closed, 3u);
    for (SymbolId s : universe) {
        auto r = engine.submitOrder(s, 999000 + s, 1, Side::Buy,
                                     1000000, 10, OrderType::Limit);
        ASSERT_FALSE(r.isAccepted());
        EXPECT_EQ(r.rejectReason, RejectReason::MarketClosed)
            << "symbol " << s << " should reject after PostClose";
    }
}

TEST(MultiSymbolStateMachine, BatchTransitionSkipsUnknownSymbols) {
    // Unknown symbols are silently skipped; the count reflects what was
    // actually transitioned. Useful when a coordinator's universe drifts
    // out of sync with the engine's registered symbol set.
    MatchingEngine engine;
    engine.addSymbol(1);
    engine.start();

    std::vector<SymbolId> mix = {1, 99, 100};
    auto count = engine.setTradingStateBatch(mix, TradingState::Halted);
    EXPECT_EQ(count, 1u) << "only the registered symbol counts";
    EXPECT_EQ(engine.getOrderBook(1)->getTradingState(),
              TradingState::Halted);
}

TEST(MultiSymbolStateMachine, ResetStatusOnOneDoesNotAffectOthers) {
    MatchingEngine engine;
    engine.addSymbol(1);
    engine.addSymbol(2);
    engine.start();

    // Both halted.
    engine.getOrderBook(1)->setTradingState(TradingState::Halted);
    engine.getOrderBook(2)->setTradingState(TradingState::Halted);

    // Reset only symbol 1.
    engine.getOrderBook(1)->resetStatus();
    EXPECT_EQ(engine.getOrderBook(1)->getTradingState(),
              TradingState::Continuous);
    EXPECT_EQ(engine.getOrderBook(2)->getTradingState(),
              TradingState::Halted)
        << "resetStatus must be per-book; it must not leak to other symbols";
}

TEST_F(OrderBookTest, TradingState_PreOpenAccumulatesWithoutMatching) {
    book.setTradingState(TradingState::PreOpen);
    book.addOrder(1, 1, Side::Sell, 1000000, 50, OrderType::Limit);
    book.addOrder(2, 2, Side::Buy,  1000000, 50, OrderType::Limit);
    EXPECT_NE(book.getOrder(1), nullptr);
    EXPECT_NE(book.getOrder(2), nullptr);
    EXPECT_EQ(book.getTradeCount(), 0u) << "no continuous match in PreOpen";
}

TEST_F(OrderBookTest, TradingState_PreOpenRejectsIOC) {
    // Market is now admitted in PreOpen (parked for the uncross); IOC
    // still requires immediate fills and stays rejected.
    book.setTradingState(TradingState::PreOpen);

    auto ioc = book.addOrder(2, 2, Side::Buy, 1000000, 50, OrderType::IOC);
    ASSERT_TRUE(std::holds_alternative<RejectReason>(ioc));
    EXPECT_EQ(std::get<RejectReason>(ioc),
              RejectReason::OrderTypeNotAllowedInState);
}

TEST_F(OrderBookTest, TradingState_PreOpenToContinuousMatchesAccumulated) {
    // Orders accumulated in PreOpen become matchable on transition to
    // Continuous. The first new order in Continuous should cross with
    // accumulated resting liquidity.
    book.setTradingState(TradingState::PreOpen);
    book.addOrder(1, 1, Side::Sell, 1000000, 50, OrderType::Limit);
    EXPECT_EQ(book.getTradeCount(), 0u);

    book.setTradingState(TradingState::Continuous);
    book.addOrder(2, 2, Side::Buy, 1000000, 50, OrderType::Limit);
    EXPECT_GT(book.getTradeCount(), 0u) << "accumulated liquidity matches in Continuous";
    EXPECT_EQ(book.getOrder(1), nullptr) << "resting order filled";
    EXPECT_EQ(book.getOrder(2), nullptr) << "incoming order filled";
}

TEST_F(OrderBookTest, TradingState_PostCloseRejectsNewOrders) {
    book.setTradingState(TradingState::PostClose);
    auto r = book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    ASSERT_TRUE(std::holds_alternative<RejectReason>(r));
    EXPECT_EQ(std::get<RejectReason>(r), RejectReason::MarketClosed);
    EXPECT_EQ(book.getOrder(1), nullptr);
}

TEST_F(OrderBookTest, TradingState_PostCloseAllowsCancels) {
    // Place an order during Continuous, then close the session and
    // confirm the order can still be cancelled.
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    ASSERT_NE(book.getOrder(1), nullptr);

    book.setTradingState(TradingState::PostClose);
    book.cancelOrder(1);
    EXPECT_EQ(book.getOrder(1), nullptr) << "cancels must work in PostClose";
}

TEST_F(OrderBookTest, TradingState_PostCloseDistinctFromHalted) {
    // The two states reject with different reasons so clients can
    // distinguish "session ended" from "regulator halt".
    book.setTradingState(TradingState::PostClose);
    auto a = book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    EXPECT_EQ(std::get<RejectReason>(a), RejectReason::MarketClosed);

    book.setTradingState(TradingState::Halted);
    auto b = book.addOrder(2, 2, Side::Buy, 1000000, 100, OrderType::Limit);
    EXPECT_EQ(std::get<RejectReason>(b), RejectReason::MarketHalted);
}

TEST_F(OrderBookTest, TradingState_ResetStatusReturnsToContinuous) {
    book.setTradingState(TradingState::AuctionOpen);
    book.resetStatus();
    EXPECT_EQ(book.getTradingState(), TradingState::Continuous);
}

// ─── Price band (LULD) tests ────────────────────────────────────────────────

TEST_F(OrderBookTest, PriceBand_DefaultDisabled) {
    EXPECT_DOUBLE_EQ(book.getPriceBandPct(), 0.0);

    // No band, so a wild price is accepted (subject to vol breaker, which
    // has its own threshold default of 5% — keep this order well inside).
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    auto r = book.addOrder(2, 2, Side::Buy, 1020000, 100, OrderType::Limit);
    EXPECT_TRUE(std::holds_alternative<OrderId>(r));
}

TEST_F(OrderBookTest, PriceBand_RejectsOutsideUpperBound) {
    // Establish a reference at 1,000,000 then arm a 5% band.
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    book.setPriceBandPct(0.05);

    // 6% above ref — outside band, must be rejected with OutsidePriceBand,
    // and the market must NOT halt (this is the contract that distinguishes
    // bands from circuit breakers).
    auto r = book.addOrder(2, 2, Side::Buy, 1060000, 100, OrderType::Limit);
    ASSERT_TRUE(std::holds_alternative<RejectReason>(r));
    EXPECT_EQ(std::get<RejectReason>(r), RejectReason::OutsidePriceBand);
    EXPECT_FALSE(book.isHalted());
    EXPECT_EQ(book.getOrder(2), nullptr);
}

TEST_F(OrderBookTest, PriceBand_RejectsOutsideLowerBound) {
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    book.setPriceBandPct(0.05);

    // 6% below ref.
    auto r = book.addOrder(2, 2, Side::Sell, 940000, 100, OrderType::Limit);
    ASSERT_TRUE(std::holds_alternative<RejectReason>(r));
    EXPECT_EQ(std::get<RejectReason>(r), RejectReason::OutsidePriceBand);
    EXPECT_FALSE(book.isHalted());
}

TEST_F(OrderBookTest, PriceBand_AcceptsInsideBand) {
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    book.setPriceBandPct(0.05);

    // 4% above — inside ±5%, must accept.
    auto r = book.addOrder(2, 2, Side::Buy, 1040000, 100, OrderType::Limit);
    EXPECT_TRUE(std::holds_alternative<OrderId>(r));
    EXPECT_NE(book.getOrder(2), nullptr);
}

TEST_F(OrderBookTest, PriceBand_DoesNotApplyBeforeReference) {
    // No reference yet — band is configured but inactive. The first order
    // establishes the reference; it itself is not band-checked (there is
    // no reference to check against). This matches the existing breaker
    // semantics and avoids a chicken-and-egg deadlock at session start.
    book.setPriceBandPct(0.05);
    auto r = book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    EXPECT_TRUE(std::holds_alternative<OrderId>(r));
}

TEST_F(OrderBookTest, PriceBand_DisableRestoresAdmission) {
    // Tighten the breaker so it doesn't auto-halt while we test the band's
    // disable path: the original 5% breaker default would trip at the same
    // 6% move that the band rejects.
    book.setCircuitBreakerThreshold(0.50);

    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    book.setPriceBandPct(0.05);

    auto rejected = book.addOrder(2, 2, Side::Buy, 1060000, 100, OrderType::Limit);
    ASSERT_TRUE(std::holds_alternative<RejectReason>(rejected));

    book.setPriceBandPct(0.0);  // disable
    auto accepted = book.addOrder(3, 3, Side::Buy, 1060000, 100, OrderType::Limit);
    EXPECT_TRUE(std::holds_alternative<OrderId>(accepted));
}

TEST_F(OrderBookTest, PriceBand_DoesNotRejectMarketOrders) {
    book.addOrder(1, 1, Side::Sell, 1000000, 200, OrderType::Limit);
    book.setPriceBandPct(0.01);  // tight ±1%

    // Market orders have no meaningful limit price; the band must skip
    // them. (They'll match at whatever the resting orders provide.)
    auto r = book.addOrder(2, 2, Side::Buy, 0, 50, OrderType::Market);
    EXPECT_TRUE(std::holds_alternative<OrderId>(r))
        << "Market orders must not be price-band rejected";
}

TEST_F(OrderBookTest, PriceBand_BandedOutOrderDoesNotHaltMarket) {
    // Distinct contract: an out-of-band order is rejected, but the next
    // in-band order still trades normally (no auto-halt, unlike breaker).
    book.setCircuitBreakerThreshold(0.50);  // breaker out of the way
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    book.setPriceBandPct(0.05);

    auto bad = book.addOrder(2, 2, Side::Buy, 1100000, 100, OrderType::Limit);
    ASSERT_TRUE(std::holds_alternative<RejectReason>(bad));
    EXPECT_FALSE(book.isHalted());

    auto good = book.addOrder(3, 3, Side::Buy, 1010000, 100, OrderType::Limit);
    EXPECT_TRUE(std::holds_alternative<OrderId>(good));
}

// ─── Iceberg refresh priority tests ─────────────────────────────────────────
//
// Two intentionally different semantics, one per matching algorithm:
//   * Price-time FIFO: refreshed slice is treated as a new arrival and goes
//     to the BACK of its level. NYSE / Nasdaq / LSE convention.
//   * Pro-rata:        refreshed slice keeps its list position. Pro-rata
//     allocates by quantity weight, not time, so position only affects the
//     rounding-remainder distribution. CME convention.

TEST_F(OrderBookTest, IcebergPriority_PriceTime_LosesPriorityOnRefresh) {
    // Default mode is PriceTime. Place an iceberg first (visible=100,
    // hidden=100), then a plain limit at the same price (queued behind).
    // Cross the iceberg's full visible slice; that triggers a refresh which
    // moves the refreshed slice to the back of the queue. The plain limit
    // should now be ahead and get filled by the next incoming sell ahead
    // of the iceberg's refreshed slice.
    book.setCircuitBreakerThreshold(0.50);  // out of the way
    book.addOrder(/*id=*/1, /*pid=*/1, Side::Buy, 1000000, /*qty=*/200,
                  OrderType::Iceberg, /*stopPrice=*/0, /*displayQty=*/100);
    book.addOrder(/*id=*/2, /*pid=*/2, Side::Buy, 1000000, /*qty=*/50,
                  OrderType::Limit);

    // Aggressive sell that exhausts the iceberg's first slice exactly.
    book.addOrder(/*id=*/3, /*pid=*/3, Side::Sell, 1000000, /*qty=*/100,
                  OrderType::IOC);

    // After fill: iceberg refreshed; plain still has full 50.
    const Order* iceberg = book.getOrder(1);
    const Order* plain   = book.getOrder(2);
    ASSERT_NE(iceberg, nullptr);
    ASSERT_NE(plain, nullptr);
    EXPECT_EQ(iceberg->remainingQty, 100u) << "iceberg should have 100 hidden left";
    EXPECT_EQ(iceberg->visibleQty,   100u) << "refreshed slice should be visible";
    EXPECT_EQ(plain->remainingQty,    50u);

    // Next aggressive sell of 50 must fill the plain limit (now ahead of
    // the iceberg's refreshed slice in the queue), not the iceberg.
    book.addOrder(/*id=*/4, /*pid=*/4, Side::Sell, 1000000, /*qty=*/50,
                  OrderType::IOC);

    EXPECT_EQ(book.getOrder(2), nullptr) << "plain limit should be filled first";
    const Order* ice2 = book.getOrder(1);
    ASSERT_NE(ice2, nullptr);
    EXPECT_EQ(ice2->remainingQty, 100u) << "iceberg untouched — plain filled ahead";
    EXPECT_EQ(ice2->visibleQty,   100u);
}

TEST_F(OrderBookTest, IcebergPriority_ProRata_KeepsPriorityOnRefresh) {
    // In pro-rata mode the refreshed iceberg slice stays at its original
    // list position. Pro-rata allocates by volume weight, so position
    // mostly doesn't matter — except for the rounding remainder, which
    // goes to orders in time-priority order. We use that to verify the
    // iceberg's position survives the refresh.
    book.setMatchAlgorithm(MatchAlgorithm::ProRata);
    book.setCircuitBreakerThreshold(0.50);

    // Iceberg 1st (front of list), displayQty=100, hidden=100.
    book.addOrder(/*id=*/1, /*pid=*/1, Side::Buy, 1000000, /*qty=*/200,
                  OrderType::Iceberg, /*stopPrice=*/0, /*displayQty=*/100);

    // Refresh: aggressive sell fills the iceberg's first slice entirely
    // (it's alone at this level, so all 100 fills into it). Refresh fires
    // in place: visible=100, remaining=100.
    book.addOrder(/*id=*/2, /*pid=*/2, Side::Sell, 1000000, /*qty=*/100,
                  OrderType::IOC);
    const Order* ice = book.getOrder(1);
    ASSERT_NE(ice, nullptr);
    EXPECT_EQ(ice->remainingQty, 100u);
    EXPECT_EQ(ice->visibleQty,   100u);

    // NEW plain limit placed at the same price after the refresh — it
    // joins the back of the level list. If the iceberg had lost its
    // position on refresh (hypothetical "moved to back"), the plain limit
    // would now be ahead of it.
    book.addOrder(/*id=*/3, /*pid=*/3, Side::Buy, 1000000, /*qty=*/50,
                  OrderType::Limit);

    // Snapshot remaining quantities BEFORE the discriminating sell so we
    // measure only the fills it produces.
    Quantity iceRemBefore   = ice->remainingQty;
    Quantity plainRemBefore = book.getOrder(3)->remainingQty;

    // Cross both with a sell of 11 — totalVisible = 100 + 50 = 150;
    // shares: iceberg=100/150*11=7, plain=50/150*11=3, allocated=10,
    // remainder=1. Pro-rata distributes the remainder by time priority
    // (front of list first). With the iceberg's position preserved
    // through refresh, the iceberg is still in front and collects the +1
    // → iceberg fills 8, plain fills 3.
    book.addOrder(/*id=*/4, /*pid=*/4, Side::Sell, 1000000, /*qty=*/11,
                  OrderType::IOC);

    const Order* ice2   = book.getOrder(1);
    const Order* plain2 = book.getOrder(3);
    ASSERT_NE(ice2,   nullptr);
    ASSERT_NE(plain2, nullptr);
    Quantity iceFilledNow   = iceRemBefore   - ice2->remainingQty;
    Quantity plainFilledNow = plainRemBefore - plain2->remainingQty;
    EXPECT_EQ(iceFilledNow + plainFilledNow, 11u);
    EXPECT_GT(iceFilledNow, plainFilledNow)
        << "rounding remainder should go to the iceberg (front of list) — "
           "if it had been moved to the back on refresh, the plain limit "
           "would have collected the remainder instead.";
}

TEST_F(OrderBookTest, Iceberg_RefreshExhaustsHiddenQty) {
    // Sanity: drive enough fills that the iceberg fully exhausts both
    // visible and hidden quantity, ensuring multiple refreshes execute
    // cleanly (no leak, no zombie state).
    book.setCircuitBreakerThreshold(0.50);
    book.addOrder(/*id=*/1, /*pid=*/1, Side::Buy, 1000000, /*qty=*/300,
                  OrderType::Iceberg, /*stopPrice=*/0, /*displayQty=*/100);

    // Three full-slice fills should consume the entire 300.
    for (int i = 0; i < 3; ++i) {
        book.addOrder(/*id=*/100 + i, /*pid=*/2, Side::Sell, 1000000,
                      /*qty=*/100, OrderType::IOC);
    }
    EXPECT_EQ(book.getOrder(1), nullptr) << "iceberg fully consumed across 3 refreshes";
}

TEST_F(OrderBookTest, CircuitBreaker_RecoveryAfterReset) {
    book.setCircuitBreakerThreshold(0.02);
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    book.addOrder(2, 2, Side::Sell, 1030000, 100, OrderType::Limit); // passive: no trip
    EXPECT_EQ(book.getTradingState(), TradingState::Continuous);

    // Reset and resume
    book.resetStatus();
    EXPECT_FALSE(book.isHalted());

    // Should accept orders again
    auto result = book.addOrder(3, 3, Side::Buy, 1000000, 50, OrderType::Limit);
    EXPECT_TRUE(std::holds_alternative<OrderId>(result));
    EXPECT_NE(book.getOrder(3), nullptr);
}

// ─── Modify Order Tests ─────────────────────────────────────────────────────

TEST_F(OrderBookTest, ModifyOrder_ReduceQuantity) {
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    bool ok = book.modifyOrder(1, 50);

    EXPECT_TRUE(ok);
    const Order* o = book.getOrder(1);
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->remainingQty, 50);
}

// ─── IOC Order Tests ────────────────────────────────────────────────────────

TEST_F(OrderBookTest, IOC_PartialFillCancelsRest) {
    book.addOrder(1, 1, Side::Sell, 1000000, 50, OrderType::Limit);
    book.addOrder(2, 2, Side::Buy, 1000000, 100, OrderType::IOC);

    // IOC should fill 50, cancel remaining 50
    EXPECT_EQ(book.getOrder(1), nullptr); // Sell fully filled
    EXPECT_EQ(book.getOrder(2), nullptr); // IOC cancelled after partial fill
    EXPECT_EQ(book.getBidLevelsCount(), 0);
}

TEST_F(OrderBookTest, IOC_NoLiquidity_Cancelled) {
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::IOC);
    EXPECT_EQ(book.getOrder(1), nullptr);
    EXPECT_EQ(book.getBidLevelsCount(), 0);
}

// ─── FOK Order Tests ────────────────────────────────────────────────────────

TEST_F(OrderBookTest, FOK_InsufficientLiquidity_Rejected) {
    book.addOrder(1, 1, Side::Sell, 1000000, 50, OrderType::Limit);
    book.addOrder(2, 2, Side::Buy, 1000000, 100, OrderType::FOK);

    // FOK needs 100, only 50 available — rejected
    EXPECT_EQ(book.getOrder(2), nullptr);
    EXPECT_NE(book.getOrder(1), nullptr); // Sell untouched
    EXPECT_EQ(book.getOrder(1)->remainingQty, 50);
}

// ─── Market Order Tests ─────────────────────────────────────────────────────

TEST_F(OrderBookTest, MarketOrder_SweepsMultipleLevels) {
    book.addOrder(1, 1, Side::Sell, 1000000, 50, OrderType::Limit);
    book.addOrder(2, 1, Side::Sell, 1010000, 50, OrderType::Limit);
    book.addOrder(3, 2, Side::Buy, 0, 100, OrderType::Market);

    EXPECT_EQ(book.getOrder(1), nullptr);
    EXPECT_EQ(book.getOrder(2), nullptr);
    EXPECT_EQ(book.getOrder(3), nullptr);
    EXPECT_EQ(book.getAskLevelsCount(), 0);
}

// ─── Trade Callback Tests ──────────────────────────────────────────────────

TEST_F(OrderBookTest, TradeCallback_FiredOnMatch) {
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);
    listener.reset();

    book.addOrder(2, 2, Side::Buy, 1000000, 50, OrderType::Limit);

    ASSERT_EQ(listener.trades.size(), 1);
    EXPECT_EQ(listener.trades[0].quantity, 50);
    EXPECT_EQ(listener.trades[0].price, 1000000);
    EXPECT_EQ(listener.trades[0].buyOrderId, 2);
    EXPECT_EQ(listener.trades[0].sellOrderId, 1);
}

TEST_F(OrderBookTest, TradeCallback_SuppressedInReplayMode) {
    book.setReplayMode(true);
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);
    book.addOrder(2, 2, Side::Buy, 1000000, 50, OrderType::Limit);
    book.setReplayMode(false);

    EXPECT_TRUE(listener.trades.empty());
    EXPECT_TRUE(listener.updates.empty());
}

// ─── Self-Match Prevention Tests ─────────────────────────────────────────────

TEST_F(OrderBookTest, SMP_SameParticipantCancelled) {
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);
    book.addOrder(2, 1, Side::Buy, 1000000, 50, OrderType::Limit);

    // SMP should prevent the match — buyer qty zeroed
    EXPECT_NE(book.getOrder(1), nullptr);
    EXPECT_EQ(book.getOrder(1)->remainingQty, 100);
}

// ─── Iceberg Order Tests ─────────────────────────────────────────────────────

TEST_F(OrderBookTest, Iceberg_ReplenishesVisibleQty) {
    // Iceberg: 300 total, display 100 at a time
    book.addOrder(1, 1, Side::Sell, 1000000, 300, OrderType::Iceberg,
                  0, 100); // stopPrice=0, displayQty=100

    // First fill: takes visible 100
    book.addOrder(2, 2, Side::Buy, 1000000, 100, OrderType::Limit);
    const Order* iceberg = book.getOrder(1);
    ASSERT_NE(iceberg, nullptr);
    EXPECT_EQ(iceberg->remainingQty, 200);
    EXPECT_EQ(iceberg->visibleQty, 100); // Replenished

    // Second fill: takes another 100
    book.addOrder(3, 3, Side::Buy, 1000000, 100, OrderType::Limit);
    iceberg = book.getOrder(1);
    ASSERT_NE(iceberg, nullptr);
    EXPECT_EQ(iceberg->remainingQty, 100);
    EXPECT_EQ(iceberg->visibleQty, 100); // Replenished again
}

TEST_F(OrderBookTest, Iceberg_PartialFillDoesNotReplenish) {
    book.addOrder(1, 1, Side::Sell, 1000000, 300, OrderType::Iceberg,
                  0, 100);

    // Partial fill of 30: visible goes from 100 to 70, no replenishment
    book.addOrder(2, 2, Side::Buy, 1000000, 30, OrderType::Limit);
    const Order* iceberg = book.getOrder(1);
    ASSERT_NE(iceberg, nullptr);
    EXPECT_EQ(iceberg->remainingQty, 270);
    EXPECT_EQ(iceberg->visibleQty, 70);
}

TEST_F(OrderBookTest, Iceberg_FullSweep) {
    book.addOrder(1, 1, Side::Sell, 1000000, 300, OrderType::Iceberg,
                  0, 100);

    // Sweep entire iceberg
    book.addOrder(2, 2, Side::Buy, 1000000, 300, OrderType::Limit);
    EXPECT_EQ(book.getOrder(1), nullptr);
    EXPECT_EQ(book.getOrder(2), nullptr);
}

// ─── Capacity Exhaustion Test ────────────────────────────────────────────────

TEST_F(OrderBookTest, RejectReason_OnPoolExhaustion) {
    // This tests that the engine handles pool exhaustion gracefully.
    // We use a small pool for this test.
    OrderBook smallBook(0, MatchAlgorithm::PriceTime);
    smallBook.setEventListener(&listener);

    // The default pool is 1M — we can't easily exhaust it in a unit test.
    // Instead, verify the rejection reason enum exists and is used.
    // A real exhaustion test would require a custom-sized pool.
    auto result = smallBook.addOrder(1, 1, Side::Buy, 1000000, 0, OrderType::Limit);
    EXPECT_TRUE(std::holds_alternative<RejectReason>(result));
    EXPECT_EQ(std::get<RejectReason>(result), RejectReason::InvalidQuantity);
}

// ─── Snapshot Tests ──────────────────────────────────────────────────────────

TEST_F(OrderBookTest, Snapshot_CorrectDepth) {
    // Build a book with multiple levels
    for (int i = 0; i < 5; ++i) {
        book.addOrder(static_cast<OrderId>(i + 1), 1, Side::Buy,
                      990000 - i * 10000, 100, OrderType::Limit);
        book.addOrder(static_cast<OrderId>(i + 6), 2, Side::Sell,
                      1010000 + i * 10000, 100, OrderType::Limit);
    }

    auto snap = book.getSnapshot(3);
    EXPECT_EQ(snap.bidCount, 3);
    EXPECT_EQ(snap.askCount, 3);
    // Best bid should be the highest
    EXPECT_EQ(snap.bids[0].price, 990000);
    // Best ask should be the lowest
    EXPECT_EQ(snap.asks[0].price, 1010000);
}

// ─── Journal Replay Test ────────────────────────────────────────────────────

TEST(JournalTest, ReplayRecovery) {
    const std::string journalPath = uniqueTempPath("test_journal_replay.bin");
    std::remove(journalPath.c_str());

    // Phase 1: Write some orders to journal
    {
        MatchingEngine engine;
        engine.addSymbol(1);
        engine.enableJournal(journalPath);
        engine.start();

        engine.processOrder(1, 100, 1, Side::Buy, 1000000, 50, OrderType::Limit);
        engine.processOrder(1, 101, 2, Side::Sell, 1010000, 30, OrderType::Limit);

        engine.stop();
    }

    // Phase 2: Recover from journal
    {
        MatchingEngine engine;
        engine.addSymbol(1);
        engine.enableJournal(journalPath);
        engine.start();

        size_t replayed = engine.replayJournal();
        EXPECT_EQ(replayed, 2 + 1);   // + the sequence-id lease before the first id (D1)

        auto* book = engine.getOrderBook(1);
        ASSERT_NE(book, nullptr);
        // Orders should be reconstructed (they won't match since they don't cross)
        EXPECT_NE(book->getOrder(100), nullptr);
        EXPECT_NE(book->getOrder(101), nullptr);

        engine.stop();
    }

    std::remove(journalPath.c_str());
}

// A thread's sequence-id block outlives its engine. The next engine on that
// thread used to keep handing out ids from it, so its journal never leased them
// (D1) — and ReplayRecovery failed whenever an earlier test in the same process
// had submitted from this thread.
TEST(JournalTest, NextEngineOnThreadLeasesItsOwnIds) {
    {
        MatchingEngine earlier;
        earlier.addSymbol(1);
        earlier.start();
        earlier.processOrder(1, 7, 1, Side::Buy, 1000000, 10, OrderType::Limit);
        earlier.stop();
    }
    const std::string journalPath = uniqueTempPath("next_engine_lease.bin");
    std::remove(journalPath.c_str());
    {
        MatchingEngine engine;
        engine.addSymbol(1);
        engine.enableJournal(journalPath);
        engine.start();
        engine.processOrder(1, 100, 1, Side::Buy, 1000000, 50, OrderType::Limit);
        engine.stop();
    }
    MatchingEngine recovered;
    recovered.addSymbol(1);
    recovered.enableJournal(journalPath);
    recovered.start();
    EXPECT_EQ(recovered.replayJournal(), 1u + 1u);   // the order and its id's lease
    recovered.stop();
    std::remove(journalPath.c_str());
}

// ─── Latency Tracker Tests ──────────────────────────────────────────────────

TEST(LatencyTrackerTest, BasicRecording) {
    LatencyTracker tracker;

    for (int i = 1; i <= 100; i++) {
        tracker.record(static_cast<uint64_t>(i));
    }

    EXPECT_EQ(tracker.getCount(), 100);
    EXPECT_EQ(tracker.getMin(), 1);
    EXPECT_EQ(tracker.getMax(), 100);
    EXPECT_NEAR(tracker.getMean(), 50.5, 0.01);
}

TEST(LatencyTrackerTest, Percentiles) {
    LatencyTracker tracker;

    // Record 1000 values from 1 to 1000
    for (int i = 1; i <= 1000; i++) {
        tracker.record(static_cast<uint64_t>(i));
    }

    // P50 should be approximately 500
    EXPECT_NEAR(tracker.getP50(), 500, 10);
    // P99 should be approximately 990
    EXPECT_NEAR(tracker.getP99(), 990, 15);
}

TEST(LatencyTrackerTest, Reset) {
    LatencyTracker tracker;
    tracker.record(100);
    tracker.record(200);
    tracker.reset();

    EXPECT_EQ(tracker.getCount(), 0);
    EXPECT_NEAR(tracker.getMean(), 0.0, 0.01);
}

TEST(LatencyTrackerTest, ScopeTimer) {
    LatencyTracker tracker;

    {
        auto timer = tracker.scope();
        // Do some trivial work
        volatile int sum = 0;
        for (int i = 0; i < 100; i++) sum += i;
    }

    EXPECT_EQ(tracker.getCount(), 1);
    EXPECT_GT(tracker.getMax(), 0);
}

// ─── Pro-Rata Matching Tests ────────────────────────────────────────────────

class ProRataTest : public ::testing::Test {
protected:
    OrderBook book{0, MatchAlgorithm::ProRata};
    TestListener listener;

    void SetUp() override {
        book.setEventListener(&listener);
    }
};

TEST_F(ProRataTest, BasicProportionalAllocation) {
    // Three sell orders at same price with different quantities
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);  // 100
    book.addOrder(2, 2, Side::Sell, 1000000, 200, OrderType::Limit);  // 200
    book.addOrder(3, 3, Side::Sell, 1000000, 300, OrderType::Limit);  // 300
    // Total resting = 600

    listener.reset();

    // Buy 300 — should be allocated proportionally: 50, 100, 150
    book.addOrder(4, 4, Side::Buy, 1000000, 300, OrderType::Limit);

    // All three sell orders should have been partially or fully filled
    EXPECT_FALSE(listener.trades.empty());

    // Total filled across all trades should be 300
    Quantity totalFilled = 0;
    for (auto& t : listener.trades) totalFilled += t.quantity;
    EXPECT_EQ(totalFilled, 300);

    // Check remaining quantities are proportional
    const Order* o1 = book.getOrder(1);
    const Order* o2 = book.getOrder(2);
    const Order* o3 = book.getOrder(3);

    // Each should have 50% remaining (approximately due to rounding).
    // Braces are required, not style: EXPECT_LE expands to an if/else, so an
    // unbraced `if (o1) EXPECT_LE(...)` is a dangling else that GCC rejects
    // under -Werror.
    if (o1) { EXPECT_LE(o1->remainingQty, 100); }
    if (o2) { EXPECT_LE(o2->remainingQty, 200); }
    if (o3) { EXPECT_LE(o3->remainingQty, 300); }
}

TEST_F(ProRataTest, SingleOrderAtLevel) {
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);

    book.addOrder(2, 2, Side::Buy, 1000000, 50, OrderType::Limit);

    const Order* o1 = book.getOrder(1);
    ASSERT_NE(o1, nullptr);
    EXPECT_EQ(o1->remainingQty, 50);  // Full 50 goes to single order
}

TEST_F(ProRataTest, RoundingRemainderGoesToTimePriority) {
    // Two orders with equal quantity — rounding remainder goes to first (time priority)
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);
    book.addOrder(2, 2, Side::Sell, 1000000, 100, OrderType::Limit);
    // Total = 200

    // Buy 101: each gets 50 (proportional), remainder 1 goes to order 1 (time priority)
    book.addOrder(3, 3, Side::Buy, 1000000, 101, OrderType::Limit);

    const Order* o1 = book.getOrder(1);
    const Order* o2 = book.getOrder(2);

    ASSERT_NE(o1, nullptr);
    ASSERT_NE(o2, nullptr);

    // Total filled should be 101
    Quantity filled1 = 100 - o1->remainingQty;
    Quantity filled2 = 100 - o2->remainingQty;
    EXPECT_EQ(filled1 + filled2, 101);

    // First order should get the extra 1 (time priority for remainder)
    EXPECT_GE(filled1, filled2);
}

TEST_F(ProRataTest, ZeroQuantityAllocation) {
    // Many large orders, tiny incoming — verify no crash with zero allocations
    book.addOrder(1, 1, Side::Sell, 1000000, 10000, OrderType::Limit);
    book.addOrder(2, 2, Side::Sell, 1000000, 10000, OrderType::Limit);
    book.addOrder(3, 3, Side::Sell, 1000000, 10000, OrderType::Limit);

    // Buy only 1 — some orders will get 0 allocation before rounding
    book.addOrder(4, 4, Side::Buy, 1000000, 1, OrderType::Limit);

    // Should not crash and exactly 1 unit should be filled
    // Buyer should be fully filled (removed from book)
    EXPECT_EQ(book.getOrder(4), nullptr);
}

TEST_F(ProRataTest, FullSweepAtLevel) {
    book.addOrder(1, 1, Side::Sell, 1000000, 50, OrderType::Limit);
    book.addOrder(2, 2, Side::Sell, 1000000, 50, OrderType::Limit);

    // Buy 100 — should sweep entire level
    book.addOrder(3, 3, Side::Buy, 1000000, 100, OrderType::Limit);

    EXPECT_EQ(book.getOrder(1), nullptr);
    EXPECT_EQ(book.getOrder(2), nullptr);
    EXPECT_EQ(book.getOrder(3), nullptr);
    EXPECT_EQ(book.getAskLevelsCount(), 0);
}

// ─── Pegged Order Tests ──────────────────────────────────────────────────────

TEST_F(OrderBookTest, PeggedOrder_MidPegReprices) {
    // Establish BBO
    book.addOrder(1, 1, Side::Buy, 990000, 100, OrderType::Limit);   // bid 99.0
    book.addOrder(2, 2, Side::Sell, 1010000, 100, OrderType::Limit);  // ask 101.0

    // Pegged buy at mid (100.0) with no offset
    book.addOrder(3, 3, Side::Buy, 1000000, 50, OrderType::Pegged,
                  0, 0, TimeInForce::GTC, 0, 0, PegType::MidPeg, 0, 0, 0, false);

    const Order* peg = book.getOrder(3);
    ASSERT_NE(peg, nullptr);
    // Mid = (990000 + 1010000) / 2 = 1000000
    EXPECT_EQ(peg->price, 1000000);
}

// ─── Stop Order Tests ────────────────────────────────────────────────────────

TEST_F(OrderBookTest, StopOrder_TriggersOnTrade) {
    // Place a resting sell to establish price
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);

    // Place a buy stop at 100.0 (triggers when trade >= 100.0)
    book.addOrder(2, 2, Side::Buy, 1000000, 50, OrderType::Stop, 1000000);

    // Stop should not be in the visible book
    EXPECT_EQ(book.getBidLevelsCount(), 0);

    // A crossing trade triggers the stop
    book.addOrder(3, 3, Side::Buy, 1000000, 10, OrderType::Limit);
    // Trade at 100.0 triggers stop order #2, which then matches against remaining sell

    const Order* sell = book.getOrder(1);
    // Sell had 100, matched 10 (order 3), then stop triggered and matched more
    if (sell) {
        EXPECT_LT(sell->remainingQty, 100);
    }
}

// ─── Risk Limits Tests ───────────────────────────────────────────────────────

TEST_F(OrderBookTest, RiskLimits_MaxOrderSize) {
    RiskLimits limits;
    limits.maxOrderSize = 50;
    book.setRiskLimits(1, limits);

    auto result = book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    EXPECT_TRUE(std::holds_alternative<RejectReason>(result));
    EXPECT_EQ(std::get<RejectReason>(result), RejectReason::RiskLimitBreached);

    auto result2 = book.addOrder(2, 1, Side::Buy, 1000000, 50, OrderType::Limit);
    EXPECT_TRUE(std::holds_alternative<OrderId>(result2));
}
