// TestHierarchicalRisk — assert-based tests for the multi-tier
// HierarchicalRiskManager pre-trade risk gate.
//
// Covers:
//   (1) trader maxOrderSize / maxOrderNotional reject;
//   (2) fat-finger reject when price is too far from reference, accept within;
//   (3) FIRM-LEVEL aggregate reject — two traders, one firm cap, fill T1 near
//       the cap, then a T2 order that would breach the firm aggregate is
//       rejected with breachedTier==Firm while a small T2 order is allowed;
//   (4) netPosition aggregates correctly across tiers after fills;
//   (5) an unmapped participant is always allowed.

#include "HierarchicalRiskManager.h"

#include <cassert>
#include <iostream>

using namespace OrderMatcher;

static int passed = 0;
#define TEST(name) std::cout << "  " << #name << "..." << std::flush
#define PASS() do { ++passed; std::cout << " PASS\n"; } while(0)

// Entity-id layout shared by several tests. Trader-tier entity id is the
// ParticipantId itself; the other tiers use distinct ids.
namespace {
constexpr ParticipantId kT1 = 1001;
constexpr ParticipantId kT2 = 1002;
constexpr uint64_t kStrat1 = 5001;
constexpr uint64_t kStrat2 = 5002;
constexpr uint64_t kAcct   = 7001;
constexpr uint64_t kFirm   = 9001;
}  // namespace

// (1) Trader-tier maxOrderSize and maxOrderNotional rejections.
void test_trader_order_limits() {
    TEST(TraderOrderLimits);

    HierarchicalRiskManager rm;
    rm.mapParticipant(kT1, kStrat1, kAcct, kFirm);

    TierLimits lim;
    lim.maxOrderSize     = 100;        // qty cap
    lim.maxOrderNotional = 1'000'000;  // price*qty cap
    rm.setLimits(RiskTier::Trader, kT1, lim);

    // Within both limits: 50 @ 100 -> notional 5,000.
    {
        auto d = rm.check(kT1, Side::Buy, /*price*/100, /*qty*/50, /*ref*/0);
        assert(d.allowed);
        assert(d.reason == RejectReason::None);
    }

    // Over the size cap: 101 > 100.
    {
        auto d = rm.check(kT1, Side::Buy, /*price*/10, /*qty*/101, /*ref*/0);
        assert(!d.allowed);
        assert(d.reason == RejectReason::RiskLimitBreached);
        assert(d.breachedTier == RiskTier::Trader);
    }

    // Within size cap but over notional cap: 100 @ 20000 -> 2,000,000 > 1,000,000.
    {
        auto d = rm.check(kT1, Side::Buy, /*price*/20'000, /*qty*/100, /*ref*/0);
        assert(!d.allowed);
        assert(d.reason == RejectReason::RiskLimitBreached);
        assert(d.breachedTier == RiskTier::Trader);
    }

    // Exactly at the notional boundary is allowed (strictly-greater rejects):
    // 100 @ 10000 -> 1,000,000 == cap.
    {
        auto d = rm.check(kT1, Side::Buy, /*price*/10'000, /*qty*/100, /*ref*/0);
        assert(d.allowed);
    }

    PASS();
}

// (2) Fat-finger: reject when price strays too far from the reference.
void test_fat_finger() {
    TEST(FatFinger);

    HierarchicalRiskManager rm;
    rm.mapParticipant(kT1, kStrat1, kAcct, kFirm);

    TierLimits lim;
    lim.maxPctFromRef = 0.05;  // 5% collar
    rm.setLimits(RiskTier::Trader, kT1, lim);

    const Price ref = 10'000;

    // Within 5%: 10,400 is +4%.
    {
        auto d = rm.check(kT1, Side::Buy, /*price*/10'400, /*qty*/1, ref);
        assert(d.allowed);
    }

    // Within 5% on the downside: 9,600 is -4%.
    {
        auto d = rm.check(kT1, Side::Sell, /*price*/9'600, /*qty*/1, ref);
        assert(d.allowed);
    }

    // Beyond 5% (fat finger): 11,000 is +10%.
    {
        auto d = rm.check(kT1, Side::Buy, /*price*/11'000, /*qty*/1, ref);
        assert(!d.allowed);
        assert(d.reason == RejectReason::RiskLimitBreached);
        assert(d.breachedTier == RiskTier::Trader);
    }

    // Far below reference is also a fat finger: 5,000 is -50%.
    {
        auto d = rm.check(kT1, Side::Sell, /*price*/5'000, /*qty*/1, ref);
        assert(!d.allowed);
        assert(d.breachedTier == RiskTier::Trader);
    }

    // With referencePrice == 0 the fat-finger rule is off -> allowed even
    // though the price is "far" from a nonexistent reference.
    {
        auto d = rm.check(kT1, Side::Buy, /*price*/99'999, /*qty*/1, /*ref*/0);
        assert(d.allowed);
    }

    PASS();
}

// (3) THE KEY FEATURE: firm-level aggregation across two traders.
void test_firm_aggregate_reject() {
    TEST(FirmAggregateReject);

    HierarchicalRiskManager rm;
    // Two distinct traders, distinct strategies, but the SAME account+firm.
    rm.mapParticipant(kT1, kStrat1, kAcct, kFirm);
    rm.mapParticipant(kT2, kStrat2, kAcct, kFirm);

    // Generous trader-tier limits so the trader tier never trips here.
    TierLimits traderLim;
    traderLim.maxNetPosition = 1'000'000;
    rm.setLimits(RiskTier::Trader, kT1, traderLim);
    rm.setLimits(RiskTier::Trader, kT2, traderLim);

    // Firm cap: aggregate net position across all firm traders <= 1000.
    TierLimits firmLim;
    firmLim.maxNetPosition = 1000;
    rm.setLimits(RiskTier::Firm, kFirm, firmLim);

    // T1 fills to near the firm cap: +950 net.
    rm.onFill(kT1, Side::Buy, /*price*/100, /*qty*/950);
    assert(rm.netPosition(RiskTier::Firm, kFirm) == 950);

    // A small T2 buy is fine: 950 + 40 = 990 <= 1000.
    {
        auto d = rm.check(kT2, Side::Buy, /*price*/100, /*qty*/40, /*ref*/0);
        assert(d.allowed);
    }

    // A larger T2 buy breaches the FIRM aggregate: 950 + 100 = 1050 > 1000,
    // even though T2's own trader limit (1,000,000) is fine.
    {
        auto d = rm.check(kT2, Side::Buy, /*price*/100, /*qty*/100, /*ref*/0);
        assert(!d.allowed);
        assert(d.reason == RejectReason::RiskLimitBreached);
        assert(d.breachedTier == RiskTier::Firm);
    }

    // T2 selling reduces the firm aggregate, so a sell is allowed: 950 - 100
    // = 850, magnitude 850 <= 1000.
    {
        auto d = rm.check(kT2, Side::Sell, /*price*/100, /*qty*/100, /*ref*/0);
        assert(d.allowed);
    }

    PASS();
}

// (4) netPosition aggregates correctly across every tier after fills.
void test_netposition_aggregation() {
    TEST(NetPositionAggregation);

    HierarchicalRiskManager rm;
    rm.mapParticipant(kT1, kStrat1, kAcct, kFirm);
    rm.mapParticipant(kT2, kStrat2, kAcct, kFirm);

    // T1: +300 then -100 -> trader net +200.
    rm.onFill(kT1, Side::Buy,  /*price*/100, /*qty*/300);
    rm.onFill(kT1, Side::Sell, /*price*/100, /*qty*/100);
    // T2: +500 -> trader net +500.
    rm.onFill(kT2, Side::Buy,  /*price*/100, /*qty*/500);

    // Trader tier: each trader's own running net.
    assert(rm.netPosition(RiskTier::Trader, kT1) == 200);
    assert(rm.netPosition(RiskTier::Trader, kT2) == 500);

    // Strategy tier: one trader each -> mirrors the trader nets.
    assert(rm.netPosition(RiskTier::Strategy, kStrat1) == 200);
    assert(rm.netPosition(RiskTier::Strategy, kStrat2) == 500);

    // Account + firm tiers: aggregate of both traders -> 200 + 500 = 700.
    assert(rm.netPosition(RiskTier::Account, kAcct) == 700);
    assert(rm.netPosition(RiskTier::Firm, kFirm) == 700);

    // An entity that was never filled reports 0.
    assert(rm.netPosition(RiskTier::Firm, /*unknown*/424242) == 0);

    PASS();
}

// (5) An unmapped participant has no hierarchy and is always allowed; onFill
//     for it is a no-op.
void test_unmapped_always_allowed() {
    TEST(UnmappedAlwaysAllowed);

    HierarchicalRiskManager rm;

    // Configure aggressive firm limits, but never map this participant.
    TierLimits firmLim;
    firmLim.maxNetPosition = 1;
    firmLim.maxOrderSize   = 1;
    rm.setLimits(RiskTier::Firm, kFirm, firmLim);

    const ParticipantId unmapped = 777777;

    // A huge order from an unmapped participant is allowed.
    {
        auto d = rm.check(unmapped, Side::Buy, /*price*/10'000,
                          /*qty*/1'000'000, /*ref*/0);
        assert(d.allowed);
        assert(d.reason == RejectReason::None);
    }

    // onFill on the unmapped participant changes no tier state.
    rm.onFill(unmapped, Side::Buy, /*price*/10'000, /*qty*/1'000'000);
    assert(rm.netPosition(RiskTier::Firm, kFirm) == 0);
    assert(rm.netPosition(RiskTier::Trader, unmapped) == 0);

    // Still allowed afterwards.
    {
        auto d = rm.check(unmapped, Side::Sell, /*price*/1, /*qty*/5, /*ref*/0);
        assert(d.allowed);
    }

    PASS();
}

int main() {
    std::cout << "\n=== Hierarchical Risk Manager Tests ===\n\n";

    test_trader_order_limits();
    test_fat_finger();
    test_firm_aggregate_reject();
    test_netposition_aggregation();
    test_unmapped_always_allowed();

    std::cout << "\n--- Results: " << passed << " passed ---\n";
    std::cout << "\nAll hierarchical risk tests passed.\n\n";
    return 0;
}
