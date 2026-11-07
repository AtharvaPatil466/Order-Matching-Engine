// MarketNotionalTest — roadmap 1.9 I4, STP-12.
//
// A Market or stop order carries no limit, so every notional cap read its price
// as 0 and let it through at any size. It is now valued at the highest price it
// can trade at: a buy at the collar, a sell at the touch; a stop from its trigger;
// the reference price where there is no collar. An order with no price to be
// valued at is refused by a cap, not waved through.

#include "MatchingEngine.h"
#include "OrderBook.h"

#include <gtest/gtest.h>

#include <limits>
#include <variant>

using namespace OrderMatcher;

namespace {

constexpr SymbolId kSym = 1;
constexpr ParticipantId kTrader = 9;
constexpr ParticipantId kMaker = 1;
constexpr Price kPx = 1'000'000;   // 100.0000
constexpr Quantity kQty = 10;      // notional 1,000 at kPx, 1,050 at the 5% collar

struct Admission {
    OrderType type;
    Side side;
    Price price = 0;
    Price stopPrice = 0;
    Price stopLimitPrice = 0;
    Price trailAmount = 0;
};

AddOrderResult submit(OrderBook& b, OrderId id, const Admission& a) {
    return b.addOrder(id, kTrader, a.side, a.price, kQty, a.type, a.stopPrice, 0,
                      TimeInForce::GTC, 0, a.stopLimitPrice, PegType::None, 0, a.trailAmount);
}

// Passes at `value`, refused one below it.
void expectValuedAt(const char* what, __int128 value, const Admission& a,
                    bool collarOn = true, Side restSide = Side::Sell) {
    SCOPED_TRACE(what);
    for (const Price cap : {static_cast<Price>(value) - 1, static_cast<Price>(value)}) {
        OrderBook b(kSym);
        if (!collarOn) b.setMarketProtectionPct(0.0);
        ASSERT_TRUE(std::holds_alternative<OrderId>(
            b.addOrder(1, kMaker, restSide, kPx, 1000, OrderType::Limit)));
        RiskLimits limits;
        limits.maxOrderNotional = cap;
        b.setRiskLimits(kTrader, limits);

        const AddOrderResult r = submit(b, 2, a);
        if (cap < value) {
            ASSERT_TRUE(std::holds_alternative<RejectReason>(r)) << "cap " << cap;
            EXPECT_EQ(std::get<RejectReason>(r), RejectReason::RiskLimitBreached);
        } else {
            EXPECT_TRUE(std::holds_alternative<OrderId>(r)) << "cap " << cap;
        }
    }
}

TEST(BookNotional, MarketBuyAtTheCollar) {
    expectValuedAt("market buy", 1050, {OrderType::Market, Side::Buy});
}

TEST(BookNotional, MarketSellAtTheTouch) {
    expectValuedAt("market sell", 1000, {OrderType::Market, Side::Sell}, true, Side::Buy);
}

TEST(BookNotional, MarketWithoutACollarAtTheReferencePrice) {
    expectValuedAt("collar off", 1000, {OrderType::Market, Side::Buy}, /*collarOn=*/false);
    // Collar on but nothing opposite: the resting bid seeded the reference.
    expectValuedAt("empty side", 1000, {OrderType::Market, Side::Buy}, true, Side::Buy);
}

TEST(BookNotional, MocIsValuedLikeAMarketOrder) {
    expectValuedAt("moc", 1050, {OrderType::MOC, Side::Buy});
}

TEST(BookNotional, StopAndMitFromTheirTrigger) {
    expectValuedAt("stop buy", 1050, {OrderType::Stop, Side::Buy, 0, kPx});
    expectValuedAt("stop sell", 1000, {OrderType::Stop, Side::Sell, 0, kPx});
    expectValuedAt("mit buy", 1050, {OrderType::MIT, Side::Buy, 0, kPx});
    expectValuedAt("mit sell", 1000, {OrderType::MIT, Side::Sell, 0, kPx});
    expectValuedAt("stop buy, collar off", 1000, {OrderType::Stop, Side::Buy, 0, kPx},
                   /*collarOn=*/false);
}

TEST(BookNotional, StopNearTheTopOfThePriceRangeDoesNotOverflow) {
    OrderBook b(kSym);
    RiskLimits limits;
    limits.maxOrderNotional = 1000;
    b.setRiskLimits(kTrader, limits);
    const AddOrderResult r = submit(b, 2, {OrderType::Stop, Side::Buy, 0,
                                           std::numeric_limits<Price>::max() - 1});
    EXPECT_EQ(std::get<RejectReason>(r), RejectReason::RiskLimitBreached);
}

TEST(BookNotional, StopLimitAtItsLimit) {
    expectValuedAt("stop limit", 1100, {OrderType::StopLimit, Side::Buy, 0, kPx, 1'100'000});
}

TEST(BookNotional, TrailingStopAtItsInitialTrigger) {
    expectValuedAt("trailing buy", 1010,
                   {OrderType::TrailingStop, Side::Buy, kPx, 0, 0, 10'000});
    expectValuedAt("trailing sell", 990,
                   {OrderType::TrailingStop, Side::Sell, kPx, 0, 0, 10'000});
}

TEST(BookNotional, LimitOrdersStillAtTheirLimit) {
    expectValuedAt("limit", 1000, {OrderType::Limit, Side::Buy, kPx});
}

TEST(BookNotional, NoPriceToValueItAtIsRefusedByACap) {
    OrderBook capped(kSym);
    RiskLimits limits;
    limits.maxOrderNotional = 1'000'000;
    capped.setRiskLimits(kTrader, limits);
    const AddOrderResult r = submit(capped, 2, {OrderType::Market, Side::Buy});
    ASSERT_TRUE(std::holds_alternative<RejectReason>(r));
    EXPECT_EQ(std::get<RejectReason>(r), RejectReason::RiskLimitBreached);

    // With no cap nothing is judged.
    OrderBook free(kSym);
    EXPECT_TRUE(std::holds_alternative<OrderId>(submit(free, 2, {OrderType::Market, Side::Buy})));
}

// ─── The engine's own caps, on both paths ───────────────────────────────────

class EngineNotional : public ::testing::TestWithParam<bool> {
protected:
    MatchingEngine engine;
    bool async() const { return GetParam(); }
    void SetUp() override {
        engine.addSymbol(kSym);
        if (async()) engine.startAsync(1); else engine.start();
    }
    void TearDown() override {
        if (async()) engine.stopAsync(); else engine.stop();
    }
    void restSell(OrderId id = 1) {  // notional 1,000: inside every cap below
        ASSERT_TRUE(engine.submitOrder(kSym, id, kMaker, Side::Sell, kPx, kQty, OrderType::Limit)
                        .isAccepted());
    }
    // Refused at submit on the sync path; the async path answers at enqueue and
    // the worker refuses it.
    void expectRefused(const SubmitResult& r, OrderId id, RejectReason reason) {
        if (!async()) EXPECT_EQ(r.rejectReason, reason);
        engine.waitForDrain();
        EXPECT_EQ(engine.getOrderBook(kSym)->getOrder(id), nullptr);
        EXPECT_EQ(engine.getOrderBook(kSym)->getOrder(1)->remainingQty, kQty) << "it traded";
    }
    void expectRefusedEmpty(const SubmitResult& r) {
        if (!async()) EXPECT_EQ(r.rejectReason, RejectReason::FatFingerReject);
        engine.waitForDrain();
    }
    SubmitResult marketBuy(OrderId id) {
        return engine.submitOrder(kSym, id, kTrader, Side::Buy, 0, kQty, OrderType::Market);
    }
    SubmitResult stopBuy(OrderId id) {
        return engine.submitOrder(kSym, id, kTrader, Side::Buy, 0, kQty, OrderType::Stop, kPx);
    }
};

TEST_P(EngineNotional, FatFingerCapValuesMarketAtTheCollar) {
    engine.setFatFingerLimits(kSym, 0, 0.0, 1049);
    restSell();
    expectRefused(marketBuy(2), 2, RejectReason::FatFingerReject);
    EXPECT_EQ(engine.getFatFingerRejectCount(), 1u);
}

TEST_P(EngineNotional, FatFingerCapValuesAStopFromItsTrigger) {
    engine.setFatFingerLimits(kSym, 0, 0.0, 1049);
    restSell();
    expectRefused(stopBuy(2), 2, RejectReason::FatFingerReject);
}

TEST_P(EngineNotional, FatFingerCapAdmitsWhatFits) {
    engine.setFatFingerLimits(kSym, 0, 0.0, 1050);
    restSell(1);
    restSell(4);
    EXPECT_TRUE(marketBuy(2).isAccepted());
    EXPECT_TRUE(stopBuy(3).isAccepted());
    engine.waitForDrain();
    EXPECT_EQ(engine.getOrderBook(kSym)->getOrder(1), nullptr) << "the market order traded";
    EXPECT_NE(engine.getOrderBook(kSym)->getOrder(3), nullptr) << "the stop parked";
    EXPECT_EQ(engine.getFatFingerRejectCount(), 0u);
}

TEST_P(EngineNotional, FatFingerCapFallsBackToTheEnginesReferencePrice) {
    engine.setFatFingerLimits(kSym, 0, 0.0, 999);
    engine.setReferencePrice(kSym, kPx);   // the book has no touch and no reference
    expectRefusedEmpty(marketBuy(2));
    engine.setFatFingerLimits(kSym, 0, 0.0, 1000);
    EXPECT_TRUE(marketBuy(3).isAccepted());
    engine.waitForDrain();
    EXPECT_EQ(engine.getFatFingerRejectCount(), 1u);
}

TEST_P(EngineNotional, FatFingerCapRefusesWhatItCannotValue) {
    engine.setFatFingerLimits(kSym, 0, 0.0, 1'000'000);
    const SubmitResult r = marketBuy(2);
    if (!async()) EXPECT_EQ(r.rejectReason, RejectReason::FatFingerReject);
    engine.waitForDrain();
    EXPECT_EQ(engine.getFatFingerRejectCount(), 1u);
}

// HierarchicalRiskManager's maxOrderNotional, on the path that calls it.
TEST(EngineNotionalHierarchy, AsyncWorkerValuesMarketAtTheCollar) {
    MatchingEngine engine;
    engine.addSymbol(kSym);
    TierLimits limits;
    limits.maxOrderNotional = 1049;
    engine.mapParticipant(kTrader, 1, 1, 1);
    engine.setRiskTierLimits(RiskTier::Trader, kTrader, limits);
    engine.startAsync(1);
    ASSERT_TRUE(engine.submitOrder(kSym, 1, kMaker, Side::Sell, kPx, 1000, OrderType::Limit)
                    .isAccepted());
    ASSERT_TRUE(engine.submitOrder(kSym, 2, kTrader, Side::Buy, 0, kQty, OrderType::Market)
                    .isAccepted());
    engine.waitForDrain();
    EXPECT_EQ(engine.getOrderBook(kSym)->getOrder(1)->remainingQty, 1000u) << "it traded";
    engine.stopAsync();
}

INSTANTIATE_TEST_SUITE_P(SyncAndAsync, EngineNotional, ::testing::Values(false, true));

}  // namespace
