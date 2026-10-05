// ReplaceQuantityTest — cancelReplace's newQty is the order's new TOTAL (FIX
// OrderQty semantics), and an order never executes more than its total
// (roadmap 1.5-E1, MATCH-4; GTDIFF-1, GTS-1, OTM-6).
//
// WHAT WAS WRONG. Filled was derived as initialQty - remainingQty, and
// cancelReplace set remainingQty = newQty with initialQty left alone, so
// newQty meant "new leaves" and every later report was off by the fills:
//   - an order for 100 with 60 filled, replaced "to 100", executed 160;
//   - an unfilled 100 grown to 120 reported filledQty 18446744073709551596
//     on its cancel (100 - 120, wrapped);
//   - a modify-down from 50 to 30 reported a phantom fill of 20 on its cancel.
// Roadmap 0.11 stopped the bleeding by refusing any quantity change on a
// partially filled order and any growth at all.
//
// THE RULE NOW. The order records what it executed (cumQty). A replace to
// total T is refused (InvalidQuantity) when T <= cumQty; otherwise the order's
// total becomes T and its leaves T - cumQty. Growth and shrink both work on a
// partially filled order. modifyOrder keeps its leaves semantics (it only
// shrinks) and lowers the total by the same amount, so filled + leaves ==
// total holds on every path and no report is derived from a stale size.

#include "OrderBook.h"

#include <cassert>
#include <cstdio>
#include <vector>

using namespace OrderMatcher;

namespace {

constexpr Price kPx = 10000;

struct Tape : EventListener {
    std::vector<Trade> trades;
    std::vector<OrderUpdate> updates;
    void onTrade(const Trade& t) override { trades.push_back(t); }
    void onOrderUpdate(const OrderUpdate& u) override { updates.push_back(u); }

    Quantity executedBy(OrderId id) const {
        Quantity q = 0;
        for (const auto& t : trades)
            if (t.buyOrderId == id || t.sellOrderId == id) q += t.quantity;
        return q;
    }
    const OrderUpdate* last(OrderId id) const {
        for (auto it = updates.rbegin(); it != updates.rend(); ++it)
            if (it->orderId == id) return &*it;
        return nullptr;
    }
};

struct Fixture {
    OrderBook book{1};
    Tape tape;
    Fixture() {
        book.setCircuitBreakerThreshold(1e9);
        book.setEventListener(&tape);
    }
    // Buy 100 @ kPx, 60 of it filled by a resting-side sell.
    void buy100Filled60() {
        book.addOrder(1, 1, Side::Buy, kPx, 100, OrderType::Limit);
        book.addOrder(2, 2, Side::Sell, kPx, 60, OrderType::Limit);
        assert(book.getOrder(1)->remainingQty == 40);
    }
};

void replaceToSameTotalNeverOverfills() {
    Fixture f;
    f.buy100Filled60();

    RejectReason why = RejectReason::None;
    assert(f.book.cancelReplace(1, kPx - 1, 100, why) && why == RejectReason::None &&
           "a reprice that restates the original total must be accepted");
    assert(f.book.getOrder(1)->remainingQty == 40 &&
           "total 100 with 60 filled leaves 40, not 100");

    f.book.addOrder(3, 3, Side::Sell, kPx - 1, 500, OrderType::Limit);
    assert(f.tape.executedBy(1) == 100 && "executed more than the 100 it was sent for");
    const OrderUpdate* u = f.tape.last(1);
    assert(u && u->status == OrderStatus::Filled && u->filledQty == 100);
    std::puts("  100 with 60 filled, replaced to 100: executes exactly 100");
}

void totalAtOrBelowExecutedIsRefused() {
    Fixture f;
    f.buy100Filled60();

    RejectReason why = RejectReason::None;
    assert(!f.book.cancelReplace(1, kPx - 1, 60, why) && why == RejectReason::InvalidQuantity &&
           "a total equal to what already executed leaves nothing to work");
    assert(!f.book.cancelReplace(1, kPx - 1, 40, why) && why == RejectReason::InvalidQuantity &&
           "a total below what already executed is not an order");
    assert(f.book.getOrder(1)->remainingQty == 40 && f.book.getOrder(1)->price == kPx &&
           "a refused replace must leave the order untouched");
    std::puts("  total <= executed: refused, order untouched");
}

void partiallyFilledOrderGrows() {
    Fixture f;
    f.buy100Filled60();

    assert(f.book.cancelReplace(1, kPx, 150) && "growth of a partially filled order");
    assert(f.book.getOrder(1)->remainingQty == 90);

    f.book.addOrder(3, 3, Side::Sell, kPx, 500, OrderType::Limit);
    assert(f.tape.executedBy(1) == 150);
    const OrderUpdate* u = f.tape.last(1);
    assert(u && u->status == OrderStatus::Filled && u->filledQty == 150 &&
           "Filled must report the new total, which is what executed");
    std::puts("  100 with 60 filled, replaced to 150: works 90 more, Filled at 150");
}

void partiallyFilledOrderShrinksAndKeepsPriority() {
    Fixture f;
    f.buy100Filled60();
    f.book.addOrder(4, 4, Side::Buy, kPx, 10, OrderType::Limit);  // queued behind #1

    assert(f.book.cancelReplace(1, kPx, 80) && "shrink of a partially filled order");
    assert(f.book.getOrder(1)->remainingQty == 20);

    // Still ahead of #4: a same-price shrink keeps time priority.
    f.book.addOrder(5, 5, Side::Sell, kPx, 20, OrderType::Limit);
    assert(f.tape.executedBy(1) == 80 && f.tape.executedBy(4) == 0);

    f.tape.updates.clear();
    f.book.addOrder(6, 1, Side::Buy, kPx - 5, 30, OrderType::Limit);
    f.book.addOrder(7, 2, Side::Sell, kPx - 5, 10, OrderType::Limit);  // #4 first, #6 untouched
    assert(f.book.cancelReplace(6, kPx - 5, 20));
    f.book.cancelOrder(6);
    const OrderUpdate* u = f.tape.last(6);
    assert(u && u->status == OrderStatus::Cancelled && u->filledQty == 0 &&
           "a resize of an unfilled order reported a phantom fill");
    std::puts("  100 with 60 filled, replaced to 80: 20 left, priority kept");
}

void unfilledOrderGrows() {
    Fixture f;
    f.book.addOrder(1, 1, Side::Buy, kPx, 100, OrderType::Limit);
    assert(f.book.cancelReplace(1, kPx, 120));
    assert(f.book.getOrder(1)->remainingQty == 120);

    f.book.cancelOrder(1);
    const OrderUpdate* u = f.tape.last(1);
    assert(u && u->status == OrderStatus::Cancelled);
    assert(u->filledQty == 0 && "the cancel report's filledQty wrapped (100 - 120)");
    std::puts("  unfilled 100 -> 120: accepted, cancel reports filled 0");
}

void modifyDownReportsNoPhantomFill() {
    Fixture f;
    f.book.addOrder(1, 1, Side::Sell, kPx, 50, OrderType::Limit);
    assert(f.book.modifyOrder(1, 30));
    f.book.cancelOrder(1);
    const OrderUpdate* u = f.tape.last(1);
    assert(u && u->status == OrderStatus::Cancelled);
    assert(u->filledQty == 0 && "a modify-down was reported as a fill of 20");

    // After a partial fill a modify lowers the total too: 100, 60 filled,
    // leaves cut to 10 -> total 70. A replace that restates 70 is price-only.
    f.buy100Filled60();
    assert(f.book.modifyOrder(1, 10));
    assert(!f.book.cancelReplace(1, kPx - 1, 60) && "total must still exceed executed");
    assert(f.book.cancelReplace(1, kPx - 1, 70));
    assert(f.book.getOrder(1)->remainingQty == 10);
    f.book.cancelOrder(1);
    u = f.tape.last(1);
    assert(u && u->filledQty == 60 && "the cancel must report what executed, 60");
    std::puts("  modify-down: no phantom fill; total follows the cut");
}

void icebergGrowthTakesAFreshSlice() {
    Fixture f;
    f.book.addOrder(1, 1, Side::Sell, kPx, 30, OrderType::Iceberg, 0, /*displayQty=*/10);
    f.book.addOrder(2, 2, Side::Buy, kPx, 25, OrderType::IOC);  // 25 of 30 filled
    assert(f.book.getOrder(1)->remainingQty == 5);

    assert(f.book.cancelReplace(1, kPx, 100));
    const Order* ice = f.book.getOrder(1);
    assert(ice->remainingQty == 75 && ice->visibleQty == 10);
    f.book.addOrder(3, 3, Side::Buy, kPx, 1000, OrderType::IOC);
    assert(f.tape.executedBy(1) == 100);
    std::puts("  iceberg 30 with 25 filled, replaced to 100: executes exactly 100");
}

}  // namespace

int main() {
    replaceToSameTotalNeverOverfills();
    totalAtOrBelowExecutedIsRefused();
    partiallyFilledOrderGrows();
    partiallyFilledOrderShrinksAndKeepsPriority();
    unfilledOrderGrows();
    modifyDownReportsNoPhantomFill();
    icebergGrowthTakesAFreshSlice();
    std::puts("ReplaceQuantityTest passed");
    return 0;
}
