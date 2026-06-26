// CriticalFixesTest — regression tests for the Critical correctness bugs found
// in the repository audit (AUDIT_REPORT.md, section 1). Each test is written to
// FAIL against the pre-fix code and PASS once the corresponding fix lands.
//
//   Fix #1 — notional overflow bypasses the order-notional risk cap (OrderBook.cpp:124)
//   Fix #2 — MIT cancel leaves a dangling pointer in stopOrders_ (OrderBook.cpp:1035)
//   Fix #3 — TrailingStop stopPrice underflow when trailAmount > ref (OrderBook.cpp:521/1311)

#include <gtest/gtest.h>
#include "OrderBook.h"
#include "Types.h"

#include <variant>

using namespace OrderMatcher;

namespace {

// Counts how many Cancelled OrderUpdates were emitted for a given order id.
class UpdateCountingListener : public EventListener {
public:
    void onTrade(const Trade&) override {}
    void onOrderUpdate(const OrderUpdate& u) override { updates.push_back(u); }
    void onMarketData(const MarketDataUpdate&) override {}

    int cancelledFor(OrderId id) const {
        int n = 0;
        for (const auto& u : updates)
            if (u.orderId == id && u.status == OrderStatus::Cancelled) ++n;
        return n;
    }
    std::vector<OrderUpdate> updates;
};

} // namespace

// ─── Fix #1: notional overflow must not bypass the risk limit ────────────────
//
// checkRiskLimits computed `price * (Price)qty / PRICE_PRECISION` in int64.
// For a large order the multiply overflows int64 (UB) and wraps negative, so
// the naive comparison `notional > maxOrderNotional` is false and the cap is
// silently bypassed. price=1e11 × qty=1e8 = 1e19 > INT64_MAX; the TRUE notional
// is 1e15, far above the 1e9 cap, so the order MUST be rejected.
TEST(CriticalFixes, NotionalOverflowDoesNotBypassRiskLimit) {
    OrderBook book(0);

    RiskLimits limits;
    limits.maxOrderSize = 0;                 // no size cap
    limits.maxOrderNotional = 1'000'000'000; // 1e9 notional cap
    limits.maxPositionSize = 0;              // no position cap
    book.setRiskLimits(/*participantId=*/7, limits);

    // IOC so the (buggy) accept path cannot rest an absurd price in the book.
    auto result = book.addOrder(/*id=*/1, /*pid=*/7, Side::Buy,
                                /*price=*/100'000'000'000LL,
                                /*qty=*/100'000'000ULL,
                                OrderType::IOC);

    ASSERT_TRUE(std::holds_alternative<RejectReason>(result))
        << "order with notional 1e15 must be rejected, not accepted";
    EXPECT_EQ(std::get<RejectReason>(result), RejectReason::RiskLimitBreached);
}

// ─── Fix #2: cancelling a parked MIT must remove it from stopOrders_ ─────────
//
// MIT orders park in stopOrders_, but cancelOrderImpl only removed Stop/
// StopLimit — leaving a dangling pointer that checkStopOrders() re-fires
// (phantom Cancelled + double-deallocate; aborts under the pool's -UNDEBUG
// double-free assert). The decoy is cancelled second so its slot sits on top
// of the LIFO free list, preserving the MIT's slot so the dangling fire is
// deterministic.
TEST(CriticalFixes, CancellingMitOrderDoesNotDanglingFire) {
    OrderBook book(0);
    UpdateCountingListener lis;
    book.setEventListener(&lis);
    // resting buy that the aggressor will cross to produce a trade
    // (also establishes the circuit-breaker reference price at 105)
    book.addOrder(1, 1, Side::Buy, 105, 10, OrderType::Limit);
    // MIT sell, triggers on upward touch of 105
    book.addOrder(2, 2, Side::Sell, 0, 30, OrderType::MIT, /*stopPrice=*/105);
    // decoy resting sell, harmless, near the market so it is admitted (won't cross)
    book.addOrder(99, 3, Side::Sell, 107, 1, OrderType::Limit);
    // cancel the MIT (real Cancelled #1 for id 2), then cancel the decoy so the
    // decoy's slot sits on TOP of the LIFO free list, preserving the MIT's slot
    book.cancelOrder(2);
    book.cancelOrder(99);
    ASSERT_EQ(book.getOrder(2), nullptr);
    // aggressor sell crosses resting buy @105 -> trade@105 -> checkStopOrders(105).
    // Fixed: MIT was removed from stopOrders_ at cancel -> no phantom.
    // Buggy: dangling MIT re-fires -> double free (abort under -UNDEBUG), or a
    // SECOND Cancelled for id 2 if assertions are off.
    book.addOrder(4, 4, Side::Sell, 105, 10, OrderType::Limit);
    EXPECT_EQ(lis.cancelledFor(2), 1) << "cancelled MIT must not re-fire from stopOrders_";
}
