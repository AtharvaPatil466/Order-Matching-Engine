// ReplaceQuantityTest — a replace can never make an order execute more than it
// was sent for, and never wraps the filled quantity (roadmap 0.11, MATCH-4).
//
// WHAT WAS WRONG. cancelReplace set remainingQty = newQty and left initialQty
// alone, and filled is derived as initialQty - remainingQty. Nothing defined
// whether newQty was the new total (FIX) or the new leaves. At HEAD:
//   - an order for 100 with 60 filled, replaced "to 100", executed 160;
//   - an unfilled 100 grown to 120 reported filledQty 18446744073709551596 on
//     its cancel (100 - 120, wrapped);
//   - an unfilled 50 cut to 30 reported a phantom fill of 20 on its cancel.
//
// THE RULE (until 1.5-E1 defines newQty as the new total):
//   - newQty == remainingQty is always allowed (a price-only replace, in
//     leaves terms, on any order);
//   - a quantity change is allowed only on a completely unfilled order, and
//     never above its original size; the new size becomes the order's size;
//   - anything else is refused with InvalidQuantity (FIX OrdRejReason 13,
//     OUCH invalid-quantity, native "invalid quantity"), order untouched.
// On an unfilled order total == leaves, so both readings of newQty agree, and
// the OUCH/FIX gateways (which have no modify message) keep resize-down.

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
};

void partiallyFilledOrderCannotBeResized() {
    OrderBook book(1);
    book.setCircuitBreakerThreshold(1e9);
    Tape tape;
    book.setEventListener(&tape);

    book.addOrder(1, 1, Side::Buy, kPx, 100, OrderType::Limit);
    book.addOrder(2, 2, Side::Sell, kPx, 60, OrderType::Limit);   // fills 60 of 100
    assert(book.getOrder(1)->remainingQty == 40);

    RejectReason why = RejectReason::None;
    assert(!book.cancelReplace(1, kPx + 1, 100, why) &&
           "a replace resized a partially filled order (100 with 60 filled -> 100)");
    assert(why == RejectReason::InvalidQuantity);
    assert(!book.cancelReplace(1, kPx + 1, 30) && "shrink of a partially filled order");
    assert(book.getOrder(1)->remainingQty == 40 && book.getOrder(1)->price == kPx &&
           "a refused replace must leave the order untouched");

    // Whatever arrives, order 1 can execute at most its remaining 40.
    book.addOrder(3, 3, Side::Sell, kPx - 1, 200, OrderType::Limit);
    assert(tape.executedBy(1) == 100 && "executed more than the 100 it was sent for");
    std::puts("  partially filled: resize refused, executed exactly 100 of 100");
}

void priceOnlyReplaceOfLeavesStillWorks() {
    OrderBook book(1);
    book.setCircuitBreakerThreshold(1e9);
    book.addOrder(1, 1, Side::Buy, kPx, 100, OrderType::Limit);
    book.addOrder(2, 2, Side::Sell, kPx, 60, OrderType::Limit);

    RejectReason why = RejectReason::None;
    assert(book.cancelReplace(1, kPx - 5, 40, why) && why == RejectReason::None &&
           "newQty == remainingQty on a partially filled order must be accepted");
    assert(book.getOrder(1)->price == kPx - 5 && book.getOrder(1)->remainingQty == 40);
    std::puts("  partially filled: price-only replace (newQty == leaves) accepted");
}

void unfilledOrderCannotGrow() {
    OrderBook book(1);
    book.setCircuitBreakerThreshold(1e9);
    Tape tape;
    book.setEventListener(&tape);

    book.addOrder(1, 1, Side::Buy, kPx, 100, OrderType::Limit);
    RejectReason why = RejectReason::None;
    assert(!book.cancelReplace(1, kPx, 120, why) &&
           "a replace grew an order past its original size");
    assert(why == RejectReason::InvalidQuantity);
    assert(book.getOrder(1)->remainingQty == 100);

    tape.updates.clear();
    book.cancelOrder(1);
    assert(!tape.updates.empty() && tape.updates.back().status == OrderStatus::Cancelled);
    assert(tape.updates.back().filledQty == 0 &&
           "the cancel report's filledQty wrapped (initialQty - remainingQty < 0)");
    std::puts("  unfilled: growth refused, cancel reports filled 0");
}

void unfilledOrderCanShrink() {
    OrderBook book(1);
    book.setCircuitBreakerThreshold(1e9);
    Tape tape;
    book.setEventListener(&tape);

    // OUCH's case: an unfilled 50 replaced to 30 at a new price.
    book.addOrder(1, 1, Side::Buy, kPx, 50, OrderType::Limit);
    assert(book.cancelReplace(1, kPx + 10, 30) && "an unfilled decrease must succeed");
    assert(book.getOrder(1)->remainingQty == 30 && book.getOrder(1)->price == kPx + 10);

    // Still unfilled, so it can shrink again (same price this time).
    assert(book.cancelReplace(1, kPx + 10, 20) && "a second unfilled decrease must succeed");
    assert(book.getOrder(1)->remainingQty == 20);

    tape.updates.clear();
    book.cancelOrder(1);
    assert(!tape.updates.empty() && tape.updates.back().status == OrderStatus::Cancelled);
    assert(tape.updates.back().filledQty == 0 &&
           "a resize reported a phantom fill (the old size minus the new)");

    // And once resized, it executes exactly its new size.
    book.addOrder(5, 1, Side::Buy, kPx, 50, OrderType::Limit);
    assert(book.cancelReplace(5, kPx, 30));
    book.addOrder(6, 2, Side::Sell, kPx, 100, OrderType::Limit);
    assert(tape.executedBy(5) == 30);
    bool sawFilled = false;
    for (const auto& u : tape.updates)
        if (u.orderId == 5 && u.status == OrderStatus::Filled) {
            assert(u.filledQty == 30 && "Filled reported the pre-resize size");
            sawFilled = true;
        }
    assert(sawFilled);
    std::puts("  unfilled: 50 -> 30 -> 20 accepted, no phantom fill, fills exactly 30");
}

}  // namespace

int main() {
    partiallyFilledOrderCannotBeResized();
    priceOnlyReplaceOfLeavesStillWorks();
    unfilledOrderCannotGrow();
    unfilledOrderCanShrink();
    std::puts("ReplaceQuantityTest passed");
    return 0;
}
