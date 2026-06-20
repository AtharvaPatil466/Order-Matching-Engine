// TestFeeEngine — maker-taker fee/rebate accrual tests.
//
// Verifies the basis-points fee math, signed maker rebate vs taker fee,
// running accrual across multiple fills, per-participant tier overrides,
// and per-participant / global reset semantics.

#include "FeeEngine.h"

#include <cassert>
#include <iostream>

using namespace OrderMatcher;

static int passed = 0;
#define TEST(name) std::cout << "  " << #name << "..." << std::flush
#define PASS() do { ++passed; std::cout << " PASS\n"; } while(0)

// Maker bps negative -> rebate (negative fee); taker bps positive -> charge.
void test_maker_rebate_and_taker_fee_signs() {
    TEST(MakerRebateAndTakerFeeSigns);

    FeeEngine fees;
    // -2 bps maker rebate, +5 bps taker fee.
    fees.setDefaultSchedule(FeeSchedule{-2.0, 5.0});

    const ParticipantId maker = 1;
    const ParticipantId taker = 2;
    const Price price = 1000000;   // 100.0000 in 4-dp fixed point
    const Quantity qty = 100;

    FillFees f = fees.applyFill(maker, taker, price, qty);

    // notional = 100,000,000. maker = round(notional * -2 / 10000) = -20000.
    //                          taker = round(notional *  5 / 10000) = +50000.
    assert(f.makerFee == -20000);
    assert(f.takerFee == 50000);
    assert(f.makerFee < 0);   // rebate paid to maker
    assert(f.takerFee > 0);   // fee charged to taker

    // Accrual mirrors the per-fill fee for a single fill.
    assert(fees.accruedFee(maker) == -20000);
    assert(fees.accruedFee(taker) == 50000);

    PASS();
}

// Fees accumulate additively across several fills.
void test_accrual_sums_across_fills() {
    TEST(AccrualSumsAcrossFills);

    FeeEngine fees;
    fees.setDefaultSchedule(FeeSchedule{-1.0, 3.0});

    const ParticipantId maker = 10;
    const ParticipantId taker = 20;

    int64_t expectedMaker = 0;
    int64_t expectedTaker = 0;
    for (int i = 1; i <= 4; ++i) {
        const Price price = 10000 * i;     // varying price
        const Quantity qty = 50 + i;       // varying qty
        FillFees f = fees.applyFill(maker, taker, price, qty);
        expectedMaker += f.makerFee;
        expectedTaker += f.takerFee;
    }

    assert(fees.accruedFee(maker) == expectedMaker);
    assert(fees.accruedFee(taker) == expectedTaker);
    // Sanity: maker accrued a (net) rebate, taker accrued a (net) charge.
    assert(expectedMaker < 0);
    assert(expectedTaker > 0);

    // A participant never involved in a fill has accrued nothing.
    assert(fees.accruedFee(999) == 0);

    PASS();
}

// A per-participant schedule overrides the venue default for that participant
// only; each side resolves its own schedule independently.
void test_participant_schedule_overrides_default() {
    TEST(ParticipantScheduleOverridesDefault);

    FeeEngine fees;
    fees.setDefaultSchedule(FeeSchedule{-2.0, 5.0});

    const ParticipantId vipMaker = 7;     // gets a richer rebate via override
    const ParticipantId normalTaker = 8;  // uses the default taker fee
    // VIP maker: -4 bps rebate (better than default -2). Taker side of this
    // override is irrelevant here since the VIP only makes in this fill.
    fees.setParticipantSchedule(vipMaker, FeeSchedule{-4.0, 1.0});

    const Price price = 1000000;   // notional 100,000,000 at qty 100
    const Quantity qty = 100;

    FillFees f = fees.applyFill(vipMaker, normalTaker, price, qty);

    // VIP maker uses its OWN -4 bps: round(1e8 * -4 / 10000) = -40000.
    assert(f.makerFee == -40000);
    // Taker has no override -> default +5 bps: +50000.
    assert(f.takerFee == 50000);

    assert(fees.accruedFee(vipMaker) == -40000);
    assert(fees.accruedFee(normalTaker) == 50000);

    // Now flip roles: the VIP acts as taker. Its override taker bps (+1) apply,
    // not the default +5, proving the override governs both sides.
    FeeEngine fees2;
    fees2.setDefaultSchedule(FeeSchedule{-2.0, 5.0});
    fees2.setParticipantSchedule(vipMaker, FeeSchedule{-4.0, 1.0});
    FillFees g = fees2.applyFill(normalTaker, vipMaker, price, qty);
    // Maker (no override) default -2 bps: -20000. VIP taker +1 bps: +10000.
    assert(g.makerFee == -20000);
    assert(g.takerFee == 10000);

    PASS();
}

// resetParticipant clears one participant; resetAll clears everyone.
void test_reset_participant_and_reset_all() {
    TEST(ResetParticipantAndResetAll);

    FeeEngine fees;
    fees.setDefaultSchedule(FeeSchedule{-2.0, 5.0});

    const ParticipantId a = 100;
    const ParticipantId b = 200;
    const Price price = 500000;
    const Quantity qty = 40;

    fees.applyFill(a, b, price, qty);   // a maker, b taker
    fees.applyFill(b, a, price, qty);   // b maker, a taker
    assert(fees.accruedFee(a) != 0);
    assert(fees.accruedFee(b) != 0);

    // resetParticipant only affects the named participant.
    const int64_t bBefore = fees.accruedFee(b);
    fees.resetParticipant(a);
    assert(fees.accruedFee(a) == 0);
    assert(fees.accruedFee(b) == bBefore);

    // After reset, the participant accrues fresh from zero.
    FillFees f = fees.applyFill(a, b, price, qty);
    assert(fees.accruedFee(a) == f.makerFee);

    // resetAll wipes every participant.
    fees.resetAll();
    assert(fees.accruedFee(a) == 0);
    assert(fees.accruedFee(b) == 0);

    PASS();
}

int main() {
    std::cout << "\n=== FeeEngine Tests ===\n\n";

    test_maker_rebate_and_taker_fee_signs();
    test_accrual_sums_across_fills();
    test_participant_schedule_overrides_default();
    test_reset_participant_and_reset_all();

    std::cout << "\n--- Results: " << passed << " passed ---\n";
    std::cout << "\nAll fee engine tests passed.\n\n";
    return 0;
}
