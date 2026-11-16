// ProRataLevelTest — pro-rata allocates over the whole level, and an elected
// stop in a pro-rata book is allocated pro-rata (MR-4).
//
// Allocation stopped at the first 1,024 orders of a level: their shares were
// computed against the whole level, the shortfall went FIFO to the same front
// orders (about twice their share), order 1,025 on got nothing, and a DMM/LMM
// order past index 1,023 lost its floor. Elected stops and trailing stops
// called the price-time match() whatever the book's algorithm.

#include "EventListener.h"
#include "OrderBook.h"

#include <gtest/gtest.h>

#include <map>

using namespace OrderMatcher;

namespace {

struct Fills : EventListener {
    std::map<OrderId, Quantity> qty;
    void onTrade(const Trade& t) override {
        qty[t.buyOrderId] += t.quantity;
        qty[t.sellOrderId] += t.quantity;
    }
};

constexpr Price PX = 100000;

struct ProRataBook {
    OrderBook book{0, MatchAlgorithm::ProRata};
    Fills fills;
    ProRataBook() {
        book.setEventListener(&fills);
        book.setCircuitBreakerThreshold(1e9);
    }
};

}  // namespace

TEST(ProRataLevel, EveryOrderOfALevelPastTheFirst1024GetsItsShare) {
    ProRataBook b;
    constexpr OrderId kOrders = 2048;
    for (OrderId id = 1; id <= kOrders; ++id)
        b.book.addOrder(id, static_cast<ParticipantId>(id), Side::Sell, PX, 10, OrderType::Limit);
    b.book.addOrder(99999, 9999, Side::Buy, PX, kOrders * 5, OrderType::Limit);  // half the level
    EXPECT_EQ(b.fills.qty[1], 5u) << "the front order got more than its share";
    EXPECT_EQ(b.fills.qty[1025], 5u);
    EXPECT_EQ(b.fills.qty[kOrders], 5u) << "the back of the level got nothing";
}

TEST(ProRataLevel, ADmmPastIndex1023KeepsItsFloor) {
    ProRataBook b;
    constexpr OrderId kOrders = 1100;
    constexpr ParticipantId kDmm = 5000;
    b.book.setParticipantRole(kDmm, ParticipantRole::DMM);
    for (OrderId id = 1; id < kOrders; ++id)
        b.book.addOrder(id, static_cast<ParticipantId>(id), Side::Sell, PX, 10, OrderType::Limit);
    b.book.addOrder(kOrders, kDmm, Side::Sell, PX, 10, OrderType::Limit);
    b.book.addOrder(99999, 9999, Side::Buy, PX, kOrders, OrderType::Limit);  // 10% of the level
    EXPECT_GE(b.fills.qty[kOrders], 4u) << "the DMM's 40% floor was lost";
}

TEST(ProRataLevel, AnElectedStopIsAllocatedProRata) {
    ProRataBook b;
    b.book.addOrder(1, 1, Side::Sell, PX, 10, OrderType::Limit);
    b.book.addOrder(2, 2, Side::Sell, PX, 30, OrderType::Limit);
    b.book.addOrder(5, 5, Side::Buy, 0, 20, OrderType::Stop, /*stopPrice=*/PX);
    b.book.addOrder(3, 3, Side::Buy, PX, 4, OrderType::Limit);  // 1 + 3, prints PX: elects #5
    // Pro-rata on 9 / 27: #1 5, #2 15. Price-time would give #1 its 9.
    EXPECT_EQ(b.fills.qty[5], 20u);
    EXPECT_EQ(b.fills.qty[1], 1u + 5u);
    EXPECT_EQ(b.fills.qty[2], 3u + 15u);
}
