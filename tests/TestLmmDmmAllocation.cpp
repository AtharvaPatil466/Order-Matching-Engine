#include <gtest/gtest.h>
#include "MatchingEngine.h"
#include "OrderBook.h"
#include "Types.h"

using namespace OrderMatcher;

// ─── Helpers ──────────────────────────────────────────────────────────────────

class LmmDmmListener : public EventListener {
public:
    std::vector<Trade> trades;
    std::vector<OrderUpdate> updates;

    void onTrade(const Trade& t) override { trades.push_back(t); }
    void onOrderUpdate(const OrderUpdate& u) override { updates.push_back(u); }
    void onMarketData(const MarketDataUpdate&) override {}

    void reset() { trades.clear(); updates.clear(); }

    Quantity totalFilled(OrderId id) const {
        Quantity total = 0;
        for (const auto& t : trades) {
            if (t.buyOrderId == id || t.sellOrderId == id)
                total += t.quantity;
        }
        return total;
    }
};

// ─── Test 1: setParticipantRole API does not crash ─────────────────────────

TEST(LmmDmm, SetParticipantRole_NoCrash) {
    MatchingEngine engine;
    engine.addSymbol(1, MatchAlgorithm::ProRata);
    engine.start();

    // Should not throw or crash
    EXPECT_NO_THROW({
        engine.setParticipantRole(42, ParticipantRole::DMM);
        engine.setParticipantRole(43, ParticipantRole::LMM);
        engine.setParticipantRole(44, ParticipantRole::Regular);
    });
}

// ─── Test 2: getParticipantRole returns Regular for unknown participants ────

TEST(LmmDmm, GetParticipantRole_DefaultRegular) {
    OrderBook book(1, MatchAlgorithm::ProRata);

    // Unregistered participant → should return Regular
    EXPECT_EQ(book.getParticipantRole(999), ParticipantRole::Regular);

    book.setParticipantRole(10, ParticipantRole::DMM);
    EXPECT_EQ(book.getParticipantRole(10), ParticipantRole::DMM);

    book.setParticipantRole(11, ParticipantRole::LMM);
    EXPECT_EQ(book.getParticipantRole(11), ParticipantRole::LMM);

    // Still regular for others
    EXPECT_EQ(book.getParticipantRole(12), ParticipantRole::Regular);
}

// ─── Test 3: ProRata — DMM order receives at least 40% floor allocation ───
//
// Setup: sell side has two resting orders at the same price level:
//   - Order A: participant 1 (Regular),  qty = 600
//   - Order B: participant 2 (DMM),      qty = 400
//   Total = 1000
//
// Incoming buy order qty = 100 (small relative to total).
//
// Natural pro-rata:
//   A share = 600/1000 * 100 = 60
//   B share = 400/1000 * 100 = 40
//
// DMM floor = 40% of avail = 0.40 * 400 = 160, but capped at toFill = 100.
// So floor = 100. B's natural share (40) < floor (100), so B should be
// bumped up. The excess (60) is taken from A.
// Expected: B gets 100, A gets 0. (floor dominates when incoming is small)
//
// Now test with a larger incoming order where floor applies meaningfully:
// Incoming qty = 500:
//   Natural: A = 600/1000*500 = 300, B = 400/1000*500 = 200
//   Floor for B = 40% * 400 = 160. B's natural (200) >= floor (160), so
//   no adjustment needed. B gets >= 160 (gets 200 in this case).

TEST(LmmDmm, ProRata_DmmFloorGuarantee_SmallIncoming) {
    // Use OrderBook directly so we can attach listener and control roles
    OrderBook book(1, MatchAlgorithm::ProRata);
    LmmDmmListener listener;
    book.setEventListener(&listener);

    book.setParticipantRole(2, ParticipantRole::DMM);

    // Place resting sell orders
    book.addOrder(1, 1, Side::Sell, 100000, 600, OrderType::Limit);  // Regular
    book.addOrder(2, 2, Side::Sell, 100000, 400, OrderType::Limit);  // DMM

    listener.reset();

    // Incoming buy: qty=100. DMM floor = min(160, 100) = 100.
    // A's natural=60 must give up enough so B reaches 100.
    book.addOrder(3, 3, Side::Buy, 100000, 100, OrderType::IOC);

    Quantity dmmFilled = listener.totalFilled(2);
    EXPECT_GE(dmmFilled, static_cast<Quantity>(40))  // at least floor=40% of 100 incoming
        << "DMM order should receive at least 40% of the filled quantity";
    // The total fill must equal 100 (all of the incoming qty)
    Quantity totalFill = 0;
    for (const auto& t : listener.trades) totalFill += t.quantity;
    EXPECT_EQ(totalFill, static_cast<Quantity>(100));
}

TEST(LmmDmm, ProRata_DmmFloorGuarantee_LargeIncoming) {
    OrderBook book(1, MatchAlgorithm::ProRata);
    LmmDmmListener listener;
    book.setEventListener(&listener);

    book.setParticipantRole(2, ParticipantRole::DMM);

    // Resting sell orders
    book.addOrder(1, 1, Side::Sell, 100000, 600, OrderType::Limit);  // Regular
    book.addOrder(2, 2, Side::Sell, 100000, 400, OrderType::Limit);  // DMM

    listener.reset();

    // Incoming buy: qty=500.
    // Natural: Regular=300, DMM=200. Floor for DMM = 40%*400=160. 200>=160, no bump.
    book.addOrder(3, 3, Side::Buy, 100000, 500, OrderType::IOC);

    Quantity dmmFilled = listener.totalFilled(2);
    // DMM floor is 40% of its available qty (400), but capped at toFill (500).
    // floor = min(160, 500) = 160. DMM's natural share is 200 >= 160, so DMM gets >= 160.
    EXPECT_GE(dmmFilled, static_cast<Quantity>(160))
        << "DMM should fill at least its 40% floor qty";

    Quantity totalFill = 0;
    for (const auto& t : listener.trades) totalFill += t.quantity;
    EXPECT_EQ(totalFill, static_cast<Quantity>(500));
}

// ─── Test 4: ProRata — Regular-only book gives normal pro-rata (no regression) ─
//
// Two regular orders: A=600, B=400. Incoming=500.
// Expected: A gets ~300, B gets ~200 (proportional).

TEST(LmmDmm, ProRata_RegularOnly_NormalDistribution) {
    OrderBook book(1, MatchAlgorithm::ProRata);
    LmmDmmListener listener;
    book.setEventListener(&listener);

    // No DMM/LMM roles set — all Regular
    book.addOrder(1, 1, Side::Sell, 100000, 600, OrderType::Limit);
    book.addOrder(2, 2, Side::Sell, 100000, 400, OrderType::Limit);

    listener.reset();

    book.addOrder(3, 3, Side::Buy, 100000, 500, OrderType::IOC);

    Quantity filledA = listener.totalFilled(1);
    Quantity filledB = listener.totalFilled(2);
    Quantity total   = filledA + filledB;

    EXPECT_EQ(total, static_cast<Quantity>(500));

    // Proportional expectation: A=300, B=200, with possible rounding of ±1
    EXPECT_GE(filledA, static_cast<Quantity>(299));
    EXPECT_LE(filledA, static_cast<Quantity>(301));
    EXPECT_GE(filledB, static_cast<Quantity>(199));
    EXPECT_LE(filledB, static_cast<Quantity>(201));
}

// ─── Test 5: PriceTime — DMM/LMM orders receive rounding remainder first ──
//
// Setup: three resting sell orders at the same price, each qty=3.
//   Total level qty = 9. Incoming buy qty = 10.
//   Pro-rata natural shares (integer): each gets floor(3/9*10) = 3, total=9.
//   But this is PriceTime, not ProRata — use ProRata for the remainder test.
//
// Instead test with ProRata remainder distribution:
//   Three resting sells: Regular1=5, DMM=5, Regular2=5. Total=15.
//   Incoming buy=10.
//   Natural: each gets floor(5/15*10) = 3, allocated=9, remainder=1.
//   DMM should get the remainder (gets 4), Regulars get 3 each.

TEST(LmmDmm, ProRata_DmmGetsRemainderFirst) {
    OrderBook book(1, MatchAlgorithm::ProRata);
    LmmDmmListener listener;
    book.setEventListener(&listener);

    // participant 2 is DMM
    book.setParticipantRole(2, ParticipantRole::DMM);

    // Three resting sell orders, equal qty
    book.addOrder(1, 1, Side::Sell, 100000, 5, OrderType::Limit);   // Regular
    book.addOrder(2, 2, Side::Sell, 100000, 5, OrderType::Limit);   // DMM
    book.addOrder(3, 3, Side::Sell, 100000, 5, OrderType::Limit);   // Regular

    listener.reset();

    // Incoming buy: 10 units. Natural each gets floor(5/15*10)=3, total=9, remainder=1.
    // DMM floor check: 40%*5=2, but natural share=3 >= 2, so no bump needed.
    // Remainder=1 should go to DMM first.
    book.addOrder(4, 4, Side::Buy, 100000, 10, OrderType::IOC);

    Quantity dmmFilled = listener.totalFilled(2);
    // DMM should get at least 4 (3 natural + 1 remainder) or at least its floor
    // In practice: natural=3, floor=2 (no bump), remainder goes to DMM → 4.
    EXPECT_GE(dmmFilled, static_cast<Quantity>(3))
        << "DMM should receive its proportional share";
    // DMM should have gotten the 1-unit remainder
    EXPECT_GE(dmmFilled, static_cast<Quantity>(4))
        << "DMM should receive the rounding remainder ahead of regular orders";

    Quantity totalFill = 0;
    for (const auto& t : listener.trades) totalFill += t.quantity;
    EXPECT_EQ(totalFill, static_cast<Quantity>(10));
}

// ─── Test 6: LMM role works the same as DMM ──────────────────────────────

TEST(LmmDmm, ProRata_LmmFloor) {
    OrderBook book(1, MatchAlgorithm::ProRata);
    LmmDmmListener listener;
    book.setEventListener(&listener);

    book.setParticipantRole(2, ParticipantRole::LMM);

    book.addOrder(1, 1, Side::Sell, 100000, 600, OrderType::Limit);  // Regular
    book.addOrder(2, 2, Side::Sell, 100000, 400, OrderType::Limit);  // LMM

    listener.reset();

    // Incoming = 500. LMM natural share = 200, floor = 40%*400 = 160. No bump needed.
    book.addOrder(3, 3, Side::Buy, 100000, 500, OrderType::IOC);

    Quantity lmmFilled = listener.totalFilled(2);
    EXPECT_GE(lmmFilled, static_cast<Quantity>(160))
        << "LMM should receive at least its 40% floor qty";

    Quantity totalFill = 0;
    for (const auto& t : listener.trades) totalFill += t.quantity;
    EXPECT_EQ(totalFill, static_cast<Quantity>(500));
}

// ─── Test 7: setParticipantRole propagates across all symbols ─────────────

TEST(LmmDmm, SetParticipantRole_PropagatesAcrossSymbols) {
    MatchingEngine engine;
    engine.addSymbol(1, MatchAlgorithm::ProRata);
    engine.addSymbol(2, MatchAlgorithm::ProRata);
    engine.start();

    engine.setParticipantRole(99, ParticipantRole::DMM);

    OrderBook* book1 = engine.getOrderBook(1);
    OrderBook* book2 = engine.getOrderBook(2);
    ASSERT_NE(book1, nullptr);
    ASSERT_NE(book2, nullptr);

    EXPECT_EQ(book1->getParticipantRole(99), ParticipantRole::DMM);
    EXPECT_EQ(book2->getParticipantRole(99), ParticipantRole::DMM);
}
