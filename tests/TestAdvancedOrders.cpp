// TestAdvancedOrders — MIT (Market-if-Touched), market-order sweep
// protection, and OCO contingent orders wired through the engine.
//
// What this proves:
//   1. A buy MIT fires a market order when price falls to/through its trigger
//      (favorable-direction mirror of a stop); a sell MIT fires on a rise.
//   2. Market-order sweep protection caps how far a market order walks from
//      the touch price — the unfilled remainder is cancelled, not swept.
//   3. OCO: through the engine, the first leg to fill cancels its sibling;
//      a user-cancelled leg dissolves the group and leaves the survivor live.

#include "MatchingEngine.h"
#include "OrderBook.h"
#include "Types.h"

#include <cassert>
#include <iostream>

using namespace OrderMatcher;

static int tests_passed = 0;
#define SECTION(name) std::cout << "\n  " << name << "..." << std::flush
#define PASS()        do { ++tests_passed; std::cout << " PASS" << std::endl; } while (0)

namespace {
void addLimit(OrderBook& b, OrderId id, ParticipantId p, Side s, Price px, Quantity q) {
    b.addOrder(id, p, s, px, q, OrderType::Limit);
}
} // namespace

// 1. Buy MIT triggers when price touches the level from above.
void test_mit_buy_triggers_on_downward_touch() {
    SECTION("MIT buy fires a market order on a downward touch of the trigger");
    OrderBook book(1);
    book.setCircuitBreakerThreshold(1e9);
    addLimit(book, 1, 1, Side::Sell, 100, 100);            // ask liquidity for the fill
    book.addOrder(2, 2, Side::Buy, 0, 30, OrderType::MIT, /*stopPrice=*/95);
    assert(book.getOrder(2) != nullptr);                   // parked, not yet triggered
    assert(book.getTradeCount() == 0);

    // Crossing pair at 95 drives lastTradePrice down to the trigger.
    addLimit(book, 3, 3, Side::Sell, 95, 10);
    addLimit(book, 4, 4, Side::Buy, 95, 10);               // trades @95
    assert(book.getOrder(2) == nullptr);                   // MIT fired + filled as market buy
    const Order* sell = book.getOrder(1);
    assert(sell != nullptr && sell->remainingQty == 70);   // 30 taken at 100
    PASS();
}

// 2. Sell MIT triggers when price touches the level from below.
void test_mit_sell_triggers_on_upward_touch() {
    SECTION("MIT sell fires a market order on an upward touch of the trigger");
    OrderBook book(1);
    book.setCircuitBreakerThreshold(1e9);
    addLimit(book, 1, 1, Side::Buy, 100, 100);             // bid liquidity for the fill
    book.addOrder(2, 2, Side::Sell, 0, 30, OrderType::MIT, /*stopPrice=*/105);
    assert(book.getOrder(2) != nullptr);

    addLimit(book, 3, 3, Side::Buy, 105, 10);
    addLimit(book, 4, 4, Side::Sell, 105, 10);             // trades @105
    assert(book.getOrder(2) == nullptr);                   // MIT fired + filled as market sell
    const Order* bid = book.getOrder(1);
    assert(bid != nullptr && bid->remainingQty == 70);
    PASS();
}

// 3. Market-order sweep protection collars the walk.
void test_market_sweep_protection() {
    SECTION("market order is collared and does not sweep beyond the protection band");
    OrderBook book(1);
    book.setCircuitBreakerThreshold(1e9);
    book.setMarketProtectionPct(0.05);                     // 5% collar; touch 100 -> bound 105
    addLimit(book, 1, 1, Side::Sell, 100, 10);
    addLimit(book, 2, 1, Side::Sell, 104, 10);
    addLimit(book, 3, 1, Side::Sell, 110, 10);
    book.addOrder(4, 2, Side::Buy, 0, 30, OrderType::Market);

    assert(book.getOrder(1) == nullptr);                   // 100 filled
    assert(book.getOrder(2) == nullptr);                   // 104 filled
    const Order* far = book.getOrder(3);
    assert(far != nullptr && far->remainingQty == 10);     // 110 beyond the collar — untouched
    assert(book.getOrder(4) == nullptr);                   // market remainder cancelled, not resting
    PASS();
}

// 4. OCO: first leg to fill cancels its sibling (end-to-end through the engine).
void test_oco_cancels_sibling_on_fill() {
    SECTION("OCO: filling one leg cancels its sibling through the engine");
    MatchingEngine engine;
    engine.addSymbol(1);
    engine.getOrderBook(1)->setCircuitBreakerThreshold(1e9);
    engine.startAsync(1, 1024);

    engine.processOrder(1, 1, 1, Side::Sell, 105, 10, OrderType::Limit);  // take-profit leg
    engine.processOrder(1, 2, 1, Side::Sell, 95,  10, OrderType::Limit);  // stop leg
    engine.waitForDrain();
    engine.registerOco(1, 1, 2);

    // Aggressor hits the low leg (id 2); OCO must cancel the high leg (id 1).
    engine.processOrder(1, 3, 2, Side::Buy, 95, 10, OrderType::Limit);
    engine.waitForDrain();

    const OrderBook* book = engine.getOrderBook(1);
    assert(book->getOrder(2) == nullptr);                  // low leg filled
    assert(book->getOrder(1) == nullptr);                  // high leg OCO-cancelled
    engine.stopAsync();
    PASS();
}

// 5. OCO: a user-cancelled leg dissolves the group; the survivor stays live.
void test_oco_user_cancel_leaves_sibling() {
    SECTION("OCO: user-cancelling one leg leaves the other live");
    MatchingEngine engine;
    engine.addSymbol(1);
    engine.getOrderBook(1)->setCircuitBreakerThreshold(1e9);
    engine.startAsync(1, 1024);

    engine.processOrder(1, 1, 1, Side::Sell, 105, 10, OrderType::Limit);
    engine.processOrder(1, 2, 1, Side::Sell, 95,  10, OrderType::Limit);
    engine.waitForDrain();
    engine.registerOco(1, 1, 2);

    engine.cancelOrder(1, 2);                              // user cancels the stop leg
    engine.waitForDrain();
    const OrderBook* book = engine.getOrderBook(1);
    assert(book->getOrder(2) == nullptr);                  // cancelled
    assert(book->getOrder(1) != nullptr);                  // sibling survives (group dissolved)

    // Filling the survivor must not try to cancel a ghost sibling.
    engine.processOrder(1, 3, 2, Side::Buy, 105, 10, OrderType::Limit);
    engine.waitForDrain();
    assert(book->getOrder(1) == nullptr);                  // filled normally
    engine.stopAsync();
    PASS();
}

int main() {
    std::cout << "Running advanced-order tests..." << std::endl;
    test_mit_buy_triggers_on_downward_touch();
    test_mit_sell_triggers_on_upward_touch();
    test_market_sweep_protection();
    test_oco_cancels_sibling_on_fill();
    test_oco_user_cancel_leaves_sibling();
    std::cout << "\nAll " << tests_passed << " advanced-order test sections passed." << std::endl;
    return 0;
}
