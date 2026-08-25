#include <iostream>
#include <cassert>
#include <random>
#include <vector>
#include <algorithm>
#include <deque>
#include <cmath>
#include "OrderBook.h"
#include "MatchingEngine.h"
#include "Journal.h"
#include "FIXParser.h"
#include "Types.h"
#include <thread>

using namespace OrderMatcher;

// ═══════════════════════════════════════════════════════════════════════════════
// Original Tests (preserved)
// ═══════════════════════════════════════════════════════════════════════════════

struct TestListener : public OrderMatcher::EventListener {
    std::vector<OrderMatcher::Trade> trades;
    std::vector<OrderMatcher::OrderUpdate> updates;
    void onTrade(const OrderMatcher::Trade& t) override { trades.push_back(t); }
    void onOrderUpdate(const OrderMatcher::OrderUpdate& u) override { updates.push_back(u); }
    void onMarketData(const OrderMatcher::MarketDataUpdate&) override {}
};

void testBasicMatching() {
    std::cout << "Running testBasicMatching..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);

    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);
    book.addOrder(2, 2, Side::Buy, 1000000, 100, OrderType::Limit);

    assert(book.getOrder(1) == nullptr);
    assert(book.getOrder(2) == nullptr);
    assert(book.getAskLevelsCount() == 0);
    assert(book.getBidLevelsCount() == 0);
    std::cout << "testBasicMatching PASSED" << std::endl;
}

void testSMP() {
    std::cout << "Running testSMP..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);

    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);
    book.addOrder(2, 1, Side::Buy, 1000000, 100, OrderType::Limit);

    assert(book.getOrder(1) != nullptr);
    assert(book.getOrder(2) == nullptr);
    assert(book.getAskLevelsCount() == 1);
    std::cout << "testSMP PASSED" << std::endl;
}

void testMarketOrder() {
    std::cout << "Running testMarketOrder..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);
    book.addOrder(2, 2, Side::Sell, 1010000, 100, OrderType::Limit);

    book.addOrder(3, 3, Side::Buy, 0, 150, OrderType::Market);

    assert(book.getOrder(1) == nullptr);
    const Order* order2 = book.getOrder(2);
    assert(order2 != nullptr && order2->remainingQty == 50);
    assert(book.getAskLevelsCount() == 1);
    std::cout << "testMarketOrder PASSED" << std::endl;
}

void testIOC() {
    std::cout << "Running testIOC..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);

    book.addOrder(2, 2, Side::Buy, 1000000, 150, OrderType::IOC);

    assert(book.getOrder(1) == nullptr);
    assert(book.getOrder(2) == nullptr);
    assert(book.getBidLevelsCount() == 0);
    std::cout << "testIOC PASSED" << std::endl;
}

void testFOK() {
    std::cout << "Running testFOK..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);

    book.addOrder(2, 2, Side::Buy, 1000000, 150, OrderType::FOK);

    assert(book.getOrder(1) != nullptr);
    assert(book.getOrder(2) == nullptr);

    book.addOrder(3, 3, Side::Buy, 1000000, 100, OrderType::FOK);
    assert(book.getOrder(1) == nullptr);
    assert(book.getOrder(3) == nullptr);
    std::cout << "testFOK PASSED" << std::endl;
}

void testStopOrder() {
    std::cout << "Running testStopOrder..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);

    book.addOrder(1, 1, Side::Buy, 1050000, 100, OrderType::Stop, 1050000);

    book.addOrder(2, 2, Side::Sell, 1040000, 50, OrderType::Limit);
    book.addOrder(3, 3, Side::Buy, 1040000, 50, OrderType::Limit);
    assert(book.getOrder(1) != nullptr);

    book.addOrder(4, 4, Side::Sell, 1050000, 50, OrderType::Limit);
    book.addOrder(5, 5, Side::Buy, 1050000, 50, OrderType::Limit);

    assert(book.getBidLevelsCount() == 1);
    const Order* order1 = book.getOrder(1);
    assert(order1 != nullptr && order1->type == OrderType::Limit);
    std::cout << "testStopOrder PASSED" << std::endl;
}

void testIcebergOrder() {
    std::cout << "Running testIcebergOrder..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);

    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Iceberg, 0, 30);

    book.addOrder(2, 2, Side::Buy, 1000000, 20, OrderType::Limit);
    const Order* order1 = book.getOrder(1);
    assert(order1->visibleQty == 10);
    assert(order1->remainingQty == 80);

    book.addOrder(3, 3, Side::Buy, 1000000, 15, OrderType::Limit);
    assert(order1->visibleQty == 25);
    assert(order1->remainingQty == 65);
    std::cout << "testIcebergOrder PASSED" << std::endl;
}

void testAnalytics() {
    std::cout << "Running testAnalytics..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);

    book.addOrder(1, 10, Side::Sell, 1000000, 100, OrderType::Limit);
    book.addOrder(2, 20, Side::Buy, 1000000, 100, OrderType::Limit);
    assert(book.getVWAP() == 1000000);

    book.addOrder(3, 10, Side::Sell, 1020000, 100, OrderType::Limit);
    book.addOrder(4, 30, Side::Buy, 1020000, 100, OrderType::Limit);
    assert(book.getVWAP() == 1010000);

    assert(book.getParticipantStats(10).ordersSubmitted == 2);
    assert(book.getParticipantStats(10).tradesExecuted == 2);
    assert(book.getParticipantStats(30).ordersSubmitted == 1);
    assert(book.getParticipantStats(30).tradesExecuted == 1);
    assert(book.getOTR(10) == 1.0);
    assert(book.getOTR(30) == 1.0);

    std::cout << "testAnalytics PASSED" << std::endl;
}

void testCircuitBreaker() {
    std::cout << "Running testCircuitBreaker..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);

    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    // A buy 6% above the 5% default band trips the breaker → volatility
    // auction (not a hard halt); the triggering order itself is rejected.
    book.addOrder(2, 2, Side::Buy, 1060000, 100, OrderType::Limit);
    assert(book.getTradingState() == TradingState::VolatilityAuction);
    assert(book.getOrder(2) == nullptr);

    // In-band orders now accumulate into the auction instead of being
    // rejected; they rest until a reopening cross.
    book.addOrder(3, 3, Side::Sell, 1000000, 100, OrderType::Limit);
    assert(book.getOrder(3) != nullptr);
    std::cout << "testCircuitBreaker PASSED" << std::endl;
}

void testStress() {
    std::cout << "Running testStress (10,000 random ops)..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);
    std::mt19937 rng(1337);
    std::uniform_int_distribution<Price> priceDist(990000, 1010000);
    std::uniform_int_distribution<int> opDist(1, 100);
    std::vector<OrderId> orders;

    for (int i = 0; i < 10000; ++i) {
        int op = opDist(rng);
        if (op <= 70) {
            OrderId id = i + 1;
            orders.push_back(id);
            book.addOrder(id, 1, (i % 2 == 0 ? Side::Buy : Side::Sell), priceDist(rng), 10, OrderType::Limit);
        } else if (!orders.empty()) {
            size_t idx = rng() % orders.size();
            book.cancelOrder(orders[idx]);
            orders[idx] = orders.back();
            orders.pop_back();
        }
    }
    std::cout << "testStress PASSED" << std::endl;
}

void testPartialFillPriority() {
    std::cout << "Running testPartialFillPriority..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);

    book.addOrder(1, 10, Side::Buy, 1000000, 100, OrderType::Limit);
    book.addOrder(2, 20, Side::Buy, 1000000, 100, OrderType::Limit);

    book.addOrder(3, 30, Side::Sell, 1000000, 50, OrderType::Limit);
    assert(listener.trades.back().buyOrderId == 1);
    assert(listener.trades.back().quantity == 50);

    book.addOrder(4, 30, Side::Sell, 1000000, 100, OrderType::Limit);
    auto history = listener.trades;
    assert(history[history.size()-2].buyOrderId == 1);
    assert(history[history.size()-1].buyOrderId == 2);

    std::cout << "testPartialFillPriority PASSED" << std::endl;
}

void testFuzz(int numOps) {
    std::cout << "Running testFuzz (" << numOps << " ops)..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);
    std::mt19937 rng(42);
    std::uniform_int_distribution<Price> priceDist(990, 1010);
    std::uniform_int_distribution<int> opDist(1, 100);
    std::deque<OrderId> activeOrders;
    OrderId nextId = 1;

    for (int i = 0; i < numOps; ++i) {
        int op = opDist(rng);
        if (op <= 50) {
            OrderId id = nextId++;
            activeOrders.push_back(id);
            book.addOrder(id, (i%10)+1, (i%2==0 ? Side::Buy : Side::Sell), priceDist(rng)*1000, 100, OrderType::Limit);
        } else if (op <= 90 && !activeOrders.empty()) {
            size_t idx = rng() % activeOrders.size();
            book.cancelOrder(activeOrders[idx]);
            activeOrders.erase(activeOrders.begin() + idx);
        } else {
            book.addOrder(nextId++, 99, (i%2==0 ? Side::Buy : Side::Sell), 0, 50, OrderType::Market);
        }

        Price bb = book.getBestBid();
        Price ba = book.getBestAsk();
        if (bb != 0 && ba != std::numeric_limits<Price>::max()) {
            assert(bb < ba);
        }
    }
    std::cout << "testFuzz PASSED" << std::endl;
}

void testSMPPurity() {
    std::cout << "Running testSMPPurity..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    book.addOrder(2, 1, Side::Sell, 1000000, 100, OrderType::Limit);

    assert(listener.trades.size() == 0);
    assert(book.getBidLevelsCount() == 1);
    assert(book.getAskLevelsCount() == 0);

    book.addOrder(3, 2, Side::Sell, 1000000, 100, OrderType::Limit);
    assert(listener.trades.size() == 1);

    std::cout << "testSMPPurity PASSED" << std::endl;
}

void testCircuitBreakerBoundaries() {
    std::cout << "Running testCircuitBreakerBoundaries..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);

    book.addOrder(2, 2, Side::Sell, 1049000, 100, OrderType::Limit);
    assert(listener.trades.size() == 0);

    book.addOrder(3, 3, Side::Buy, 1051000, 100, OrderType::Limit);

    book.addOrder(4, 4, Side::Sell, 1000000, 100, OrderType::Limit);
    assert(listener.trades.size() == 0);

    std::cout << "testCircuitBreakerBoundaries PASSED" << std::endl;
}

// ═══════════════════════════════════════════════════════════════════════════════
// New Feature Tests
// ═══════════════════════════════════════════════════════════════════════════════

// ─── Stop-Limit Orders ──────────────────────────────────────────────────────

void testStopLimitOrder() {
    std::cout << "Running testStopLimitOrder..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);

    // Stop-Limit Buy: trigger at 105, limit at 104
    // stopPrice=1050000, stopLimitPrice=1040000
    book.addOrder(1, 1, Side::Buy, 1040000, 100, OrderType::StopLimit, 1050000, 0,
                  TimeInForce::GTC, 0, 1040000);

    // Trade at 104 -> no trigger
    book.addOrder(2, 2, Side::Sell, 1040000, 50, OrderType::Limit);
    book.addOrder(3, 3, Side::Buy, 1040000, 50, OrderType::Limit);
    assert(book.getOrder(1) != nullptr);

    // Trade at 105 -> TRIGGER! Order becomes Limit Buy @ 104
    book.addOrder(4, 4, Side::Sell, 1050000, 50, OrderType::Limit);
    book.addOrder(5, 5, Side::Buy, 1050000, 50, OrderType::Limit);

    const Order* order1 = book.getOrder(1);
    assert(order1 != nullptr);
    assert(order1->type == OrderType::Limit);
    assert(order1->price == 1040000);  // limit price, not stop price
    assert(order1->isStopTriggered == true);

    std::cout << "testStopLimitOrder PASSED" << std::endl;
}

// ─── Post-Only Orders ───────────────────────────────────────────────────────

void testPostOnly() {
    std::cout << "Running testPostOnly..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);

    // Resting sell @ 100
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);

    // Post-Only Buy @ 100 -> would cross -> REJECT
    book.addOrder(2, 2, Side::Buy, 1000000, 50, OrderType::PostOnly);
    assert(book.getOrder(2) == nullptr);
    assert(book.getOrder(1) != nullptr);
    assert(book.getOrder(1)->remainingQty == 100); // not matched

    // Post-Only Buy @ 99 -> won't cross -> ACCEPT
    book.addOrder(3, 3, Side::Buy, 990000, 50, OrderType::PostOnly);
    assert(book.getOrder(3) != nullptr);
    assert(book.getBidLevelsCount() == 1);

    // Post-Only Sell @ 101 -> won't cross -> ACCEPT
    book.addOrder(4, 4, Side::Sell, 1010000, 50, OrderType::PostOnly);
    assert(book.getOrder(4) != nullptr);

    // Post-Only Sell @ 99 -> would cross (bid at 99) -> REJECT
    book.addOrder(5, 5, Side::Sell, 990000, 50, OrderType::PostOnly);
    assert(book.getOrder(5) == nullptr);

    std::cout << "testPostOnly PASSED" << std::endl;
}

// ─── Hidden Orders ──────────────────────────────────────────────────────────

void testHiddenOrders() {
    std::cout << "Running testHiddenOrders..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);

    // Hidden buy @ 100
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Hidden);
    assert(book.getOrder(1) != nullptr);
    assert(book.getOrder(1)->isHidden == true);
    assert(book.getBidLevelsCount() == 1);

    // Market data snapshot should NOT show hidden orders
    auto snap = book.getSnapshot();
    assert(snap.bidCount == 0); // hidden order not visible

    // But hidden orders SHOULD match
    book.addOrder(2, 2, Side::Sell, 1000000, 50, OrderType::Limit);
    assert(book.getOrder(2) == nullptr); // matched
    const Order* o1 = book.getOrder(1);
    assert(o1 != nullptr && o1->remainingQty == 50);

    // Can also use hidden flag on limit orders
    book.addOrder(3, 3, Side::Sell, 1010000, 100, OrderType::Limit, 0, 0,
                  TimeInForce::GTC, 0, 0, PegType::None, 0, 0, 0, true);
    assert(book.getOrder(3) != nullptr);
    assert(book.getOrder(3)->isHidden == true);

    std::cout << "testHiddenOrders PASSED" << std::endl;
}

// ─── Cancel/Replace ─────────────────────────────────────────────────────────

void testCancelReplace() {
    std::cout << "Running testCancelReplace..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);

    // Buy 100 @ 100
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    assert(book.getBidLevelsCount() == 1);

    // Cancel/Replace: change price to 101, qty to 80
    bool ok = book.cancelReplace(1, 1010000, 80);
    assert(ok);
    const Order* o = book.getOrder(1);
    assert(o != nullptr);
    assert(o->price == 1010000);
    assert(o->remainingQty == 80);

    // Cancel/Replace: same price, reduce qty (preserves priority)
    ok = book.cancelReplace(1, 1010000, 50);
    assert(ok);
    assert(book.getOrder(1)->remainingQty == 50);

    // Cancel/Replace: same price, increase qty (loses priority)
    book.addOrder(2, 2, Side::Buy, 1010000, 100, OrderType::Limit);
    ok = book.cancelReplace(1, 1010000, 200);
    assert(ok);
    // Order 2 should now have priority over order 1

    // Sell 50 -> should match order 2 first (has priority)
    book.addOrder(3, 3, Side::Sell, 1010000, 50, OrderType::Limit);
    auto history = listener.trades;
    assert(history.back().buyOrderId == 2);

    std::cout << "testCancelReplace PASSED" << std::endl;
}

void testCancelReplaceCrossing() {
    std::cout << "Running testCancelReplaceCrossing..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);

    // Resting sell @ 100
    book.addOrder(1, 1, Side::Sell, 1000000, 50, OrderType::Limit);

    // Resting buy @ 99
    book.addOrder(2, 2, Side::Buy, 990000, 100, OrderType::Limit);

    // Cancel/Replace buy to 100 -> should cross and match
    bool ok = book.cancelReplace(2, 1000000, 100);
    assert(ok);

    // Sell order 1 should be fully filled
    assert(book.getOrder(1) == nullptr);

    // Buy order 2 should have 50 remaining
    const Order* o2 = book.getOrder(2);
    assert(o2 != nullptr);
    assert(o2->remainingQty == 50);
    assert(o2->price == 1000000);

    std::cout << "testCancelReplaceCrossing PASSED" << std::endl;
}

// ─── Kill Switch ────────────────────────────────────────────────────────────

void testKillSwitch() {
    std::cout << "Running testKillSwitch..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);

    // Participant 1: multiple orders
    book.addOrder(1, 1, Side::Buy, 990000, 100, OrderType::Limit);
    book.addOrder(2, 1, Side::Buy, 980000, 100, OrderType::Limit);
    book.addOrder(3, 1, Side::Sell, 1010000, 100, OrderType::Limit);

    // Participant 2: one order
    book.addOrder(4, 2, Side::Buy, 995000, 100, OrderType::Limit);

    assert(book.getBidLevelsCount() == 3);
    assert(book.getAskLevelsCount() == 1);

    // Kill switch for participant 1
    uint64_t cancelled = book.cancelAllForParticipant(1);
    assert(cancelled == 3);

    assert(book.getOrder(1) == nullptr);
    assert(book.getOrder(2) == nullptr);
    assert(book.getOrder(3) == nullptr);
    assert(book.getOrder(4) != nullptr);
    assert(book.getBidLevelsCount() == 1); // only participant 2's order remains
    assert(book.getAskLevelsCount() == 0);

    std::cout << "testKillSwitch PASSED" << std::endl;
}

// ─── Time-in-Force: GTD ─────────────────────────────────────────────────────

void testGTDExpiry() {
    std::cout << "Running testGTDExpiry..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);

    uint64_t now = 1000000000;
    uint64_t expiry = now + 100;

    // GTD order that expires at now+100
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit, 0, 0,
                  TimeInForce::GTD, expiry);
    assert(book.getOrder(1) != nullptr);

    // Not expired yet
    book.expireOrders(now + 50);
    assert(book.getOrder(1) != nullptr);

    // Expired
    book.expireOrders(now + 100);
    assert(book.getOrder(1) == nullptr);

    std::cout << "testGTDExpiry PASSED" << std::endl;
}

void testDAYExpiry() {
    std::cout << "Running testDAYExpiry..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);

    uint64_t sessionEnd = 2000000000;

    // DAY order
    book.addOrder(1, 1, Side::Sell, 1010000, 100, OrderType::Limit, 0, 0,
                  TimeInForce::DAY, sessionEnd);
    // GTC order (should NOT expire)
    book.addOrder(2, 2, Side::Sell, 1020000, 100, OrderType::Limit);

    assert(book.getOrder(1) != nullptr);
    assert(book.getOrder(2) != nullptr);

    // End of day
    book.expireOrders(sessionEnd);
    assert(book.getOrder(1) == nullptr); // DAY expired
    assert(book.getOrder(2) != nullptr); // GTC survives

    std::cout << "testDAYExpiry PASSED" << std::endl;
}

// ─── Pre-trade Risk Checks ──────────────────────────────────────────────────

void testRiskLimits() {
    std::cout << "Running testRiskLimits..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);

    RiskLimits limits;
    limits.maxOrderSize = 500;
    limits.maxOrderNotional = 10000000; // 1000.00 in fixed point
    book.setRiskLimits(1, limits);

    // Within limits
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    assert(book.getOrder(1) != nullptr);

    // Exceeds max order size
    book.addOrder(2, 1, Side::Buy, 1000000, 600, OrderType::Limit);
    assert(book.getOrder(2) == nullptr); // rejected

    // No limits for participant 2
    book.addOrder(3, 2, Side::Buy, 1000000, 600, OrderType::Limit);
    assert(book.getOrder(3) != nullptr);

    std::cout << "testRiskLimits PASSED" << std::endl;
}

// ─── Market Data Snapshot ───────────────────────────────────────────────────

void testMarketDataSnapshot() {
    std::cout << "Running testMarketDataSnapshot..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);

    // Build order book
    book.addOrder(1, 1, Side::Buy, 990000, 100, OrderType::Limit);
    book.addOrder(2, 2, Side::Buy, 990000, 50, OrderType::Limit);
    book.addOrder(3, 3, Side::Buy, 980000, 200, OrderType::Limit);

    book.addOrder(4, 4, Side::Sell, 1010000, 100, OrderType::Limit);
    book.addOrder(5, 5, Side::Sell, 1020000, 150, OrderType::Limit);

    auto snap = book.getSnapshot(5);

    assert(snap.bidCount == 2);
    assert(snap.askCount == 2);

    // Best bid level
    assert(snap.bids[0].price == 990000);
    assert(snap.bids[0].totalQuantity == 150); // 100 + 50
    assert(snap.bids[0].orderCount == 2);

    // Second bid level
    assert(snap.bids[1].price == 980000);
    assert(snap.bids[1].totalQuantity == 200);

    // Best ask level
    assert(snap.asks[0].price == 1010000);
    assert(snap.asks[0].totalQuantity == 100);

    std::cout << "testMarketDataSnapshot PASSED" << std::endl;
}

void testMarketDataHidesHiddenOrders() {
    std::cout << "Running testMarketDataHidesHiddenOrders..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);

    // Visible order
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    // Hidden order at same price
    book.addOrder(2, 2, Side::Buy, 1000000, 200, OrderType::Hidden);

    auto snap = book.getSnapshot();
    assert(snap.bidCount == 1);
    assert(snap.bids[0].totalQuantity == 100); // only visible qty
    assert(snap.bids[0].orderCount == 1);

    std::cout << "testMarketDataHidesHiddenOrders PASSED" << std::endl;
}

// ─── Order Status Notifications ─────────────────────────────────────────────

void testOrderStatusCallbacks() {
    std::cout << "Running testOrderStatusCallbacks..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);

    auto& updates = listener.updates;

    // Add and match
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);
    book.addOrder(2, 2, Side::Buy, 1000000, 100, OrderType::Limit);

    // Should have: Accepted(1), Accepted(2), Filled(1), Filled(2)
    bool found_accept_1 = false, found_accept_2 = false;
    bool found_fill_1 = false, found_fill_2 = false;

    for (const auto& u : updates) {
        if (u.orderId == 1 && u.status == OrderStatus::Accepted) found_accept_1 = true;
        if (u.orderId == 2 && u.status == OrderStatus::Accepted) found_accept_2 = true;
        if (u.orderId == 1 && u.status == OrderStatus::Filled) found_fill_1 = true;
        if (u.orderId == 2 && u.status == OrderStatus::Filled) found_fill_2 = true;
    }

    assert(found_accept_1);
    assert(found_accept_2);
    assert(found_fill_1);
    assert(found_fill_2);

    std::cout << "testOrderStatusCallbacks PASSED" << std::endl;
}

void testOrderStatusRejection() {
    std::cout << "Running testOrderStatusRejection..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);

    auto& updates = listener.updates;

    // FOK with insufficient liquidity -> rejected
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::FOK);

    bool foundReject = false;
    for (const auto& u : updates) {
        if (u.orderId == 1 && u.status == OrderStatus::Rejected
            && u.rejectReason == RejectReason::FOKInsufficientLiquidity) {
            foundReject = true;
        }
    }
    assert(foundReject);

    std::cout << "testOrderStatusRejection PASSED" << std::endl;
}

// ─── Trailing Stop ──────────────────────────────────────────────────────────

void testTrailingStopSell() {
    std::cout << "Running testTrailingStopSell..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);

    // Seed with a trade so lastTradePrice is set
    book.addOrder(10, 10, Side::Sell, 1000000, 10, OrderType::Limit);
    book.addOrder(11, 11, Side::Buy, 1000000, 10, OrderType::Limit);
    // lastTradePrice = 100.00

    // Trailing Stop Sell: trail by 20000 (2.00)
    // trailRefPrice = 100.00, stopPrice = 98.00
    book.addOrder(1, 1, Side::Sell, 1000000, 50, OrderType::TrailingStop, 0, 0,
                  TimeInForce::GTC, 0, 0, PegType::None, 0, 20000);
    const Order* ts = book.getOrder(1);
    assert(ts != nullptr);
    assert(ts->trailRefPrice == 1000000);
    assert(ts->stopPrice == 980000);

    // Price goes up to 102 -> trailRef should update to 102, stop = 100
    book.addOrder(20, 20, Side::Sell, 1020000, 10, OrderType::Limit);
    book.addOrder(21, 21, Side::Buy, 1020000, 10, OrderType::Limit);

    ts = book.getOrder(1);
    assert(ts != nullptr);
    assert(ts->trailRefPrice == 1020000);
    assert(ts->stopPrice == 1000000);

    // Price drops to 100 -> triggers stop at 100
    // Need a resting bid for the triggered sell to match
    book.addOrder(30, 30, Side::Buy, 1000000, 100, OrderType::Limit);
    book.addOrder(31, 31, Side::Sell, 1000000, 10, OrderType::Limit);
    book.addOrder(32, 32, Side::Buy, 1000000, 10, OrderType::Limit);

    // Check if trailing stop was triggered
    // The order should have been converted to limit and matched or resting
    ts = book.getOrder(1);
    if (ts != nullptr) {
        assert(ts->type == OrderType::Limit);
    }

    std::cout << "testTrailingStopSell PASSED" << std::endl;
}

void testTrailingStopBuy() {
    std::cout << "Running testTrailingStopBuy..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);

    // Seed trade at 100
    book.addOrder(10, 10, Side::Sell, 1000000, 10, OrderType::Limit);
    book.addOrder(11, 11, Side::Buy, 1000000, 10, OrderType::Limit);

    // Trailing Stop Buy: trail by 10000 (1.00)
    // trailRefPrice = 100.00, stopPrice = 101.00
    book.addOrder(1, 1, Side::Buy, 1000000, 50, OrderType::TrailingStop, 0, 0,
                  TimeInForce::GTC, 0, 0, PegType::None, 0, 10000);

    const Order* ts = book.getOrder(1);
    assert(ts != nullptr);
    assert(ts->trailRefPrice == 1000000);
    assert(ts->stopPrice == 1010000);

    // Price drops to 99 -> trailRef = 99, stop = 100
    book.addOrder(20, 20, Side::Sell, 990000, 10, OrderType::Limit);
    book.addOrder(21, 21, Side::Buy, 990000, 10, OrderType::Limit);

    ts = book.getOrder(1);
    assert(ts != nullptr);
    assert(ts->trailRefPrice == 990000);
    assert(ts->stopPrice == 1000000);

    std::cout << "testTrailingStopBuy PASSED" << std::endl;
}

// ─── Pegged Orders ──────────────────────────────────────────────────────────

void testPeggedMidPrice() {
    std::cout << "Running testPeggedMidPrice..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);

    // Build book: bid 99, ask 101
    book.addOrder(1, 1, Side::Buy, 990000, 100, OrderType::Limit);
    book.addOrder(2, 2, Side::Sell, 1010000, 100, OrderType::Limit);

    // Mid-peg buy with 0 offset -> should peg at (99+101)/2 = 100
    book.addOrder(3, 3, Side::Buy, 1000000, 50, OrderType::Pegged, 0, 0,
                  TimeInForce::GTC, 0, 0, PegType::MidPeg, 0);

    const Order* peg = book.getOrder(3);
    assert(peg != nullptr);
    assert(peg->price == 1000000); // mid-price

    std::cout << "testPeggedMidPrice PASSED" << std::endl;
}

void testPeggedPrimaryPeg() {
    std::cout << "Running testPeggedPrimaryPeg..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);

    // Build book: bid 99, ask 101
    book.addOrder(1, 1, Side::Buy, 990000, 100, OrderType::Limit);
    book.addOrder(2, 2, Side::Sell, 1010000, 100, OrderType::Limit);

    // Primary-peg buy with +1000 offset -> pegs at best_bid + 0.10 = 99.10
    book.addOrder(3, 3, Side::Buy, 990000, 50, OrderType::Pegged, 0, 0,
                  TimeInForce::GTC, 0, 0, PegType::PrimaryPeg, 1000);

    const Order* peg = book.getOrder(3);
    assert(peg != nullptr);
    assert(peg->price == 991000); // 99.00 + 0.10

    std::cout << "testPeggedPrimaryPeg PASSED" << std::endl;
}

// ─── Minimum Execution Quantity ─────────────────────────────────────────────

void testMinQty() {
    std::cout << "Running testMinQty..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);

    // Resting sell 30 @ 100
    book.addOrder(1, 1, Side::Sell, 1000000, 30, OrderType::Limit);

    // Buy 100 with minQty 50 -> only 30 available, won't match, rests in book
    book.addOrder(2, 2, Side::Buy, 1000000, 100, OrderType::Limit, 0, 0,
                  TimeInForce::GTC, 0, 0, PegType::None, 0, 0, 50);
    assert(book.getOrder(1) != nullptr); // sell still resting
    assert(book.getOrder(2) != nullptr); // buy resting (minQty not met)

    // IOC with minQty that can't be met -> cancel
    book.addOrder(3, 3, Side::Buy, 1000000, 100, OrderType::IOC, 0, 0,
                  TimeInForce::GTC, 0, 0, PegType::None, 0, 0, 50);
    assert(book.getOrder(3) == nullptr); // cancelled

    std::cout << "testMinQty PASSED" << std::endl;
}

// ─── Pro-Rata Matching ──────────────────────────────────────────────────────

void testProRataMatching() {
    std::cout << "Running testProRataMatching..." << std::endl;
    OrderBook book(0, MatchAlgorithm::ProRata);
    TestListener listener;
    book.setEventListener(&listener);

    // Three resting sells at 100: 100, 200, 300 qty
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);
    book.addOrder(2, 2, Side::Sell, 1000000, 200, OrderType::Limit);
    book.addOrder(3, 3, Side::Sell, 1000000, 300, OrderType::Limit);
    // Total: 600

    // Buy 300 @ 100 -> pro-rata should allocate proportionally
    // Order 1: 100/600 * 300 = 50
    // Order 2: 200/600 * 300 = 100
    // Order 3: 300/600 * 300 = 150
    book.addOrder(4, 4, Side::Buy, 1000000, 300, OrderType::Limit);

    auto history = listener.trades;
    assert(history.size() == 3);

    // Check proportional allocation
    uint64_t fill1 = 0, fill2 = 0, fill3 = 0;
    for (const auto& t : history) {
        if (t.sellOrderId == 1) fill1 += t.quantity;
        else if (t.sellOrderId == 2) fill2 += t.quantity;
        else if (t.sellOrderId == 3) fill3 += t.quantity;
    }

    assert(fill1 == 50);
    assert(fill2 == 100);
    assert(fill3 == 150);
    assert(fill1 + fill2 + fill3 == 300);

    std::cout << "testProRataMatching PASSED" << std::endl;
}

// ─── Multi-Symbol ───────────────────────────────────────────────────────────

void testMultiSymbol() {
    std::cout << "Running testMultiSymbol..." << std::endl;
    MatchingEngine engine;
    engine.addSymbol(1);
    engine.addSymbol(2);
    engine.start();

    // Symbol 1: AAPL
    engine.processOrder(1, 1, 10, Side::Sell, 1500000, 100, OrderType::Limit);
    engine.processOrder(1, 2, 20, Side::Buy, 1500000, 50, OrderType::Limit);

    // Symbol 2: GOOG
    engine.processOrder(2, 3, 10, Side::Sell, 2800000, 200, OrderType::Limit);
    engine.processOrder(2, 4, 20, Side::Buy, 2800000, 100, OrderType::Limit);

    // Check books are independent
    auto* book1 = engine.getOrderBook(1);
    auto* book2 = engine.getOrderBook(2);

    assert(book1 != nullptr);
    assert(book2 != nullptr);

    const Order* o1 = book1->getOrder(1);
    assert(o1 != nullptr && o1->remainingQty == 50);

    const Order* o3 = book2->getOrder(3);
    assert(o3 != nullptr && o3->remainingQty == 100);

    engine.stop();
    std::cout << "testMultiSymbol PASSED" << std::endl;
}

void testMultiSymbolKillSwitch() {
    std::cout << "Running testMultiSymbolKillSwitch..." << std::endl;
    MatchingEngine engine;
    engine.addSymbol(1);
    engine.addSymbol(2);
    engine.start();

    // Participant 10 has orders in both symbols
    engine.processOrder(1, 1, 10, Side::Buy, 1000000, 100, OrderType::Limit);
    engine.processOrder(2, 2, 10, Side::Sell, 2000000, 100, OrderType::Limit);
    engine.processOrder(1, 3, 20, Side::Buy, 990000, 100, OrderType::Limit);

    // Kill switch for participant 10
    uint64_t cancelled = engine.killSwitch(10);
    assert(cancelled == 2);

    // Participant 20's order should survive
    assert(engine.getOrderBook(1)->getOrder(3) != nullptr);

    engine.stop();
    std::cout << "testMultiSymbolKillSwitch PASSED" << std::endl;
}

// ─── Journal / Persistence ──────────────────────────────────────────────────

void testJournal() {
    std::cout << "Running testJournal..." << std::endl;

    const std::string journalPath = "/tmp/ob_test_journal.bin";

    // Write
    {
        Journal journal(journalPath);
        journal.logAddOrder(1, 10, 0, Side::Buy, 1000000, 100, OrderType::Limit);
        journal.logAddOrder(2, 20, 0, Side::Sell, 1010000, 50, OrderType::Limit);
        journal.logCancelOrder(2);
        journal.logModifyOrder(1, 50);
        journal.logCancelReplace(1, 1020000, 80);
        journal.flush();
    }

    // Read back
    {
        Journal journal(journalPath);
        auto entries = journal.readAll();
        assert(entries.size() == 5);
        assert(entries[0].entryType == JournalEntry::Type::AddOrder);
        assert(entries[0].orderId == 1);
        assert(entries[0].price == 1000000);
        assert(entries[1].entryType == JournalEntry::Type::AddOrder);
        assert(entries[2].entryType == JournalEntry::Type::CancelOrder);
        assert(entries[2].orderId == 2);
        assert(entries[3].entryType == JournalEntry::Type::ModifyOrder);
        assert(entries[3].newQty == 50);
        assert(entries[4].entryType == JournalEntry::Type::CancelReplace);
        assert(entries[4].newPrice == 1020000);
    }

    // Cleanup
    std::remove(journalPath.c_str());
    std::cout << "testJournal PASSED" << std::endl;
}

void testJournalReplay() {
    std::cout << "Running testJournalReplay..." << std::endl;

    const std::string journalPath = "/tmp/ob_test_journal_replay.bin";

    // Record operations
    {
        Journal journal(journalPath);
        journal.logAddOrder(1, 10, 0, Side::Sell, 1000000, 100, OrderType::Limit);
        journal.logAddOrder(2, 20, 0, Side::Buy, 1000000, 50, OrderType::Limit);
        journal.logAddOrder(3, 30, 0, Side::Buy, 990000, 200, OrderType::Limit);
        journal.logCancelOrder(3);
        journal.flush();
    }

    // Replay into a fresh OrderBook
    {
        Journal journal(journalPath);
        auto entries = journal.readAll();

        OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);
        for (const auto& e : entries) {
            switch (e.entryType) {
                case JournalEntry::Type::AddOrder:
                    book.addOrder(e.orderId, e.participantId, e.side, e.price, e.quantity,
                                  e.orderType, e.stopPrice, e.displayQty, e.timeInForce,
                                  e.expiryTime, e.stopLimitPrice, e.pegType, e.pegOffset,
                                  e.trailAmount, e.minQty, e.hidden);
                    break;
                case JournalEntry::Type::CancelOrder:
                    book.cancelOrder(e.orderId);
                    break;
                case JournalEntry::Type::ModifyOrder:
                    book.modifyOrder(e.orderId, e.newQty);
                    break;
                case JournalEntry::Type::CancelReplace:
                    book.cancelReplace(e.orderId, e.newPrice, e.newQty);
                    break;
                case JournalEntry::Type::Snapshot:
                    book.addOrder(e.orderId, e.participantId, e.side, e.price, e.quantity,
                                  e.orderType, e.stopPrice, e.displayQty, e.timeInForce,
                                  e.expiryTime, e.stopLimitPrice, e.pegType, e.pegOffset,
                                  e.trailAmount, e.minQty, e.hidden);
                    break;
            }
        }

        // Verify replayed state
        assert(book.getOrder(1) != nullptr);
        assert(book.getOrder(1)->remainingQty == 50); // 100 - 50 matched
        assert(book.getOrder(2) == nullptr); // fully matched
        assert(book.getOrder(3) == nullptr); // cancelled
        assert(listener.trades.size() == 1);
    }

    std::remove(journalPath.c_str());
    std::cout << "testJournalReplay PASSED" << std::endl;
}

// ─── Market Data Callbacks ──────────────────────────────────────────────────

void testMarketDataCallbacks() {
    std::cout << "Running testMarketDataCallbacks..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);

    std::vector<MarketDataUpdate> mdUpdates;
    struct MDListener : public EventListener {
        std::vector<MarketDataUpdate>* md_ptr;
        void onTrade(const Trade&) override {}
        void onOrderUpdate(const OrderUpdate&) override {}
        void onMarketData(const MarketDataUpdate& u) override { md_ptr->push_back(u); }
    } md_listener;
    md_listener.md_ptr = &mdUpdates;
    book.setEventListener(&md_listener);

    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    // Should get an Add update
    bool foundAdd = false;
    for (const auto& u : mdUpdates) {
        if (u.action == MarketDataUpdate::Action::Add && u.side == Side::Buy)
            foundAdd = true;
    }
    assert(foundAdd);

    mdUpdates.clear();
    book.cancelOrder(1);
    bool foundDelete = false;
    for (const auto& u : mdUpdates) {
        if (u.action == MarketDataUpdate::Action::Delete && u.side == Side::Buy)
            foundDelete = true;
    }
    assert(foundDelete);

    std::cout << "testMarketDataCallbacks PASSED" << std::endl;
}

// ─── Input Validation ───────────────────────────────────────────────────────

void testInputValidation() {
    std::cout << "Running testInputValidation..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);

    // Zero quantity -> reject
    book.addOrder(1, 1, Side::Buy, 1000000, 0, OrderType::Limit);
    assert(book.getOrder(1) == nullptr);

    // Negative price for limit -> reject
    book.addOrder(2, 1, Side::Buy, -100, 100, OrderType::Limit);
    assert(book.getOrder(2) == nullptr);

    // Market order with price 0 -> OK (market orders don't need price)
    book.addOrder(100, 100, Side::Sell, 1000000, 50, OrderType::Limit);
    book.addOrder(3, 1, Side::Buy, 0, 50, OrderType::Market);
    assert(book.getOrder(100) == nullptr); // matched

    std::cout << "testInputValidation PASSED" << std::endl;
}

// ─── Comprehensive Fuzz with new features ───────────────────────────────────

void testFuzzWithNewFeatures(int numOps) {
    std::cout << "Running testFuzzWithNewFeatures (" << numOps << " ops)..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);
    std::mt19937 rng(12345);
    std::uniform_int_distribution<Price> priceDist(990, 1010);
    std::uniform_int_distribution<int> opDist(1, 100);
    std::uniform_int_distribution<int> typeDist(0, 5);
    std::deque<OrderId> activeOrders;
    OrderId nextId = 1;

    for (int i = 0; i < numOps; ++i) {
        int op = opDist(rng);

        if (op <= 40) {
            // Add various order types
            OrderId id = nextId++;
            activeOrders.push_back(id);
            int t = typeDist(rng);
            Price p = priceDist(rng) * 1000;
            Side s = (i % 2 == 0) ? Side::Buy : Side::Sell;
            ParticipantId pid = (i % 10) + 1;

            switch(t) {
                case 0: book.addOrder(id, pid, s, p, 100, OrderType::Limit); break;
                case 1: book.addOrder(id, pid, s, 0, 50, OrderType::Market); break;
                case 2: book.addOrder(id, pid, s, p, 100, OrderType::IOC); break;
                case 3: book.addOrder(id, pid, s, p, 100, OrderType::PostOnly); break;
                case 4: book.addOrder(id, pid, s, p, 100, OrderType::Hidden); break;
                case 5: book.addOrder(id, pid, s, p, 100, OrderType::Iceberg, 0, 30); break;
            }
        } else if (op <= 70 && !activeOrders.empty()) {
            // Cancel
            size_t idx = rng() % activeOrders.size();
            book.cancelOrder(activeOrders[idx]);
            activeOrders.erase(activeOrders.begin() + idx);
        } else if (op <= 85 && !activeOrders.empty()) {
            // Cancel/Replace
            size_t idx = rng() % activeOrders.size();
            Price newP = priceDist(rng) * 1000;
            book.cancelReplace(activeOrders[idx], newP, 50);
        } else if (op <= 95 && !activeOrders.empty()) {
            // Modify
            size_t idx = rng() % activeOrders.size();
            book.modifyOrder(activeOrders[idx], 10);
        } else {
            // Market order
            OrderId id = nextId++;
            book.addOrder(id, 99, (i % 2 == 0 ? Side::Buy : Side::Sell), 0, 50, OrderType::Market);
        }

        // Invariant: best bid < best ask
        Price bb = book.getBestBid();
        Price ba = book.getBestAsk();
        if (bb != 0 && ba != std::numeric_limits<Price>::max()) {
            assert(bb < ba);
        }
    }
    std::cout << "testFuzzWithNewFeatures PASSED" << std::endl;
}

// ─── Uncross Auction (actual trade execution) ───────────────────────────────

void testUncrossExecution() {
    std::cout << "Running testUncrossExecution..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);

    // Build crossed book:
    // Bids: 100@102, 50@101, 200@100
    // Asks: 80@99, 120@100, 100@101
    book.addOrder(1, 1, Side::Buy, 1020000, 100, OrderType::Limit);
    book.addOrder(2, 2, Side::Buy, 1010000, 50, OrderType::Limit);
    book.addOrder(3, 3, Side::Buy, 1000000, 200, OrderType::Limit);
    book.addOrder(4, 4, Side::Sell, 990000, 80, OrderType::Limit);
    book.addOrder(5, 5, Side::Sell, 1000000, 120, OrderType::Limit);
    book.addOrder(6, 6, Side::Sell, 1010000, 100, OrderType::Limit);

    size_t tradesBefore = listener.trades.size();
    book.uncross();
    size_t tradesAfter = listener.trades.size();

    // Uncross should have generated actual trades
    assert(tradesAfter > tradesBefore);

    // All trades should be at the same uncross price
    Price uncrossPrice = listener.trades[tradesBefore].price;
    for (size_t i = tradesBefore; i < tradesAfter; ++i) {
        assert(listener.trades[i].price == uncrossPrice);
    }

    // Verify total matched volume
    Quantity totalTraded = 0;
    for (size_t i = tradesBefore; i < tradesAfter; ++i) {
        totalTraded += listener.trades[i].quantity;
    }
    assert(totalTraded > 0);

    std::cout << "testUncrossExecution PASSED (price=" << toDouble(uncrossPrice)
              << ", volume=" << totalTraded << ", trades=" << (tradesAfter - tradesBefore) << ")" << std::endl;
}

// ─── Depth Limit ────────────────────────────────────────────────────────────

void testDepthLimit() {
    std::cout << "Running testDepthLimit..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);
    book.setMaxDepth(3); // max 3 price levels per side

    // Add 3 bid levels — all should succeed
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    book.addOrder(2, 2, Side::Buy, 990000, 100, OrderType::Limit);
    book.addOrder(3, 3, Side::Buy, 980000, 100, OrderType::Limit);
    assert(book.getBidLevelsCount() == 3);

    // 4th bid level at new price — should be rejected (cancelled after no match)
    book.addOrder(4, 4, Side::Buy, 970000, 100, OrderType::Limit);
    assert(book.getOrder(4) == nullptr);
    assert(book.getBidLevelsCount() == 3);

    // Adding to an existing level should still work
    book.addOrder(5, 5, Side::Buy, 1000000, 50, OrderType::Limit);
    assert(book.getOrder(5) != nullptr);
    assert(book.getBidLevelsCount() == 3);

    // Same for asks
    book.addOrder(10, 10, Side::Sell, 1010000, 100, OrderType::Limit);
    book.addOrder(11, 11, Side::Sell, 1020000, 100, OrderType::Limit);
    book.addOrder(12, 12, Side::Sell, 1030000, 100, OrderType::Limit);
    assert(book.getAskLevelsCount() == 3);

    book.addOrder(13, 13, Side::Sell, 1040000, 100, OrderType::Limit);
    assert(book.getOrder(13) == nullptr);
    assert(book.getAskLevelsCount() == 3);

    std::cout << "testDepthLimit PASSED" << std::endl;
}

// ─── Journal CRC Validation ─────────────────────────────────────────────────

void testJournalCRC() {
    std::cout << "Running testJournalCRC..." << std::endl;
    const std::string path = "/tmp/ob_test_crc.bin";

    // Write valid entries
    {
        Journal journal(path);
        journal.logAddOrder(1, 10, 0, Side::Buy, 1000000, 100, OrderType::Limit);
        journal.logAddOrder(2, 20, 0, Side::Sell, 1010000, 50, OrderType::Limit);
        journal.flush();
    }

    // Read with CRC validation — should get 2 entries
    {
        Journal journal(path);
        auto entries = journal.readAll(true);
        assert(entries.size() == 2);
    }

    // Corrupt the file by flipping a byte
    {
        FILE* f = std::fopen(path.c_str(), "r+b");
        assert(f);
        // Seek to middle of first entry and flip a byte
        std::fseek(f, 20, SEEK_SET);
        uint8_t byte;
        std::fread(&byte, 1, 1, f);
        byte ^= 0xFF;
        std::fseek(f, 20, SEEK_SET);
        std::fwrite(&byte, 1, 1, f);
        std::fclose(f);
    }

    // Read with CRC validation — should detect corruption and stop at entry 0
    {
        Journal journal(path);
        auto entries = journal.readAll(true);
        assert(entries.size() == 0); // first entry corrupted, stops reading
    }

    // Read without CRC validation — should still get 2 entries
    {
        Journal journal(path);
        auto entries = journal.readAll(false);
        assert(entries.size() == 2);
    }

    std::remove(path.c_str());
    std::cout << "testJournalCRC PASSED" << std::endl;
}

// ─── Journal Snapshot ───────────────────────────────────────────────────────

void testJournalSnapshot() {
    std::cout << "Running testJournalSnapshot..." << std::endl;
    const std::string path = "/tmp/ob_test_snapshot.bin";

    // Build some book state
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);
    book.addOrder(1, 10, Side::Sell, 1000000, 100, OrderType::Limit);
    book.addOrder(2, 20, Side::Buy, 1000000, 30, OrderType::Limit);
    // Order 1 now has 70 remaining

    book.addOrder(3, 30, Side::Buy, 990000, 200, OrderType::Limit);

    // Write snapshot
    {
        Journal journal(path);
        const Order* out[10];
        size_t numOrders = book.getAllOrders(out, 10);
        for (size_t i = 0; i < numOrders; ++i) {
            const auto* o = out[i];
            journal.logSnapshot(o->id, o->participantId, o->symbolId,
                                o->side, o->price, o->remainingQty, o->type,
                                o->timeInForce, o->expiryTime, o->stopPrice,
                                o->stopLimitPrice, o->displayQty, o->pegType,
                                o->pegOffset, o->trailAmount, o->minQty, o->isHidden);
        }
        journal.flush();
    }

    // Replay snapshot into fresh book
    {
        Journal journal(path);
        auto entries = journal.readAll();

        OrderBook newBook;
        for (const auto& e : entries) {
            if (e.entryType == JournalEntry::Type::Snapshot) {
                newBook.addOrder(e.orderId, e.participantId, e.side, e.price,
                                e.quantity, e.orderType, e.stopPrice, e.displayQty,
                                e.timeInForce, e.expiryTime, e.stopLimitPrice,
                                e.pegType, e.pegOffset, e.trailAmount, e.minQty, e.hidden);
            }
        }

        // Verify state matches
        const Order* o1 = newBook.getOrder(1);
        assert(o1 != nullptr);
        assert(o1->remainingQty == 70);

        const Order* o3 = newBook.getOrder(3);
        assert(o3 != nullptr);
        assert(o3->remainingQty == 200);
    }

    std::remove(path.c_str());
    std::cout << "testJournalSnapshot PASSED" << std::endl;
}

// ─── Async Engine ───────────────────────────────────────────────────────────

void testAsyncEngine() {
    std::cout << "Running testAsyncEngine..." << std::endl;
    MatchingEngine engine;
    engine.addSymbol(1);
    engine.startAsync(1, 1024); // no CPU pinning, 1K queue

    // Submit orders asynchronously
    engine.processOrder(1, 1, 10, Side::Sell, 1000000, 100, OrderType::Limit);
    engine.processOrder(1, 2, 20, Side::Buy, 1000000, 100, OrderType::Limit);
    engine.processOrder(1, 3, 30, Side::Sell, 1010000, 200, OrderType::Limit);
    engine.processOrder(1, 4, 40, Side::Buy, 1010000, 50, OrderType::Limit);

    // Wait for all to be processed
    engine.waitForDrain();

    // Verify results
    auto* book = engine.getOrderBook(1);
    assert(book != nullptr);

    // Order 1 & 2: matched (both gone)
    assert(book->getOrder(1) == nullptr);
    assert(book->getOrder(2) == nullptr);

    // Order 3: partially filled (200 - 50 = 150 remaining)
    const Order* o3 = book->getOrder(3);
    assert(o3 != nullptr);
    assert(o3->remainingQty == 150);

    // Order 4: fully filled
    assert(book->getOrder(4) == nullptr);

    assert(engine.getProcessedCount() == 4);

    engine.stopAsync();
    std::cout << "testAsyncEngine PASSED" << std::endl;
}

void testAsyncCancelReplace() {
    std::cout << "Running testAsyncCancelReplace..." << std::endl;
    MatchingEngine engine;
    engine.startAsync(1, 1024);

    // Add order via async
    engine.processOrder(0, 1, 10, Side::Buy, 1000000, 100, OrderType::Limit);
    engine.waitForDrain();

    // Cancel via async
    engine.cancelOrder(0, 1);
    engine.waitForDrain();

    auto* book = engine.getOrderBook(0);
    assert(book->getOrder(1) == nullptr);

    engine.stopAsync();
    std::cout << "testAsyncCancelReplace PASSED" << std::endl;
}

// ─── FIX Protocol ───────────────────────────────────────────────────────────

void testFIXParsing() {
    std::cout << "Running testFIXParsing..." << std::endl;

    // Build a FIX message using SOH (\x01) delimiter
    std::string raw;
    raw += "8=FIX.4.2"; raw += '\x01';
    raw += "9=100"; raw += '\x01';
    raw += "35=D"; raw += '\x01';
    raw += "11=12345"; raw += '\x01';
    raw += "49=100"; raw += '\x01';
    raw += "55=1"; raw += '\x01';
    raw += "54=1"; raw += '\x01';
    raw += "44=150"; raw += '\x01';
    raw += "38=200"; raw += '\x01';
    raw += "40=2"; raw += '\x01';
    raw += "59=1"; raw += '\x01';
    raw += "10=000"; raw += '\x01';

    FixMessage msg;
    assert(msg.parse(raw.c_str(), raw.size()));

    assert(msg.getString(FixTag::MsgType) == "D");
    assert(msg.getInt(FixTag::ClOrdID) == 12345);
    assert(msg.getInt(FixTag::SenderCompID) == 100);
    assert(msg.getInt(FixTag::Symbol) == 1);
    assert(msg.getChar(FixTag::Side) == '1'); // Buy
    assert(msg.getInt(FixTag::Price) == 150);
    assert(msg.getInt(FixTag::OrderQty) == 200);
    assert(msg.getChar(FixTag::OrdType) == '2'); // Limit
    assert(msg.getChar(FixTag::TimeInForce) == '1'); // GTC

    std::cout << "testFIXParsing PASSED" << std::endl;
}

void testFIXAdapter() {
    std::cout << "Running testFIXAdapter..." << std::endl;

    // Test NewOrderSingle parsing via fixToOrderParams
    std::string raw;
    raw += "35=D"; raw += '\x01';
    raw += "11=42"; raw += '\x01';
    raw += "49=10"; raw += '\x01';
    raw += "55=1"; raw += '\x01';
    raw += "54=1"; raw += '\x01';
    raw += "44=100000"; raw += '\x01';
    raw += "38=500"; raw += '\x01';
    raw += "40=2"; raw += '\x01';
    raw += "59=1"; raw += '\x01';

    FixMessage msg;
    assert(msg.parse(raw.c_str(), raw.size()));
    auto params = fixToOrderParams(msg);

    assert(params.valid);
    assert(params.orderId == 42);
    assert(params.participantId == 10);
    assert(params.symbolId == 1);
    assert(params.side == Side::Buy);
    assert(params.price == 100000);
    assert(params.qty == 500);
    assert(params.orderType == OrderType::Limit);
    assert(params.tif == TimeInForce::GTC);

    // Test IOC via TimeInForce=3
    std::string rawIOC;
    rawIOC += "35=D"; rawIOC += '\x01';
    rawIOC += "11=43"; rawIOC += '\x01';
    rawIOC += "49=20"; rawIOC += '\x01';
    rawIOC += "55=2"; rawIOC += '\x01';
    rawIOC += "54=2"; rawIOC += '\x01';
    rawIOC += "44=50000"; rawIOC += '\x01';
    rawIOC += "38=100"; rawIOC += '\x01';
    rawIOC += "40=2"; rawIOC += '\x01';
    rawIOC += "59=3"; rawIOC += '\x01';

    FixMessage msgIOC;
    assert(msgIOC.parse(rawIOC.c_str(), rawIOC.size()));
    auto paramsIOC = fixToOrderParams(msgIOC);
    assert(paramsIOC.valid);
    assert(paramsIOC.orderType == OrderType::IOC);
    assert(paramsIOC.side == Side::Sell);

    // Test Cancel parsing (35=F)
    std::string rawCancel;
    rawCancel += "35=F"; rawCancel += '\x01';
    rawCancel += "41=42"; rawCancel += '\x01';
    rawCancel += "55=1"; rawCancel += '\x01';

    FixMessage msgCancel;
    assert(msgCancel.parse(rawCancel.c_str(), rawCancel.size()));
    auto cancelParams = fixToOrderParams(msgCancel);
    assert(cancelParams.valid);
    assert(cancelParams.action == FixOrderParams::Action::Cancel);
    assert(cancelParams.orderId == 42);
    assert(cancelParams.symbolId == 1);

    // Test CancelReplace parsing (35=G)
    std::string rawCR;
    rawCR += "35=G"; rawCR += '\x01';
    rawCR += "41=42"; rawCR += '\x01';
    rawCR += "55=1"; rawCR += '\x01';
    rawCR += "44=105000"; rawCR += '\x01';
    rawCR += "38=300"; rawCR += '\x01';

    FixMessage msgCR;
    assert(msgCR.parse(rawCR.c_str(), rawCR.size()));
    auto crParams = fixToOrderParams(msgCR);
    assert(crParams.valid);
    assert(crParams.action == FixOrderParams::Action::CancelReplace);
    assert(crParams.orderId == 42);
    assert(crParams.newPrice == 105000);
    assert(crParams.newQty == 300);

    // Test ExecutionReport serialization
    std::string report = FixSerializer::buildExecutionReport(
        42, 1, '2', '2', Side::Buy, 100000, 100, 100, 0, 100000, 100);
    assert(report.find("35=8") != std::string::npos);
    assert(report.find("8=FIX.4.2") != std::string::npos);
    assert(report.find("10=") != std::string::npos);

    std::cout << "testFIXAdapter PASSED" << std::endl;
}

void testFIXEndToEnd() {
    std::cout << "Running testFIXEndToEnd..." << std::endl;

    OrderBook book(1);
    TestListener listener;
    book.setEventListener(&listener);

    // Build FIX NewOrderSingle for a sell
    std::string fixSell;
    fixSell += "35=D"; fixSell += '\x01';
    fixSell += "11=1"; fixSell += '\x01';
    fixSell += "49=10"; fixSell += '\x01';
    fixSell += "55=1"; fixSell += '\x01';
    fixSell += "54=2"; fixSell += '\x01';
    fixSell += "44=1000000"; fixSell += '\x01';
    fixSell += "38=100"; fixSell += '\x01';
    fixSell += "40=2"; fixSell += '\x01';
    fixSell += "59=1"; fixSell += '\x01';

    FixMessage sellMsg;
    assert(sellMsg.parse(fixSell.c_str(), fixSell.size()));
    auto sellParams = fixToOrderParams(sellMsg);
    book.addOrder(sellParams.orderId, sellParams.participantId, sellParams.side,
                  sellParams.price, sellParams.qty, sellParams.orderType);

    // Build FIX NewOrderSingle for a buy at same price
    std::string fixBuy;
    fixBuy += "35=D"; fixBuy += '\x01';
    fixBuy += "11=2"; fixBuy += '\x01';
    fixBuy += "49=20"; fixBuy += '\x01';
    fixBuy += "55=1"; fixBuy += '\x01';
    fixBuy += "54=1"; fixBuy += '\x01';
    fixBuy += "44=1000000"; fixBuy += '\x01';
    fixBuy += "38=100"; fixBuy += '\x01';
    fixBuy += "40=2"; fixBuy += '\x01';
    fixBuy += "59=1"; fixBuy += '\x01';

    FixMessage buyMsg;
    assert(buyMsg.parse(fixBuy.c_str(), fixBuy.size()));
    auto buyParams = fixToOrderParams(buyMsg);
    book.addOrder(buyParams.orderId, buyParams.participantId, buyParams.side,
                  buyParams.price, buyParams.qty, buyParams.orderType);

    // Should match
    assert(book.getOrder(1) == nullptr);
    assert(book.getOrder(2) == nullptr);
    assert(listener.trades.size() == 1);
    assert(listener.trades[0].price == 1000000);

    std::cout << "testFIXEndToEnd PASSED" << std::endl;
}

// ─── Get All Orders ─────────────────────────────────────────────────────────

void testGetAllOrders() {
    std::cout << "Running testGetAllOrders..." << std::endl;
    OrderBook book;
    TestListener listener;
    book.setEventListener(&listener);

    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    book.addOrder(2, 2, Side::Sell, 1010000, 100, OrderType::Limit);
    book.addOrder(3, 3, Side::Buy, 990000, 50, OrderType::Limit);

    const Order* out[10];
    size_t numOrders = book.getAllOrders(out, 10);
    assert(numOrders == 3);

    // Cancel one
    book.cancelOrder(2);
    numOrders = book.getAllOrders(out, 10);
    assert(numOrders == 2);

    std::cout << "testGetAllOrders PASSED" << std::endl;
}

void testIngressRateLimit() {
    std::cout << "Running testIngressRateLimit..." << std::endl;

    MatchingEngine engine;
    engine.addSymbol(1);
    engine.setRateLimit(1, 1); // 1 msg/sec, burst of 1
    engine.startAsync(1, 64);

    engine.processOrder(1, 1001, 42, Side::Buy, toPrice(99.00), 10, OrderType::Limit);
    engine.processOrder(1, 1002, 42, Side::Buy, toPrice(98.00), 10, OrderType::Limit);
    engine.waitForDrain();

    auto* book = engine.getOrderBook(1);
    assert(book != nullptr);
    assert(book->getOrder(1001) != nullptr);
    assert(book->getOrder(1002) == nullptr);
    assert(engine.getProcessedCount() == 1);
    assert(engine.getRateLimitedCount() == 1);
    assert(engine.getDroppedCount() == 0);
    assert(engine.getBackpressureRejectCount() == 0);

    engine.stopAsync();
    std::cout << "testIngressRateLimit PASSED" << std::endl;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════════════

int main() {
    try {
        // Original tests
        testBasicMatching();
        testSMP();
        testSMPPurity();
        testMarketOrder();
        testIOC();
        testFOK();
        testStopOrder();
        testIcebergOrder();
        testAnalytics();
        testCircuitBreaker();
        testCircuitBreakerBoundaries();
        testStress();
        testPartialFillPriority();
        testFuzz(100000);

        // New feature tests
        testStopLimitOrder();
        testPostOnly();
        testHiddenOrders();
        testCancelReplace();
        testCancelReplaceCrossing();
        testKillSwitch();
        testGTDExpiry();
        testDAYExpiry();
        testRiskLimits();
        testMarketDataSnapshot();
        testMarketDataHidesHiddenOrders();
        testOrderStatusCallbacks();
        testOrderStatusRejection();
        testTrailingStopSell();
        testTrailingStopBuy();
        testPeggedMidPrice();
        testPeggedPrimaryPeg();
        testMinQty();
        testProRataMatching();
        testMultiSymbol();
        testMultiSymbolKillSwitch();
        testJournal();
        testJournalReplay();
        testMarketDataCallbacks();
        testInputValidation();
        testIngressRateLimit();
        testFuzzWithNewFeatures(100000);

        std::cout << "\nALL TESTS PASSED!" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
