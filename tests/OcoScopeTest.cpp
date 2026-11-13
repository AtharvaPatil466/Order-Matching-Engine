// OcoScopeTest — an OCO link names one live order on one symbol (MIDS-2, MIDS-9).
//
// Links were keyed by order id alone, engine-wide: a fill of another symbol's
// order with the same id cancelled the linked sibling's namesake there and
// disarmed the real pair. A link also outlived its order when the order left
// by any path but the async cancel, so a later order reusing the id inherited
// it. And the OCO's sibling cancel must give back the sibling's reserved
// position (MIDS-9).

#include "MatchingEngine.h"
#include "OrderBook.h"

#include <gtest/gtest.h>

using namespace OrderMatcher;

namespace {
constexpr Price PX = 1000000;

void quietBreakers(MatchingEngine& e, std::initializer_list<SymbolId> syms) {
    for (SymbolId s : syms) e.getOrderBook(s)->setCircuitBreakerThreshold(1e9);
}
}  // namespace

TEST(OcoScope, AFillOnAnotherSymbolWithTheSameIdDoesNothing) {
    MatchingEngine e;
    e.addSymbol(0);
    e.addSymbol(1);
    quietBreakers(e, {0, 1});
    e.startAsync(2, 1024);
    e.submitOrder(0, 7, 1, Side::Sell, PX + 100, 10, OrderType::Limit);
    e.submitOrder(0, 8, 1, Side::Sell, PX + 200, 10, OrderType::Limit);
    e.submitOrder(1, 8, 2, Side::Buy, PX - 500, 10, OrderType::Limit);
    e.submitOrder(1, 7, 2, Side::Sell, PX, 5, OrderType::Limit);
    e.waitForDrain();
    e.registerOco(0, 7, 8);
    e.submitOrder(1, 99, 3, Side::Buy, PX, 5, OrderType::Limit);  // fills P2's #7 on symbol 1
    e.waitForDrain();
    EXPECT_NE(e.getOrderBook(1)->getOrder(8), nullptr) << "a stranger's order was cancelled";
    e.submitOrder(0, 100, 4, Side::Buy, PX + 100, 10, OrderType::Limit);  // the real leg fills
    e.waitForDrain();
    EXPECT_EQ(e.getOrderBook(0)->getOrder(8), nullptr) << "the real OCO was disarmed";
    e.stopAsync();
}

TEST(OcoScope, ALinkDiesWithItsOrderSoAReusedIdDoesNotInheritIt) {
    MatchingEngine e;
    e.addSymbol(0);
    e.start();
    quietBreakers(e, {0});
    e.submitOrder(0, 7, 1, Side::Sell, PX + 100, 10, OrderType::Limit);
    e.submitOrder(0, 8, 1, Side::Sell, PX + 200, 10, OrderType::Limit);
    e.registerOco(0, 7, 8);
    e.submitCancel(0, 7, 1);                                            // leg #7 leaves
    e.submitOrder(0, 7, 2, Side::Sell, PX + 50, 10, OrderType::Limit);  // P2 reuses id 7
    e.submitOrder(0, 100, 4, Side::Buy, PX + 50, 10, OrderType::Limit); // and it fills
    EXPECT_NE(e.getOrderBook(0)->getOrder(8), nullptr) << "P1's #8 cancelled by P2's fill";
}

TEST(OcoScope, AnOcoCancelledSiblingReleasesItsReservedPosition) {
    MatchingEngine e;
    e.addSymbol(0);
    quietBreakers(e, {0});
    e.setPositionLimit(1, 20);
    e.startAsync(1, 1024);
    e.submitOrder(0, 7, 1, Side::Sell, PX + 100, 10, OrderType::Limit);
    e.submitOrder(0, 8, 1, Side::Sell, PX + 200, 10, OrderType::Limit);
    e.waitForDrain();
    e.registerOco(0, 7, 8);
    e.submitOrder(0, 100, 4, Side::Buy, PX + 100, 10, OrderType::Limit);  // #7 fills: -10
    e.waitForDrain();
    ASSERT_EQ(e.getOrderBook(0)->getOrder(8), nullptr);
    const auto r = e.submitOrder(0, 9, 1, Side::Sell, PX + 300, 10, OrderType::Limit);
    e.waitForDrain();
    EXPECT_TRUE(r.isAccepted()) << "reason " << static_cast<int>(r.rejectReason);
    EXPECT_NE(e.getOrderBook(0)->getOrder(9), nullptr);
    e.stopAsync();
}
