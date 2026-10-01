// AuctionCapacityTest — no auction container drops an entry silently.
//
// WHAT WAS WRONG (roadmap 1.8-H6, finding AUCT-6). Every auction call site
// ignored FixedVector::push_back's "full" result.
//   (a) Clearing candidates lived in a FixedVector<Price, 4096>, filled with
//       the bid levels first. Past 4096 levels the ask levels were dropped, so
//       the true clearing price was never evaluated: one participant with 4095
//       one-lot bids moved the auction price.
//   (b) The parked lists (auction market orders, on-close orders, stops) hold
//       16384. Order 16385 was allocated, ACKED and put in the lookup, then
//       belonged to no list: it never traded, was never cancelled, and lived
//       forever.
//
// WHAT IS PINNED. Candidates are every populated level on either side, with
// no cap. An order that would not fit its parked list is rejected with
// CapacityExhausted before it is acked.

#include "EventListener.h"
#include "OrderBook.h"

#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <variant>

using namespace OrderMatcher;

namespace {

constexpr int kParkedCapacity = 16384;  // FixedVector size of each parked list
constexpr size_t kPool = 20000;         // keeps pool pressure (95%) out of the way

struct Recorder : EventListener {
    std::map<OrderId, OrderUpdate> last;
    std::map<OrderId, int> accepted;
    void onOrderUpdate(const OrderUpdate& u) override {
        last[u.orderId] = u;
        if (u.status == OrderStatus::Accepted) ++accepted[u.orderId];
    }
};

void candidatesAreNotCapped() {
    OrderBook book(1, MatchAlgorithm::PriceTime, 10000);
    book.setCircuitBreakerThreshold(1e9);
    book.setTradingState(TradingState::AuctionOpen);
    book.addOrder(1, 10, Side::Buy, 10000, 100, OrderType::Limit);
    book.addOrder(2, 20, Side::Sell, 9990, 100, OrderType::Limit);
    book.setReferencePrice(9990);
    const Price honest = book.computeAuctionState().indicativePrice;
    assert(honest == 9990 && "precondition: 9990 and 10000 tie, the reference picks 9990");

    // 4095 one-lot bids below the cross: 4096 bid levels, so the ask level was
    // the 4097th candidate and fell off the end.
    for (int i = 0; i < 4095; ++i)
        book.addOrder(100 + i, 77, Side::Buy, 9989 - i, 1, OrderType::Limit);

    const AuctionResult a = book.computeAuctionState();
    assert(a.hasCross && a.pairedVolume == 100);
    assert(a.indicativePrice == 9990 &&
           "4095 one-lot bids pushed the ask level out of the candidate set");
    book.uncross();
    assert(book.getSnapshot().lastTradePrice == 9990);
    std::puts("  candidates past 4096 levels are still evaluated");
}

// Fills `list` to capacity with `add(id)`, then checks order kParkedCapacity+1
// is refused with CapacityExhausted and was never acked.
template <typename AddFn>
void overflowIsRejectedBeforeAck(const char* what, OrderBook& book, Recorder& rec, AddFn add) {
    for (int i = 1; i <= kParkedCapacity; ++i) {
        const AddOrderResult r = add(static_cast<OrderId>(i));
        assert(std::holds_alternative<OrderId>(r));
    }
    const OrderId over = kParkedCapacity + 1;
    const AddOrderResult r = add(over);
    assert(std::holds_alternative<RejectReason>(r) &&
           std::get<RejectReason>(r) == RejectReason::CapacityExhausted &&
           "the order past the parked list's capacity was accepted");
    assert(rec.accepted[over] == 0 && "the overflowing order was acked before it was refused");
    assert(rec.last[over].status == OrderStatus::Rejected);
    assert(book.getOrder(over) == nullptr && "the refused order is still live in the lookup");
    std::string err;
    assert(book.validateIntegrity(&err));
    std::printf("  %s: order %d refused with CapacityExhausted, never acked\n", what,
                kParkedCapacity + 1);
}

void auctionMarketOverflow() {
    OrderBook book(1, MatchAlgorithm::PriceTime, kPool);
    Recorder rec;
    book.setEventListener(&rec);
    book.setCircuitBreakerThreshold(1e9);
    book.setTradingState(TradingState::AuctionOpen);
    overflowIsRejectedBeforeAck("auction market orders", book, rec, [&](OrderId id) {
        return book.addOrder(id, 10, Side::Buy, 0, 1, OrderType::Market);
    });

    // Every accepted market order trades or is cancelled at the cross.
    book.addOrder(100000, 20, Side::Sell, 100, kParkedCapacity, OrderType::Limit);
    book.uncross();
    for (int i = 1; i <= kParkedCapacity; ++i)
        assert(book.getOrder(static_cast<OrderId>(i)) == nullptr);
}

void onCloseOverflow() {
    OrderBook book(1, MatchAlgorithm::PriceTime, kPool);
    Recorder rec;
    book.setEventListener(&rec);
    overflowIsRejectedBeforeAck("on-close orders", book, rec, [&](OrderId id) {
        return book.addOrder(id, 10, Side::Sell, 0, 1, OrderType::MOC);
    });
}

void stopOverflow() {
    OrderBook book(1, MatchAlgorithm::PriceTime, kPool);
    Recorder rec;
    book.setEventListener(&rec);
    overflowIsRejectedBeforeAck("stop orders", book, rec, [&](OrderId id) {
        return book.addOrder(id, 10, Side::Buy, 200, 1, OrderType::Stop, /*stopPrice=*/150);
    });
}

}  // namespace

int main() {
    candidatesAreNotCapped();
    auctionMarketOverflow();
    onCloseOverflow();
    stopOverflow();
    std::puts("AuctionCapacityTest passed");
    return 0;
}
