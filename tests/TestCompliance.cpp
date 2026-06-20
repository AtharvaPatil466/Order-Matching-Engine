// TestCompliance — risk, fees, and audit trail wired through the engine.
//
// What this proves (end-to-end via the async MatchingEngine):
//   1. Multi-tier risk: a firm-level aggregate position cap rejects a second
//      trader's order that would breach the firm total, even though that
//      trader's own limit is fine. The firm net position reflects only the
//      fills that were allowed.
//   2. Fat-finger: an order priced wildly away from the reference is rejected
//      pre-trade (before it can match).
//   3. Maker-taker fees accrue on a fill: the resting maker earns the rebate,
//      the aggressing taker pays the fee.
//   4. The CAT audit trail captures the order lifecycle (new / trade / cancel).

#include "MatchingEngine.h"
#include "HierarchicalRiskManager.h"
#include "FeeEngine.h"
#include "Types.h"

#include <cassert>
#include <iostream>
#include <string>

using namespace OrderMatcher;

static int tests_passed = 0;
#define SECTION(name) std::cout << "\n  " << name << "..." << std::flush
#define PASS()        do { ++tests_passed; std::cout << " PASS" << std::endl; } while (0)

// 1. Firm-level aggregate position cap across two traders.
void test_firm_aggregate_risk_cap() {
    SECTION("firm aggregate position cap rejects a breaching second trader");
    MatchingEngine engine;
    engine.addSymbol(1);
    engine.getOrderBook(1)->setCircuitBreakerThreshold(1e9);   // isolate risk logic
    engine.startAsync(1, 1024);

    // T1 (id 1) and T2 (id 2) both belong to firm 1000; firm cap = 100.
    engine.mapParticipant(1, /*strat=*/10, /*acct=*/100, /*firm=*/1000);
    engine.mapParticipant(2, /*strat=*/11, /*acct=*/101, /*firm=*/1000);
    engine.setRiskTierLimits(RiskTier::Firm, 1000, TierLimits{0, 0, /*maxNetPosition=*/100, 0.0});

    // Unmapped seller (participant 9) provides liquidity.
    engine.processOrder(1, 100, 9, Side::Sell, 100, 60, OrderType::Limit);
    engine.processOrder(1, 1,   1, Side::Buy,  100, 60, OrderType::Limit);  // T1 buys 60
    engine.waitForDrain();
    assert(engine.riskNetPosition(RiskTier::Firm, 1000) == 60);

    // T2 wants 60 more — firm would hit 120 > 100, so it must be rejected.
    engine.processOrder(1, 101, 9, Side::Sell, 100, 60, OrderType::Limit);  // fresh liquidity
    engine.processOrder(1, 2,   2, Side::Buy,  100, 60, OrderType::Limit);  // T2 buy -> rejected
    engine.waitForDrain();
    const OrderBook* book = engine.getOrderBook(1);
    assert(book->getOrder(2) == nullptr);                 // T2 order rejected pre-trade
    assert(book->getOrder(101) != nullptr);               // seller untouched (no match happened)
    assert(engine.riskNetPosition(RiskTier::Firm, 1000) == 60);

    // A smaller T2 order (60 + 30 = 90 <= 100) is allowed and fills.
    engine.processOrder(1, 3, 2, Side::Buy, 100, 30, OrderType::Limit);
    engine.waitForDrain();
    assert(engine.riskNetPosition(RiskTier::Firm, 1000) == 90);

    engine.stopAsync();
    PASS();
}

// 2. Fat-finger: a wildly mispriced order is rejected pre-trade.
void test_fat_finger_reject() {
    SECTION("fat-finger check rejects an order far from the reference price");
    MatchingEngine engine;
    engine.addSymbol(1);
    engine.getOrderBook(1)->setCircuitBreakerThreshold(1e9);
    engine.startAsync(1, 1024);

    // Trader 1 limited to 10% from the reference (NBBO mid proxy).
    engine.mapParticipant(1, 10, 100, 1000);
    engine.setRiskTierLimits(RiskTier::Trader, 1, TierLimits{0, 0, 0, /*maxPctFromRef=*/0.10});

    // Establish a two-sided book (unmapped p9) so a mid price exists (~101).
    engine.processOrder(1, 100, 9, Side::Buy,  100, 10, OrderType::Limit);
    engine.processOrder(1, 101, 9, Side::Sell, 102, 10, OrderType::Limit);
    engine.waitForDrain();

    // Trader 1 buys at 200 — ~98% above the mid — must be rejected pre-trade,
    // so it never matches the resting sell at 102.
    engine.processOrder(1, 1, 1, Side::Buy, 200, 10, OrderType::Limit);
    engine.waitForDrain();
    const OrderBook* book = engine.getOrderBook(1);
    assert(book->getOrder(1) == nullptr);            // rejected
    assert(book->getOrder(101) != nullptr);          // resting sell untouched

    engine.stopAsync();
    PASS();
}

// 3. Maker-taker fee accrual on a fill.
void test_maker_taker_fees() {
    SECTION("maker earns the rebate and taker pays the fee on a fill");
    MatchingEngine engine;
    engine.addSymbol(1);
    engine.getOrderBook(1)->setCircuitBreakerThreshold(1e9);
    engine.startAsync(1, 1024);

    // -2 bps maker rebate, +5 bps taker fee. notional 100*100 = 10000 ->
    // maker fee llround(10000*-2/10000) = -2; taker llround(10000*5/10000) = 5.
    engine.setDefaultFeeSchedule(FeeSchedule{/*makerBps=*/-2.0, /*takerBps=*/5.0});

    engine.processOrder(1, 1, 1, Side::Sell, 100, 100, OrderType::Limit);  // resting maker (p1)
    engine.processOrder(1, 2, 2, Side::Buy,  100, 100, OrderType::Limit);  // aggressing taker (p2)
    engine.waitForDrain();

    assert(engine.accruedFee(1) == -2);   // maker rebate
    assert(engine.accruedFee(2) == 5);    // taker fee

    engine.stopAsync();
    PASS();
}

// 4. Audit trail captures the order lifecycle.
void test_audit_trail_capture() {
    SECTION("CAT audit trail records new / trade / cancel events");
    MatchingEngine engine;
    engine.addSymbol(1);
    engine.getOrderBook(1)->setCircuitBreakerThreshold(1e9);
    engine.startAsync(1, 1024);
    engine.enableAuditTrail(true);

    engine.processOrder(1, 1, 1, Side::Sell, 100, 100, OrderType::Limit);  // New
    engine.processOrder(1, 2, 2, Side::Buy,  100, 50,  OrderType::Limit);  // New + Trade
    engine.waitForDrain();
    engine.cancelOrder(1, 1);                                              // Cancel (remaining sell)
    engine.waitForDrain();

    // At least: New(1), New(2), Trade, Cancel(1).
    assert(engine.auditRecordCount() >= 4);
    const std::string jsonl = engine.auditTrailJsonl();
    assert(!jsonl.empty());
    assert(jsonl.find("Trade") != std::string::npos);
    assert(jsonl.find("Cancel") != std::string::npos);

    engine.stopAsync();
    PASS();
}

int main() {
    std::cout << "Running compliance integration tests..." << std::endl;
    test_firm_aggregate_risk_cap();
    test_fat_finger_reject();
    test_maker_taker_fees();
    test_audit_trail_capture();
    std::cout << "\nAll " << tests_passed << " compliance test sections passed." << std::endl;
    return 0;
}
