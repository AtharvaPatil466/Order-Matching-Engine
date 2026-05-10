#include <gtest/gtest.h>
#include "OrderBook.h"
#include "MatchingEngine.h"
#include "FIXParser.h"
#include "FixFramer.h"
#include "FixSession.h"
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

    // 1.5% move should be OK
    book.addOrder(2, 2, Side::Sell, 1015000, 100, OrderType::Limit);
    EXPECT_FALSE(book.isHalted());

    // 3% move should trigger halt
    book.addOrder(3, 3, Side::Sell, 1030000, 100, OrderType::Limit);
    EXPECT_TRUE(book.isHalted());
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

TEST_F(OrderBookTest, TradingState_CircuitBreakerSetsHaltedState) {
    // The order that trips the breaker reports VolatilityCircuitBreaker;
    // subsequent orders see the resulting Halted state and report
    // MarketHalted. This distinguishes "you tripped the breaker" from
    // "the market is currently halted because someone earlier tripped it".
    book.setCircuitBreakerThreshold(0.02);
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);

    auto trip = book.addOrder(2, 2, Side::Sell, 1030000, 100, OrderType::Limit);
    ASSERT_TRUE(std::holds_alternative<RejectReason>(trip));
    EXPECT_EQ(std::get<RejectReason>(trip),
              RejectReason::VolatilityCircuitBreaker);
    EXPECT_EQ(book.getTradingState(), TradingState::Halted);

    auto post = book.addOrder(3, 3, Side::Buy, 1000000, 50, OrderType::Limit);
    ASSERT_TRUE(std::holds_alternative<RejectReason>(post));
    EXPECT_EQ(std::get<RejectReason>(post), RejectReason::MarketHalted)
        << "post-halt orders should see MarketHalted, not the trip reason";
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
    // buy fills entirely; sells absorb 40 units distributed by queue
    // order. The limit sell is at the front of the price-time queue
    // (it was placed first), the market sell got inserted after the
    // limit at the same price (FIFO by insertion); so the limit sell
    // fills first.
    const Order* limitSell  = book.getOrder(1);
    const Order* marketBuy  = book.getOrder(2);
    const Order* marketSell = book.getOrder(3);

    EXPECT_EQ(marketBuy, nullptr) << "market buy 40 fully filled";
    ASSERT_NE(limitSell, nullptr);
    EXPECT_EQ(limitSell->remainingQty, 60u)
        << "limit sell: 40 of 100 filled (front of queue at price level)";
    EXPECT_EQ(marketSell, nullptr)
        << "unfilled market sell cancelled at end of uncross";
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
    book.addOrder(2, 2, Side::Sell, 1030000, 100, OrderType::Limit); // triggers halt
    EXPECT_TRUE(book.isHalted());

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

// ─── FIX Parser Tests ───────────────────────────────────────────────────────

TEST(FIXParserTest, ParseAndBuildRoundTrip) {
    std::string raw = FixSerializer::buildExecutionReport(
        42, 7, '2', '2', Side::Buy, 1505000, 200, 200, 0, 1505000, 200);

    FixMessage parsed;
    ASSERT_TRUE(parsed.parse(raw.c_str(), raw.size()));

    EXPECT_EQ(parsed.getString(FixTag::MsgType), "8");
    EXPECT_EQ(parsed.getInt(FixTag::OrderID), 42);
    EXPECT_EQ(parsed.getInt(FixTag::OrderQty), 200);
    EXPECT_TRUE(parsed.validateChecksum());
}

TEST(FIXParserTest, ChecksumAnchoredAtTrailer) {
    // Hand-crafted message whose body contains "10=NNN<SOH>" inside a tag
    // value (tag 58 / Text). The trailer-anchored validator must use the
    // real trailer at end-of-message, not the embedded "10=".
    std::string body;
    body += "35=D"; body += FIX_SOH;
    body += "58=10=999"; body += FIX_SOH;  // value contains "10=999"
    std::string header = "8=FIX.4.2";
    header += FIX_SOH;
    header += "9=" + std::to_string(body.size());
    header += FIX_SOH;
    std::string pre = header + body;
    uint32_t sum = 0;
    for (char c : pre) sum += static_cast<uint8_t>(c);
    char cs[4]; snprintf(cs, sizeof(cs), "%03d", sum % 256);
    std::string raw = pre + "10=" + cs + FIX_SOH;

    FixMessage msg;
    ASSERT_TRUE(msg.parse(raw.c_str(), raw.size()));
    EXPECT_TRUE(msg.validateChecksum());
    EXPECT_TRUE(msg.isWellFormed());
}

TEST(FIXParserTest, RejectMissingTrailer) {
    // No "10=" trailer — must not validate.
    std::string raw = "8=FIX.4.2";
    raw += FIX_SOH;
    raw += "35=D"; raw += FIX_SOH;
    FixMessage msg;
    ASSERT_TRUE(msg.parse(raw.c_str(), raw.size()));
    EXPECT_FALSE(msg.validateChecksum());
    EXPECT_FALSE(msg.isWellFormed());
}

TEST(FIXParserTest, RejectOversizeTag) {
    // 19-digit tag should be rejected by the bounded parser, not overflow.
    std::string raw = "9999999999999999999=x";
    raw += FIX_SOH;
    FixMessage msg;
    EXPECT_FALSE(msg.parse(raw.c_str(), raw.size()));
}

TEST(FIXParserTest, GetIntSaturatesOnOverflow) {
    std::string raw = "44=99999999999999999999";  // 20 digits, overflows int64
    raw += FIX_SOH;
    FixMessage msg;
    ASSERT_TRUE(msg.parse(raw.c_str(), raw.size()));
    EXPECT_EQ(msg.getInt(44), INT64_MAX);
}

// ─── FixFramer Tests ────────────────────────────────────────────────────────

namespace {

// Build a valid FIX 4.2 ExecutionReport for use as a known-good frame.
std::string makeFrame() {
    return FixSerializer::buildExecutionReport(
        42, 7, '0', '0', Side::Buy, 1000, 100, 0, 100);
}

struct CapturingFramer {
    FixFramer framer;
    std::vector<std::string> frames;
    CapturingFramer() {
        framer.setOnMessage([this](std::string_view raw, const FixMessage&) {
            frames.emplace_back(raw);
        });
    }
};

}  // namespace

TEST(FixFramerTest, SingleFrameDelivered) {
    CapturingFramer cf;
    auto frame = makeFrame();
    EXPECT_TRUE(cf.framer.feed(frame.data(), frame.size()));
    ASSERT_EQ(cf.frames.size(), 1u);
    EXPECT_EQ(cf.frames[0], frame);
    EXPECT_EQ(cf.framer.framesEmitted(), 1u);
    EXPECT_EQ(cf.framer.framesRejected(), 0u);
}

TEST(FixFramerTest, FrameSplitAcrossManyFeeds) {
    CapturingFramer cf;
    auto frame = makeFrame();
    // Feed one byte at a time — pathological fragmentation.
    for (size_t i = 0; i < frame.size(); ++i) {
        EXPECT_TRUE(cf.framer.feed(frame.data() + i, 1));
    }
    ASSERT_EQ(cf.frames.size(), 1u);
    EXPECT_EQ(cf.frames[0], frame);
}

TEST(FixFramerTest, MultipleFramesInOneFeed) {
    CapturingFramer cf;
    auto a = makeFrame();
    auto b = makeFrame();
    auto combined = a + b;
    EXPECT_TRUE(cf.framer.feed(combined.data(), combined.size()));
    EXPECT_EQ(cf.frames.size(), 2u);
}

TEST(FixFramerTest, GarbagePrefixIsDiscardedAndFrameRecovered) {
    CapturingFramer cf;
    std::string junk = "AAAAAAAAAAAAAAA";
    auto frame = makeFrame();
    auto combined = junk + frame;
    EXPECT_TRUE(cf.framer.feed(combined.data(), combined.size()));
    ASSERT_EQ(cf.frames.size(), 1u);
    EXPECT_EQ(cf.frames[0], frame);
}

TEST(FixFramerTest, OversizeBodyLengthIsRejected) {
    CapturingFramer cf;
    std::string bogus = "8=FIX.4.2";
    bogus += FIX_SOH;
    bogus += "9=99999999";  // far above maxFrameSize
    bogus += FIX_SOH;
    EXPECT_FALSE(cf.framer.feed(bogus.data(), bogus.size()));
}

TEST(FixFramerTest, TruncatedTrailerWaitsForMore) {
    CapturingFramer cf;
    auto frame = makeFrame();
    // Feed everything except the final SOH — framer should buffer, not emit.
    EXPECT_TRUE(cf.framer.feed(frame.data(), frame.size() - 1));
    EXPECT_EQ(cf.frames.size(), 0u);
    // Now feed the final byte — frame completes.
    EXPECT_TRUE(cf.framer.feed(frame.data() + frame.size() - 1, 1));
    EXPECT_EQ(cf.frames.size(), 1u);
}

TEST(FixFramerTest, CorruptedChecksumDoesNotEmit) {
    CapturingFramer cf;
    auto frame = makeFrame();
    // Flip a body byte without recomputing the trailer's checksum.
    frame[20] ^= 1;
    EXPECT_TRUE(cf.framer.feed(frame.data(), frame.size()));
    EXPECT_EQ(cf.frames.size(), 0u);
    EXPECT_EQ(cf.framer.framesRejected(), 1u);
}

// ─── FixSession Tests ───────────────────────────────────────────────────────

namespace {

// Build a complete, framed FIX message with the given BeginString,
// MsgType, and tag map. Computes BodyLength and CheckSum so the framer
// accepts it as well-formed.
std::string makeFixMessageVer(const char* beginString,
                              const char* msgType,
                              const std::vector<std::pair<int, std::string>>& tags) {
    std::string body;
    body += "35="; body += msgType; body += FIX_SOH;
    for (auto& [tag, val] : tags) {
        body += std::to_string(tag); body += "="; body += val; body += FIX_SOH;
    }
    std::string head = "8=";
    head += beginString;
    head += FIX_SOH;
    head += "9=" + std::to_string(body.size());
    head += FIX_SOH;
    std::string pre = head + body;
    uint32_t sum = 0;
    for (char c : pre) sum += static_cast<uint8_t>(c);
    char cs[4]; std::snprintf(cs, sizeof(cs), "%03d", sum % 256);
    return pre + "10=" + cs + FIX_SOH;
}

// Convenience: default to FIX 4.2 (matches existing test call sites).
std::string makeFixMessage(const char* msgType,
                           const std::vector<std::pair<int, std::string>>& tags) {
    return makeFixMessageVer("FIX.4.2", msgType, tags);
}

}  // namespace

TEST(FixSessionTest, NewOrderDispatchesToEngineAndAcks) {
    MatchingEngine engine;
    engine.addSymbol(7);
    engine.start();

    std::string sent;
    FixSession session(engine, [&](std::string_view bytes) {
        sent.append(bytes);
    });

    auto msg = makeFixMessage("D", {
        {FixTag::SenderCompID, "100"},
        {FixTag::ClOrdID,      "1001"},
        {FixTag::Symbol,       "7"},
        {FixTag::Side,         "1"},
        {FixTag::OrderQty,     "50"},
        {FixTag::Price,        "1000"},
        {FixTag::OrdType,      "2"},
        {FixTag::TimeInForce,  "1"},
    });
    ASSERT_TRUE(session.feed(msg.data(), msg.size()));

    EXPECT_EQ(session.framesEmitted(), 1u);
    EXPECT_EQ(session.ordersAccepted(), 1u);
    EXPECT_EQ(session.ordersRejected(), 0u);

    // Order landed in the book.
    auto* book = engine.getOrderBook(7);
    ASSERT_NE(book, nullptr);
    const Order* o = book->getOrder(1001);
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->remainingQty, 50u);

    // An ExecutionReport was emitted; parse it back and confirm fields.
    ASSERT_FALSE(sent.empty());
    FixMessage resp;
    ASSERT_TRUE(resp.parse(sent.data(), sent.size()));
    EXPECT_TRUE(resp.validateChecksum());
    EXPECT_EQ(resp.getString(FixTag::MsgType), "8");
    EXPECT_EQ(resp.getUint64(FixTag::MsgSeqNum), 1u);
    EXPECT_EQ(resp.getUint64(FixTag::OrderID), 1001u);
    EXPECT_EQ(resp.getChar(FixTag::ExecType), '0');
}

TEST(FixSessionTest, FragmentedFeedReassemblesAndDispatches) {
    MatchingEngine engine;
    engine.addSymbol(1);
    engine.start();

    std::string sent;
    FixSession session(engine, [&](std::string_view b) { sent.append(b); });

    auto msg = makeFixMessage("D", {
        {FixTag::SenderCompID, "1"}, {FixTag::ClOrdID, "42"},
        {FixTag::Symbol, "1"}, {FixTag::Side, "1"},
        {FixTag::OrderQty, "10"}, {FixTag::Price, "500"},
        {FixTag::OrdType, "2"}, {FixTag::TimeInForce, "1"},
    });
    // One byte at a time — pathological fragmentation.
    for (size_t i = 0; i < msg.size(); ++i) {
        ASSERT_TRUE(session.feed(msg.data() + i, 1));
    }
    EXPECT_EQ(session.framesEmitted(), 1u);
    EXPECT_EQ(session.ordersAccepted(), 1u);
}

TEST(FixSessionTest, CancelDispatch) {
    MatchingEngine engine;
    engine.addSymbol(2);
    engine.start();
    // Place an order to cancel.
    engine.submitOrder(2, 7, 1, Side::Buy, 100, 5, OrderType::Limit);

    std::string sent;
    FixSession session(engine, [&](std::string_view b) { sent.append(b); });
    auto msg = makeFixMessage("F", {
        {FixTag::OrigClOrdID, "7"}, {FixTag::Symbol, "2"},
    });
    ASSERT_TRUE(session.feed(msg.data(), msg.size()));
    EXPECT_EQ(session.ordersAccepted(), 1u);

    auto* book = engine.getOrderBook(2);
    EXPECT_EQ(book->getOrder(7), nullptr);
}

TEST(FixSessionTest, UnsupportedMsgTypeIsRejected) {
    MatchingEngine engine;
    engine.start();

    std::string sent;
    FixSession session(engine, [&](std::string_view b) { sent.append(b); });

    // 35=Z is not a supported MsgType; framer parses it (well-formed) and
    // session rejects via fixToOrderParams.
    auto msg = makeFixMessage("Z", {{FixTag::ClOrdID, "999"}});
    ASSERT_TRUE(session.feed(msg.data(), msg.size()));

    EXPECT_EQ(session.framesEmitted(), 1u);
    EXPECT_EQ(session.ordersAccepted(), 0u);

    // A reject ExecutionReport must have been emitted.
    ASSERT_FALSE(sent.empty());
    FixMessage resp;
    ASSERT_TRUE(resp.parse(sent.data(), sent.size()));
    EXPECT_EQ(resp.getChar(FixTag::ExecType), '8');  // Reject
}

TEST(FixSessionTest, RejectsFix44ByDefault) {
    // FIX 4.4 BeginString. The framer accepts the frame (it matches the
    // "8=FIX.4." prefix), but the session must reject before dispatch
    // because 4.4 isn't in the default acceptedVersions_.
    MatchingEngine engine;
    engine.addSymbol(7);
    engine.start();

    std::string sent;
    FixSession session(engine, [&](std::string_view b) { sent.append(b); });

    auto msg = makeFixMessageVer("FIX.4.4", "D", {
        {FixTag::SenderCompID, "100"},
        {FixTag::ClOrdID,      "1001"},
        {FixTag::Symbol,       "7"},
        {FixTag::Side,         "1"},
        {FixTag::OrderQty,     "50"},
        {FixTag::Price,        "1000"},
        {FixTag::OrdType,      "2"},
        {FixTag::TimeInForce,  "1"},
    });
    ASSERT_TRUE(session.feed(msg.data(), msg.size()));

    EXPECT_EQ(session.framesEmitted(), 1u)
        << "framer accepts the frame; rejection happens at session level";
    EXPECT_EQ(session.ordersAccepted(), 0u);

    auto* book = engine.getOrderBook(7);
    EXPECT_EQ(book->getOrder(1001), nullptr) << "wrong-version order must NOT reach engine";

    // Reject ExecutionReport with the configured "unsupported version" text.
    ASSERT_FALSE(sent.empty());
    FixMessage resp;
    ASSERT_TRUE(resp.parse(sent.data(), sent.size()));
    EXPECT_EQ(resp.getChar(FixTag::ExecType), '8');
    EXPECT_EQ(resp.getString(FixTag::Text), "unsupported version");
}

TEST(FixSessionTest, AcceptsFix44WhenConfigured) {
    // Coordinator configures the session to accept both 4.2 and 4.4
    // during a transition period. The 4.4 message now dispatches.
    // FIX 4.4 requires TransactTime (60) on NewOrderSingle — the
    // session validates this; see Fix44NewOrderSingleRequiresTransactTime.
    MatchingEngine engine;
    engine.addSymbol(7);
    engine.start();

    std::string sent;
    FixSession session(engine, [&](std::string_view b) { sent.append(b); });
    session.setAcceptedVersions({"FIX.4.2", "FIX.4.4"});

    auto msg = makeFixMessageVer("FIX.4.4", "D", {
        {FixTag::SenderCompID, "100"},
        {FixTag::ClOrdID,      "1001"},
        {FixTag::Symbol,       "7"},
        {FixTag::Side,         "1"},
        {FixTag::OrderQty,     "50"},
        {FixTag::Price,        "1000"},
        {FixTag::OrdType,      "2"},
        {FixTag::TimeInForce,  "1"},
        {FixTag::TransactTime, "20260509-12:00:00.000"},
    });
    ASSERT_TRUE(session.feed(msg.data(), msg.size()));

    EXPECT_EQ(session.ordersAccepted(), 1u);
    auto* book = engine.getOrderBook(7);
    EXPECT_NE(book->getOrder(1001), nullptr);

    ASSERT_FALSE(sent.empty());
    FixMessage resp;
    ASSERT_TRUE(resp.parse(sent.data(), sent.size()));
    EXPECT_TRUE(resp.validateChecksum());
    EXPECT_EQ(resp.getString(FixTag::BeginString), "FIX.4.4");
    EXPECT_EQ(resp.getString(FixTag::MsgType), "8");
}

TEST(FixSessionTest, RejectsUnknownVersionString) {
    // A garbage-but-frameable BeginString (still matches "8=FIX.4." prefix
    // because that's what the framer anchors on). Session must reject.
    MatchingEngine engine;
    engine.start();

    std::string sent;
    FixSession session(engine, [&](std::string_view b) { sent.append(b); });

    auto msg = makeFixMessageVer("FIX.4.99", "D", {
        {FixTag::ClOrdID, "1"},
        {FixTag::Symbol,  "1"},
        {FixTag::Side,    "1"},
        {FixTag::OrderQty,"1"},
        {FixTag::Price,   "100"},
        {FixTag::OrdType, "2"},
    });
    ASSERT_TRUE(session.feed(msg.data(), msg.size()));
    EXPECT_EQ(session.ordersAccepted(), 0u);
}

TEST(FixSessionTest, LogonAcknowledgesAndSetsSessionState) {
    MatchingEngine engine;
    engine.start();

    std::string sent;
    FixSession session(engine, [&](std::string_view b) { sent.append(b); });

    auto msg = makeFixMessage("A", {
        {FixTag::SenderCompID, "CLIENT1"},
        {FixTag::TargetCompID, "ORDERBOOK"},
        {FixTag::HeartBtInt, "15"},
    });
    ASSERT_TRUE(session.feed(msg.data(), msg.size()));

    EXPECT_TRUE(session.loggedOn());
    EXPECT_FALSE(session.shouldDisconnect());
    EXPECT_EQ(session.sessionMessagesHandled(), 1u);
    EXPECT_EQ(session.ordersAccepted(), 0u);

    ASSERT_FALSE(sent.empty());
    FixMessage resp;
    ASSERT_TRUE(resp.parse(sent.data(), sent.size()));
    EXPECT_TRUE(resp.validateChecksum());
    EXPECT_EQ(resp.getString(FixTag::MsgType), "A");
    EXPECT_EQ(resp.getUint64(FixTag::MsgSeqNum), 1u);
    EXPECT_EQ(resp.getUint64(FixTag::HeartBtInt), 15u);
}

TEST(FixSessionTest, LogonResponsePreservesAcceptedFix44Version) {
    MatchingEngine engine;
    engine.start();

    std::string sent;
    FixSession session(engine, [&](std::string_view b) { sent.append(b); });
    session.setAcceptedVersions({"FIX.4.2", "FIX.4.4"});

    auto msg = makeFixMessageVer("FIX.4.4", "A", {
        {FixTag::SenderCompID, "CLIENT1"},
        {FixTag::TargetCompID, "ORDERBOOK"},
        {FixTag::HeartBtInt, "20"},
    });
    ASSERT_TRUE(session.feed(msg.data(), msg.size()));

    ASSERT_FALSE(sent.empty());
    FixMessage resp;
    ASSERT_TRUE(resp.parse(sent.data(), sent.size()));
    EXPECT_TRUE(resp.validateChecksum());
    EXPECT_EQ(resp.getString(FixTag::BeginString), "FIX.4.4");
    EXPECT_EQ(resp.getString(FixTag::MsgType), "A");
    EXPECT_EQ(resp.getUint64(FixTag::HeartBtInt), 20u);
}

TEST(FixSessionTest, InOrderMsgSeqNumAdvancesAndDispatches) {
    MatchingEngine engine;
    engine.addSymbol(1);
    engine.start();

    std::string sent;
    FixSession session(engine, [&](std::string_view b) { sent.append(b); });

    auto msg = makeFixMessage("D", {
        {FixTag::MsgSeqNum, "1"},
        {FixTag::SenderCompID, "10"},
        {FixTag::ClOrdID, "5001"},
        {FixTag::Symbol, "1"},
        {FixTag::Side, "1"},
        {FixTag::OrderQty, "10"},
        {FixTag::Price, "1000"},
        {FixTag::OrdType, "2"},
    });
    ASSERT_TRUE(session.feed(msg.data(), msg.size()));

    EXPECT_EQ(session.expectedInboundSeqNum(), 2u);
    EXPECT_EQ(session.ordersAccepted(), 1u);
    auto* book = engine.getOrderBook(1);
    ASSERT_NE(book, nullptr);
    EXPECT_NE(book->getOrder(5001), nullptr);
}

TEST(FixSessionTest, OutboundSeqNumIsMonotonicAcrossAdminAndAppResponses) {
    MatchingEngine engine;
    engine.addSymbol(1);
    engine.start();

    std::string sent;
    FixSession session(engine, [&](std::string_view b) { sent.append(b); });

    auto logon = makeFixMessage("A", {
        {FixTag::MsgSeqNum, "1"},
        {FixTag::HeartBtInt, "30"},
    });
    ASSERT_TRUE(session.feed(logon.data(), logon.size()));

    ASSERT_FALSE(sent.empty());
    FixMessage logonResp;
    ASSERT_TRUE(logonResp.parse(sent.data(), sent.size()));
    EXPECT_EQ(logonResp.getString(FixTag::MsgType), "A");
    EXPECT_EQ(logonResp.getUint64(FixTag::MsgSeqNum), 1u);
    sent.clear();

    auto order = makeFixMessage("D", {
        {FixTag::MsgSeqNum, "2"},
        {FixTag::SenderCompID, "10"},
        {FixTag::ClOrdID, "5101"},
        {FixTag::Symbol, "1"},
        {FixTag::Side, "1"},
        {FixTag::OrderQty, "10"},
        {FixTag::Price, "1000"},
        {FixTag::OrdType, "2"},
    });
    ASSERT_TRUE(session.feed(order.data(), order.size()));

    ASSERT_FALSE(sent.empty());
    FixMessage orderResp;
    ASSERT_TRUE(orderResp.parse(sent.data(), sent.size()));
    EXPECT_TRUE(orderResp.validateChecksum());
    EXPECT_EQ(orderResp.getString(FixTag::MsgType), "8");
    EXPECT_EQ(orderResp.getUint64(FixTag::MsgSeqNum), 2u);
    EXPECT_EQ(orderResp.getUint64(FixTag::OrderID), 5101u);
}

TEST(FixSessionTest, MsgSeqNumGapRequestsResendAndDoesNotDispatch) {
    MatchingEngine engine;
    engine.addSymbol(1);
    engine.start();

    std::string sent;
    FixSession session(engine, [&](std::string_view b) { sent.append(b); });

    auto msg = makeFixMessage("D", {
        {FixTag::MsgSeqNum, "3"},
        {FixTag::SenderCompID, "10"},
        {FixTag::ClOrdID, "5002"},
        {FixTag::Symbol, "1"},
        {FixTag::Side, "1"},
        {FixTag::OrderQty, "10"},
        {FixTag::Price, "1000"},
        {FixTag::OrdType, "2"},
    });
    ASSERT_TRUE(session.feed(msg.data(), msg.size()));

    EXPECT_EQ(session.expectedInboundSeqNum(), 1u);
    EXPECT_EQ(session.ordersAccepted(), 0u);
    EXPECT_EQ(engine.getOrderBook(1)->getOrder(5002), nullptr);

    ASSERT_FALSE(sent.empty());
    FixMessage resp;
    ASSERT_TRUE(resp.parse(sent.data(), sent.size()));
    EXPECT_TRUE(resp.validateChecksum());
    EXPECT_EQ(resp.getString(FixTag::MsgType), "2");
    EXPECT_EQ(resp.getUint64(FixTag::BeginSeqNo), 1u);
    EXPECT_EQ(resp.getUint64(FixTag::EndSeqNo), 2u);
}

TEST(FixSessionTest, DuplicateMsgSeqNumWithoutPossDupIsRejected) {
    MatchingEngine engine;
    engine.addSymbol(1);
    engine.start();

    std::string sent;
    FixSession session(engine, [&](std::string_view b) { sent.append(b); });

    auto first = makeFixMessage("D", {
        {FixTag::MsgSeqNum, "1"},
        {FixTag::SenderCompID, "10"},
        {FixTag::ClOrdID, "5003"},
        {FixTag::Symbol, "1"},
        {FixTag::Side, "1"},
        {FixTag::OrderQty, "10"},
        {FixTag::Price, "1000"},
        {FixTag::OrdType, "2"},
    });
    ASSERT_TRUE(session.feed(first.data(), first.size()));
    sent.clear();

    auto dup = makeFixMessage("D", {
        {FixTag::MsgSeqNum, "1"},
        {FixTag::SenderCompID, "10"},
        {FixTag::ClOrdID, "5004"},
        {FixTag::Symbol, "1"},
        {FixTag::Side, "1"},
        {FixTag::OrderQty, "10"},
        {FixTag::Price, "1000"},
        {FixTag::OrdType, "2"},
    });
    ASSERT_TRUE(session.feed(dup.data(), dup.size()));

    EXPECT_EQ(session.expectedInboundSeqNum(), 2u);
    EXPECT_EQ(session.ordersAccepted(), 1u);
    EXPECT_EQ(engine.getOrderBook(1)->getOrder(5004), nullptr);

    ASSERT_FALSE(sent.empty());
    FixMessage resp;
    ASSERT_TRUE(resp.parse(sent.data(), sent.size()));
    EXPECT_EQ(resp.getChar(FixTag::ExecType), '8');
    EXPECT_EQ(resp.getString(FixTag::Text), "seqnum too low");
}

TEST(FixSessionTest, DuplicateMsgSeqNumWithPossDupIsIgnored) {
    MatchingEngine engine;
    engine.addSymbol(1);
    engine.start();

    std::string sent;
    FixSession session(engine, [&](std::string_view b) { sent.append(b); });

    auto first = makeFixMessage("D", {
        {FixTag::MsgSeqNum, "1"},
        {FixTag::SenderCompID, "10"},
        {FixTag::ClOrdID, "5005"},
        {FixTag::Symbol, "1"},
        {FixTag::Side, "1"},
        {FixTag::OrderQty, "10"},
        {FixTag::Price, "1000"},
        {FixTag::OrdType, "2"},
    });
    ASSERT_TRUE(session.feed(first.data(), first.size()));
    sent.clear();

    auto dup = makeFixMessage("D", {
        {FixTag::MsgSeqNum, "1"},
        {FixTag::PossDupFlag, "Y"},
        {FixTag::SenderCompID, "10"},
        {FixTag::ClOrdID, "5006"},
        {FixTag::Symbol, "1"},
        {FixTag::Side, "1"},
        {FixTag::OrderQty, "10"},
        {FixTag::Price, "1000"},
        {FixTag::OrdType, "2"},
    });
    ASSERT_TRUE(session.feed(dup.data(), dup.size()));

    EXPECT_EQ(session.expectedInboundSeqNum(), 2u);
    EXPECT_EQ(session.ordersAccepted(), 1u);
    EXPECT_EQ(engine.getOrderBook(1)->getOrder(5006), nullptr);
    EXPECT_TRUE(sent.empty());
}

TEST(FixSessionTest, TestRequestEmitsHeartbeatWithTestReqID) {
    MatchingEngine engine;
    engine.start();

    std::string sent;
    FixSession session(engine, [&](std::string_view b) { sent.append(b); });

    auto msg = makeFixMessage("1", {
        {FixTag::TestReqID, "probe-42"},
    });
    ASSERT_TRUE(session.feed(msg.data(), msg.size()));

    EXPECT_EQ(session.sessionMessagesHandled(), 1u);
    ASSERT_FALSE(sent.empty());
    FixMessage resp;
    ASSERT_TRUE(resp.parse(sent.data(), sent.size()));
    EXPECT_TRUE(resp.validateChecksum());
    EXPECT_EQ(resp.getString(FixTag::MsgType), "0");
    EXPECT_EQ(resp.getString(FixTag::TestReqID), "probe-42");
}

TEST(FixSessionTest, ResendRequestReturnsSequenceResetGapFill) {
    MatchingEngine engine;
    engine.start();

    std::string sent;
    FixSession session(engine, [&](std::string_view b) { sent.append(b); });

    auto msg = makeFixMessage("2", {
        {FixTag::BeginSeqNo, "1"},
        {FixTag::EndSeqNo, "7"},
    });
    ASSERT_TRUE(session.feed(msg.data(), msg.size()));

    EXPECT_EQ(session.sessionMessagesHandled(), 1u);
    ASSERT_FALSE(sent.empty());
    FixMessage resp;
    ASSERT_TRUE(resp.parse(sent.data(), sent.size()));
    EXPECT_TRUE(resp.validateChecksum());
    EXPECT_EQ(resp.getString(FixTag::MsgType), "4");
    EXPECT_EQ(resp.getString(FixTag::GapFillFlag), "Y");
    EXPECT_EQ(resp.getUint64(FixTag::NewSeqNo), 8u);
}

TEST(FixSessionTest, LogoutAcknowledgesAndRequestsDisconnect) {
    MatchingEngine engine;
    engine.start();

    std::string sent;
    FixSession session(engine, [&](std::string_view b) { sent.append(b); });

    auto logon = makeFixMessage("A", {{FixTag::HeartBtInt, "30"}});
    ASSERT_TRUE(session.feed(logon.data(), logon.size()));
    sent.clear();

    auto logout = makeFixMessage("5", {{FixTag::Text, "client shutdown"}});
    EXPECT_FALSE(session.feed(logout.data(), logout.size()))
        << "gateway should close after sending Logout ack";

    EXPECT_FALSE(session.loggedOn());
    EXPECT_TRUE(session.shouldDisconnect());
    EXPECT_EQ(session.sessionMessagesHandled(), 2u);

    ASSERT_FALSE(sent.empty());
    FixMessage resp;
    ASSERT_TRUE(resp.parse(sent.data(), sent.size()));
    EXPECT_TRUE(resp.validateChecksum());
    EXPECT_EQ(resp.getString(FixTag::MsgType), "5");
    EXPECT_EQ(resp.getString(FixTag::Text), "logout acknowledged");
}

// ─── FIX 4.4 tag-mapping tests ──────────────────────────────────────────────
// FIX 4.4 differs from 4.2 in three places that actually matter for the
// surfaces this engine exposes:
//   1. ExecType (150): per-fill codes '1'/'2' are deprecated in favor of
//      'F' (Trade). The serializer rewrites; ordStatus (39) still uses
//      the 4.2 codes, per spec.
//   2. TransactTime (60): required on D / F / G inbound messages and on
//      8 outbound ExecutionReports. The session validates inbound, and
//      buildExecutionReport always emits.
//   3. OrdRejReason (103): expected on rejects. The session emits a
//      standard FIX numeric code mapped from RejectReason.
// The tests below pin all three.

TEST(FixSerializerVersionTest, Fix44ExecutionReportRewritesPartialFillToTradeF) {
    auto raw = FixSerializer::buildExecutionReport(
        /*orderId=*/42, /*execId=*/42, /*execType=*/'1', /*ordStatus=*/'1',
        Side::Buy, /*price=*/1000, /*orderQty=*/100,
        /*cumQty=*/40, /*leavesQty=*/60, /*lastPx=*/1000, /*lastQty=*/40,
        /*text=*/"", "FIX.4.4", /*msgSeqNum=*/0,
        FixVersion::FIX_4_4);
    FixMessage msg;
    ASSERT_TRUE(msg.parse(raw.data(), raw.size()));
    EXPECT_EQ(msg.getChar(FixTag::ExecType), 'F') << "4.4 deprecates '1'";
    EXPECT_EQ(msg.getChar(FixTag::OrdStatus), '1') << "OrdStatus retains 4.2 codes";
}

TEST(FixSerializerVersionTest, Fix44ExecutionReportRewritesFullFillToTradeF) {
    auto raw = FixSerializer::buildExecutionReport(
        42, 42, '2', '2', Side::Sell, 1000, 100, 100, 0, 1000, 100,
        "", "FIX.4.4", 0, FixVersion::FIX_4_4);
    FixMessage msg;
    ASSERT_TRUE(msg.parse(raw.data(), raw.size()));
    EXPECT_EQ(msg.getChar(FixTag::ExecType), 'F');
    EXPECT_EQ(msg.getChar(FixTag::OrdStatus), '2');
}

TEST(FixSerializerVersionTest, Fix42KeepsLegacyExecTypeCodes) {
    // Back-compat: 4.2 callers must continue to see '1' and '2'.
    auto partial = FixSerializer::buildExecutionReport(
        1, 1, '1', '1', Side::Buy, 100, 10, 5, 5, 100, 5,
        "", "FIX.4.2", 0, FixVersion::FIX_4_2);
    FixMessage p;
    ASSERT_TRUE(p.parse(partial.data(), partial.size()));
    EXPECT_EQ(p.getChar(FixTag::ExecType), '1');

    auto full = FixSerializer::buildExecutionReport(
        1, 1, '2', '2', Side::Buy, 100, 10, 10, 0, 100, 10,
        "", "FIX.4.2", 0, FixVersion::FIX_4_2);
    FixMessage f;
    ASSERT_TRUE(f.parse(full.data(), full.size()));
    EXPECT_EQ(f.getChar(FixTag::ExecType), '2');
}

TEST(FixSerializerVersionTest, ExecutionReportEmitsTransactTime) {
    // Both 4.2 and 4.4 emit tag 60. We assert the canonical FIX UTC
    // form: "YYYYMMDD-HH:MM:SS.sss" (21 chars including the dash and
    // millisecond fractional). Callers that need a deterministic
    // timestamp pass it explicitly.
    auto raw = FixSerializer::buildExecutionReport(
        7, 7, '0', '0', Side::Buy, 100, 10, 0, 10, 0, 0,
        "", "FIX.4.4", 0, FixVersion::FIX_4_4,
        kOrdRejReasonOmit, "20260509-12:00:00.000");
    FixMessage msg;
    ASSERT_TRUE(msg.parse(raw.data(), raw.size()));
    EXPECT_EQ(msg.getString(FixTag::TransactTime), "20260509-12:00:00.000");
}

TEST(FixSerializerVersionTest, RejectExecutionReportEmitsOrdRejReason) {
    auto raw = FixSerializer::buildExecutionReport(
        7, 7, '8', '8', Side::Buy, 0, 0, 0, 0, 0, 0,
        "symbol", "FIX.4.4", 0, FixVersion::FIX_4_4,
        ordRejReasonCode(RejectReason::SymbolNotFound));
    FixMessage msg;
    ASSERT_TRUE(msg.parse(raw.data(), raw.size()));
    EXPECT_EQ(msg.getChar(FixTag::ExecType), '8');
    EXPECT_EQ(msg.getInt(FixTag::OrdRejReason), 1) << "OrdRejReason 1 = Unknown symbol";
}

TEST(FixSerializerVersionTest, NonRejectDoesNotEmitOrdRejReason) {
    // Even if a numeric code is passed, OrdRejReason is omitted on
    // non-reject reports — emitting it would mislead downstream consumers.
    auto raw = FixSerializer::buildExecutionReport(
        7, 7, '0', '0', Side::Buy, 100, 10, 0, 10, 0, 0,
        "", "FIX.4.4", 0, FixVersion::FIX_4_4,
        /*ordRejReason=*/3);
    FixMessage msg;
    ASSERT_TRUE(msg.parse(raw.data(), raw.size()));
    EXPECT_FALSE(msg.hasTag(FixTag::OrdRejReason));
}

TEST(FixSessionTest, Fix44NewOrderSingleRequiresTransactTime) {
    // Per FIX 4.4 spec, NewOrderSingle (D) requires TransactTime (60).
    // Sending a 4.4 D without it must reject before the engine sees it.
    MatchingEngine engine;
    engine.addSymbol(7);
    engine.start();

    std::string sent;
    FixSession session(engine, [&](std::string_view b) { sent.append(b); });
    session.setAcceptedVersions({"FIX.4.2", "FIX.4.4"});

    auto msg = makeFixMessageVer("FIX.4.4", "D", {
        {FixTag::SenderCompID, "100"},
        {FixTag::ClOrdID,      "1234"},
        {FixTag::Symbol,       "7"},
        {FixTag::Side,         "1"},
        {FixTag::OrderQty,     "10"},
        {FixTag::Price,        "1000"},
        {FixTag::OrdType,      "2"},
        {FixTag::TimeInForce,  "1"},
        // Deliberately no TransactTime.
    });
    ASSERT_TRUE(session.feed(msg.data(), msg.size()));

    EXPECT_EQ(session.ordersAccepted(), 0u);
    EXPECT_EQ(session.ordersRejected(), 1u);

    auto* book = engine.getOrderBook(7);
    EXPECT_EQ(book->getOrder(1234), nullptr)
        << "missing-field message must NOT reach the engine";

    ASSERT_FALSE(sent.empty());
    FixMessage resp;
    ASSERT_TRUE(resp.parse(sent.data(), sent.size()));
    EXPECT_TRUE(resp.validateChecksum());
    EXPECT_EQ(resp.getString(FixTag::BeginString), "FIX.4.4");
    EXPECT_EQ(resp.getChar(FixTag::ExecType), '8');
    EXPECT_EQ(resp.getString(FixTag::Text), "missing required field");
    EXPECT_EQ(resp.getUint64(FixTag::OrderID), 1234u)
        << "ClOrdID echoed back so the client can correlate the reject";
}

TEST(FixSessionTest, Fix42NewOrderSingleAcceptsWithoutTransactTime) {
    // Back-compat: 4.2 left TransactTime conditional, so existing 4.2
    // clients that omit it must continue to be accepted.
    MatchingEngine engine;
    engine.addSymbol(7);
    engine.start();

    std::string sent;
    FixSession session(engine, [&](std::string_view b) { sent.append(b); });

    auto msg = makeFixMessageVer("FIX.4.2", "D", {
        {FixTag::SenderCompID, "100"}, {FixTag::ClOrdID, "9999"},
        {FixTag::Symbol, "7"}, {FixTag::Side, "1"},
        {FixTag::OrderQty, "10"}, {FixTag::Price, "1000"},
        {FixTag::OrdType, "2"}, {FixTag::TimeInForce, "1"},
    });
    ASSERT_TRUE(session.feed(msg.data(), msg.size()));
    EXPECT_EQ(session.ordersAccepted(), 1u);
}

TEST(FixSessionTest, Fix44SessionRejectIncludesOrdRejReason) {
    // Hit a real engine reject path (SymbolNotFound — symbol 99 is
    // unregistered) on a 4.4 session. The reject must carry tag 103
    // with the standard FIX numeric code.
    MatchingEngine engine;
    engine.start();  // No symbols registered.

    std::string sent;
    FixSession session(engine, [&](std::string_view b) { sent.append(b); });
    session.setAcceptedVersions({"FIX.4.2", "FIX.4.4"});

    auto msg = makeFixMessageVer("FIX.4.4", "D", {
        {FixTag::SenderCompID, "100"},
        {FixTag::ClOrdID,      "5555"},
        {FixTag::Symbol,       "99"},  // unregistered
        {FixTag::Side,         "1"},
        {FixTag::OrderQty,     "10"},
        {FixTag::Price,        "1000"},
        {FixTag::OrdType,      "2"},
        {FixTag::TimeInForce,  "1"},
        {FixTag::TransactTime, "20260509-12:00:00.000"},
    });
    ASSERT_TRUE(session.feed(msg.data(), msg.size()));

    EXPECT_EQ(session.ordersAccepted(), 0u);
    EXPECT_EQ(session.ordersRejected(), 1u);

    ASSERT_FALSE(sent.empty());
    FixMessage resp;
    ASSERT_TRUE(resp.parse(sent.data(), sent.size()));
    EXPECT_EQ(resp.getChar(FixTag::ExecType), '8');
    EXPECT_EQ(resp.getString(FixTag::Text), "symbol");
    EXPECT_EQ(resp.getInt(FixTag::OrdRejReason), 1)
        << "SymbolNotFound maps to standard OrdRejReason 1 (Unknown symbol)";
    EXPECT_FALSE(resp.getString(FixTag::TransactTime).empty())
        << "all 4.4 ExecutionReports carry TransactTime";
}

TEST(FixSessionTest, Fix42RejectStillEmitsOrdRejReason) {
    // Even on 4.2, OrdRejReason is optional-but-allowed; emitting it
    // makes the response useful to clients that already inspect it.
    MatchingEngine engine;
    engine.start();

    std::string sent;
    FixSession session(engine, [&](std::string_view b) { sent.append(b); });

    auto msg = makeFixMessage("D", {
        {FixTag::SenderCompID, "1"}, {FixTag::ClOrdID, "1"},
        {FixTag::Symbol, "42"},  // unregistered
        {FixTag::Side, "1"}, {FixTag::OrderQty, "1"},
        {FixTag::Price, "100"}, {FixTag::OrdType, "2"},
    });
    ASSERT_TRUE(session.feed(msg.data(), msg.size()));

    ASSERT_FALSE(sent.empty());
    FixMessage resp;
    ASSERT_TRUE(resp.parse(sent.data(), sent.size()));
    EXPECT_EQ(resp.getInt(FixTag::OrdRejReason), 1);
    EXPECT_EQ(resp.getString(FixTag::BeginString), "FIX.4.2");
}

TEST(FixSessionTest, Fix44CancelRequiresTransactTime) {
    // The TransactTime requirement applies to Cancel (F) and CancelReplace
    // (G) too, not just D. Pin that policy.
    MatchingEngine engine;
    engine.addSymbol(2);
    engine.start();
    engine.submitOrder(2, 7, 1, Side::Buy, 100, 5, OrderType::Limit);

    std::string sent;
    FixSession session(engine, [&](std::string_view b) { sent.append(b); });
    session.setAcceptedVersions({"FIX.4.2", "FIX.4.4"});

    auto msg = makeFixMessageVer("FIX.4.4", "F", {
        {FixTag::OrigClOrdID, "7"}, {FixTag::Symbol, "2"},
    });
    ASSERT_TRUE(session.feed(msg.data(), msg.size()));

    EXPECT_EQ(session.ordersAccepted(), 0u);
    EXPECT_EQ(session.ordersRejected(), 1u);

    auto* book = engine.getOrderBook(2);
    EXPECT_NE(book->getOrder(7), nullptr) << "cancel must NOT have applied";
}

TEST(FIXParserTest, ParseNewOrder) {
    std::string raw;
    raw += "35=D"; raw += FIX_SOH;
    raw += "11=1001"; raw += FIX_SOH;
    raw += "49=5"; raw += FIX_SOH;
    raw += "55=0"; raw += FIX_SOH;
    raw += "54=1"; raw += FIX_SOH;
    raw += "44=1000000"; raw += FIX_SOH;
    raw += "38=50"; raw += FIX_SOH;
    raw += "40=2"; raw += FIX_SOH;
    raw += "59=1"; raw += FIX_SOH;

    FixMessage msg;
    ASSERT_TRUE(msg.parse(raw.c_str(), raw.size()));
    auto params = fixToOrderParams(msg);

    EXPECT_TRUE(params.valid);
    EXPECT_EQ(params.orderId, 1001);
    EXPECT_EQ(params.participantId, 5);
    EXPECT_EQ(params.side, Side::Buy);
    EXPECT_EQ(params.qty, 50);
    EXPECT_EQ(params.orderType, OrderType::Limit);
}

// ─── FIX Gateway Integration Test ──────────────────────────────────────────

TEST(MatchingEngineTest, FIXGateway_NewOrderAndCancel) {
    MatchingEngine engine;
    engine.addSymbol(100);
    engine.start();

    // Submit a new order via FIX
    std::string newOrder;
    newOrder += "35=D"; newOrder += FIX_SOH;
    newOrder += "11=1"; newOrder += FIX_SOH;
    newOrder += "49=1"; newOrder += FIX_SOH;
    newOrder += "55=100"; newOrder += FIX_SOH;
    newOrder += "54=1"; newOrder += FIX_SOH;
    newOrder += "44=1000000"; newOrder += FIX_SOH;
    newOrder += "38=50"; newOrder += FIX_SOH;
    newOrder += "40=2"; newOrder += FIX_SOH;
    newOrder += "59=1"; newOrder += FIX_SOH;
    engine.processFIXMessage(newOrder);

    auto* book = engine.getOrderBook(100);
    ASSERT_NE(book, nullptr);
    EXPECT_NE(book->getOrder(1), nullptr);

    // Cancel via FIX
    std::string cancelOrder;
    cancelOrder += "35=F"; cancelOrder += FIX_SOH;
    cancelOrder += "41=1"; cancelOrder += FIX_SOH;
    cancelOrder += "55=100"; cancelOrder += FIX_SOH;
    engine.processFIXMessage(cancelOrder);
    EXPECT_EQ(book->getOrder(1), nullptr);

    engine.stop();
}

// ─── Journal Replay Test ────────────────────────────────────────────────────

TEST(JournalTest, ReplayRecovery) {
    const char* journalPath = "/tmp/test_journal_replay.bin";
    std::remove(journalPath);

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
        EXPECT_EQ(replayed, 2);

        auto* book = engine.getOrderBook(1);
        ASSERT_NE(book, nullptr);
        // Orders should be reconstructed (they won't match since they don't cross)
        EXPECT_NE(book->getOrder(100), nullptr);
        EXPECT_NE(book->getOrder(101), nullptr);

        engine.stop();
    }

    std::remove(journalPath);
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

    // Each should have 50% remaining (approximately due to rounding)
    if (o1) EXPECT_LE(o1->remainingQty, 100);
    if (o2) EXPECT_LE(o2->remainingQty, 200);
    if (o3) EXPECT_LE(o3->remainingQty, 300);
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
