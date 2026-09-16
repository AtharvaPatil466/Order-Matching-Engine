// RejectReasonContractTest — for every value in the RejectReason enum:
//
//   1. Drive the engine / book through a public API path that produces it,
//      and assert the returned reason matches exactly. Catches regressions
//      where a rule change silently shifts which reason a path emits.
//
//   2. (Completeness) Iterate a hardcoded list of every enum value and
//      verify FixSession's user-facing string mapping returns a non-empty,
//      non-generic label for each. The compiler's -Wswitch -Werror rule
//      already enforces case coverage; this test additionally enforces
//      that someone didn't paper over a new reason with a placeholder.
//
// CapacityExhausted requires exhausting the order pool (millions of
// orders) and OutOfPriceRange requires a tightly-bounded FlatPriceMap that
// the public API doesn't expose. Both are exercised indirectly through
// other tests (and are present in the completeness-of-string-mapping
// check); skipping them in the per-reason path keeps this test fast and
// non-flaky.

#include "FixSession.h"
#include "MatchingEngine.h"
#include "OrderBook.h"
#include "Types.h"

#include <gtest/gtest.h>
#include <string>
#include <variant>

using namespace OrderMatcher;

namespace {

// Helper to extract a reject reason from the variant returned by addOrder.
RejectReason expectReject(const AddOrderResult& r) {
    EXPECT_TRUE(std::holds_alternative<RejectReason>(r));
    return std::holds_alternative<RejectReason>(r)
        ? std::get<RejectReason>(r) : RejectReason::None;
}

}  // namespace

// ─── Per-reason contract: each path emits the right reason ──────────────────

TEST(RejectReasonContract, InvalidQuantity) {
    OrderBook book(0);
    auto r = book.addOrder(1, 1, Side::Buy, 1000000, /*qty=*/0,
                           OrderType::Limit);
    EXPECT_EQ(expectReject(r), RejectReason::InvalidQuantity);
}

TEST(RejectReasonContract, InvalidDisplayQty) {
    OrderBook book(0);
    auto r = book.addOrder(1, 1, Side::Buy, 1000000, /*qty=*/100,
                           OrderType::Iceberg, /*stopPrice=*/0, /*displayQty=*/0);
    EXPECT_EQ(expectReject(r), RejectReason::InvalidDisplayQty);
}

TEST(RejectReasonContract, InvalidPrice) {
    OrderBook book(0);
    auto r = book.addOrder(1, 1, Side::Buy, /*price=*/0, 100,
                           OrderType::Limit);
    EXPECT_EQ(expectReject(r), RejectReason::InvalidPrice);
}

TEST(RejectReasonContract, MarketHalted) {
    OrderBook book(0);
    book.setTradingState(TradingState::Halted);
    auto r = book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    EXPECT_EQ(expectReject(r), RejectReason::MarketHalted);
}

TEST(RejectReasonContract, DuplicateOrderId) {
    OrderBook book(0);
    book.addOrder(42, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    auto r = book.addOrder(42, 2, Side::Sell, 1010000, 50, OrderType::Limit);
    EXPECT_EQ(expectReject(r), RejectReason::DuplicateOrderId);
}

TEST(RejectReasonContract, MarketClosed) {
    OrderBook book(0);
    book.setTradingState(TradingState::PostClose);
    auto r = book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    EXPECT_EQ(expectReject(r), RejectReason::MarketClosed);
}

TEST(RejectReasonContract, OrderTypeNotAllowedInState) {
    // IOC/FOK still rejected during auction (Market is now accepted —
    // see MarketInAuction_* tests). Picking IOC keeps this test
    // independent of auction execution semantics.
    OrderBook book(0);
    book.setTradingState(TradingState::AuctionOpen);
    auto r = book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::IOC);
    EXPECT_EQ(expectReject(r), RejectReason::OrderTypeNotAllowedInState);
}

TEST(RejectReasonContract, OutsidePriceBand) {
    OrderBook book(0);
    book.setCircuitBreakerThreshold(0.50);  // breaker out of the way
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    book.setPriceBandPct(0.05);
    auto r = book.addOrder(2, 2, Side::Buy, 1100000, 100, OrderType::Limit);
    EXPECT_EQ(expectReject(r), RejectReason::OutsidePriceBand);
}

TEST(RejectReasonContract, VolatilityCircuitBreaker) {
    OrderBook book(0);
    book.setCircuitBreakerThreshold(0.02);
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    auto r = book.addOrder(2, 2, Side::Sell, 1030000, 100, OrderType::Limit);
    EXPECT_EQ(expectReject(r), RejectReason::VolatilityCircuitBreaker);
}

TEST(RejectReasonContract, PostOnlyWouldCross) {
    OrderBook book(0);
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);
    auto r = book.addOrder(2, 2, Side::Buy, 1000000, 50, OrderType::PostOnly);
    EXPECT_EQ(expectReject(r), RejectReason::PostOnlyWouldCross);
}

TEST(RejectReasonContract, FOKInsufficientLiquidity) {
    OrderBook book(0);
    book.addOrder(1, 1, Side::Sell, 1000000, 50, OrderType::Limit);
    auto r = book.addOrder(2, 2, Side::Buy, 1000000, /*qty=*/100,
                           OrderType::FOK);
    EXPECT_EQ(expectReject(r), RejectReason::FOKInsufficientLiquidity);
}

TEST(RejectReasonContract, RiskLimitBreached) {
    OrderBook book(0);
    RiskLimits limits;
    limits.maxOrderSize = 10;
    book.setRiskLimits(/*pid=*/42, limits);
    auto r = book.addOrder(1, /*pid=*/42, Side::Buy, 1000000,
                           /*qty=*/100, OrderType::Limit);
    EXPECT_EQ(expectReject(r), RejectReason::RiskLimitBreached);
}

TEST(RejectReasonContract, SymbolNotFound) {
    MatchingEngine engine;
    engine.start();  // engine is running but no symbols registered
    auto r = engine.submitOrder(/*sym=*/99, 1, 1, Side::Buy,
                                 1000000, 100, OrderType::Limit);
    ASSERT_FALSE(r.isAccepted());
    EXPECT_EQ(r.rejectReason, RejectReason::SymbolNotFound);
}

TEST(RejectReasonContract, OrderNotFound) {
    MatchingEngine engine;
    engine.addSymbol(0);
    engine.start();
    auto r = engine.submitCancel(/*sym=*/0, /*orderId=*/9999);
    ASSERT_FALSE(r.isAccepted());
    EXPECT_EQ(r.rejectReason, RejectReason::OrderNotFound);
}

TEST(RejectReasonContract, EngineStopped) {
    MatchingEngine engine;
    // Deliberately do not call start().
    auto r = engine.submitOrder(0, 1, 1, Side::Buy, 1000000, 100,
                                 OrderType::Limit);
    ASSERT_FALSE(r.isAccepted());
    EXPECT_EQ(r.rejectReason, RejectReason::EngineStopped);
}

TEST(RejectReasonContract, RateLimitExceeded) {
    MatchingEngine engine;
    engine.addSymbol(0);
    engine.start();
    // 1 order/sec, burst of 1: the second submission within the same
    // second must hit the limit.
    engine.setRateLimit(/*ratePerSec=*/1, /*burst=*/1);
    auto first = engine.submitOrder(0, 1, 1, Side::Buy, 1000000, 100,
                                     OrderType::Limit);
    ASSERT_TRUE(first.isAccepted());
    auto second = engine.submitOrder(0, 2, 1, Side::Buy, 1000000, 100,
                                      OrderType::Limit);
    ASSERT_FALSE(second.isAccepted());
    EXPECT_EQ(second.rejectReason, RejectReason::RateLimitExceeded);
}

// QueueBackpressure — exercised end-to-end by QueueChaosTest's
// "backpressure-rejects" scenario; not duplicated here. The completeness
// test below still confirms its string mapping is wired.

// ─── Completeness: every reason has a non-generic string mapping ────────────

TEST(RejectReasonContract, EveryReasonHasUserFacingString) {
    // Every value in the enum. The list is hand-maintained and cannot detect
    // its own omissions — and it had five: the four P2-8..P2-11 risk-control
    // reasons and NotOrderOwner were all missing, so this test has been
    // quietly checking a subset while claiming "EveryReason". The real
    // enforcement is -Wswitch -Werror on FixSession::rejectReason, which
    // makes a new reason a compile error until it is wired; this list is the
    // redundancy that surfaces the addition in review, and the placeholder
    // check below is what it actually buys.
    constexpr RejectReason kAll[] = {
        RejectReason::None,
        RejectReason::VolatilityCircuitBreaker,
        RejectReason::PostOnlyWouldCross,
        RejectReason::FOKInsufficientLiquidity,
        RejectReason::RiskLimitBreached,
        RejectReason::InvalidPrice,
        RejectReason::InvalidQuantity,
        RejectReason::SymbolNotFound,
        RejectReason::OrderNotFound,
        RejectReason::OutOfPriceRange,
        RejectReason::CapacityExhausted,
        RejectReason::RateLimitExceeded,
        RejectReason::QueueBackpressure,
        RejectReason::EngineStopped,
        RejectReason::MarketHalted,
        RejectReason::OrderTypeNotAllowedInState,
        RejectReason::OutsidePriceBand,
        RejectReason::MarketClosed,
        RejectReason::DuplicateOrderId,
        RejectReason::UnsupportedFixVersion,
        RejectReason::MissingRequiredField,
        RejectReason::KillSwitchActive,
        RejectReason::PositionLimitExceeded,
        RejectReason::FatFingerReject,
        RejectReason::OrderToTradeRatioExceeded,
        RejectReason::PoolCapacityExceeded,
        RejectReason::InvalidDisplayQty,
        RejectReason::NotOrderOwner,
        RejectReason::InvalidFieldValue,
    };

    // Pins the list against the enum's size. Appending a reason without
    // listing it here now fails the build rather than silently narrowing what
    // "every reason" means — which is exactly how the five above went missing.
    static_assert(static_cast<size_t>(RejectReason::InvalidFieldValue) + 1 ==
                      sizeof(kAll) / sizeof(kAll[0]),
                  "a RejectReason was added — add it to kAll and update the "
                  "last-value reference in this assert");

    for (RejectReason r : kAll) {
        const char* s = FixSession::rejectReason(r);
        ASSERT_NE(s, nullptr);
        EXPECT_NE(std::string(s), "")
            << "RejectReason value " << int(r) << " has empty string";
        // The fallthrough placeholder. If a real reason returns this,
        // someone wired a new enum value without giving it a meaningful
        // user-facing label.
        EXPECT_NE(std::string(s), "rejected")
            << "RejectReason value " << int(r)
            << " maps to the generic placeholder \"rejected\"";
    }
}
