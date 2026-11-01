// MarketMinQtyTest — audit OTM-2, roadmap 1.6-F2.
//
// WHAT WAS WRONG. screenMinQty read a Market order's price (0) as a limit. A
// market buy carrying minQty saw no ask at or below 0, failed its minimum,
// did not "cross" at 0 — and was rested: a MARKET order in the bids at price
// 0, with the asks it never traded against still there. The next market sell
// swept down to it and printed at 0 (audit h3 U1: `TRADE buy=3 sell=5 px=0
// q=15`). The 0.12 stop-gap refused Market+minQty outright.
//
// WHAT IS PINNED. Market+minQty behaves as IOC+minQty: the minimum counts
// liquidity at every level; met, the order trades and its remainder is
// cancelled; not met, it is cancelled untraded. Either way nothing rests and
// nothing prints at 0.

#include "OrderBook.h"

#include <cassert>
#include <cstdio>
#include <variant>
#include <vector>

using namespace OrderMatcher;

namespace {

struct Recorder : EventListener {
    std::vector<Trade> trades;
    std::vector<OrderUpdate> updates;
    void onTrade(const Trade& t) override { trades.push_back(t); }
    void onOrderUpdate(const OrderUpdate& u) override { updates.push_back(u); }
    const OrderUpdate* last(OrderId id) const {
        for (auto it = updates.rbegin(); it != updates.rend(); ++it)
            if (it->orderId == id) return &*it;
        return nullptr;
    }
    Quantity filled(OrderId id) const {
        Quantity q = 0;
        for (const Trade& t : trades)
            if (t.buyOrderId == id || t.sellOrderId == id) q += t.quantity;
        return q;
    }
};

AddOrderResult market(OrderBook& b, OrderId id, Side side, Quantity qty, Quantity minQty) {
    return b.addOrder(id, /*participantId=*/9, side, 0, qty, OrderType::Market,
                      /*stopPrice=*/0, /*displayQty=*/0, TimeInForce::GTC, /*expiryTime=*/0,
                      /*stopLimitPrice=*/0, PegType::None, /*pegOffset=*/0,
                      /*trailAmount=*/0, minQty);
}

void check(bool ok, const char* what, OrderBook& b, const Recorder& rec, OrderId id) {
    if (ok) return;
    const OrderUpdate* u = rec.last(id);
    std::fprintf(stderr, "FAIL %s: order %llu inBook=%d filled=%llu last status=%d "
                 "bids=%zu asks=%zu bestBid=%lld\n",
                 what, (unsigned long long)id, b.getOrder(id) != nullptr,
                 (unsigned long long)rec.filled(id), u ? (int)u->status : -1,
                 b.getBidLevelsCount(), b.getAskLevelsCount(), (long long)b.getBestBid());
    assert(false && "Market+minQty must behave as IOC+minQty");
}

// h3 U1: ask 10 @ 5000. Market buy 20 minQty 5; limit buy 10 @ 4000; market
// sell 25. Nothing may trade at 0.
void auditSequence() {
    OrderBook book(1);
    book.setCircuitBreakerThreshold(1e9);
    Recorder rec;
    book.setEventListener(&rec);
    book.addOrder(1, 1, Side::Sell, 5000, 10, OrderType::Limit);

    const auto r = market(book, 3, Side::Buy, 20, 5);
    check(std::holds_alternative<OrderId>(r), "market buy minQty 5 admitted", book, rec, 3);
    check(rec.filled(3) == 10 && book.getOrder(3) == nullptr && book.getBidLevelsCount() == 0,
          "met: trades the 10 offered, remainder cancelled, nothing rests", book, rec, 3);
    const OrderUpdate* u = rec.last(3);
    check(u && u->status == OrderStatus::Cancelled && u->filledQty == 10, "met: final Cancelled",
          book, rec, 3);

    book.addOrder(4, 4, Side::Buy, 4000, 10, OrderType::Limit);
    assert(std::holds_alternative<OrderId>(market(book, 5, Side::Sell, 25, 5)));
    for (const Trade& t : rec.trades) {
        if (t.price <= 0) {
            std::fprintf(stderr, "TRADE buy=%llu sell=%llu px=%lld q=%llu\n",
                         (unsigned long long)t.buyOrderId, (unsigned long long)t.sellOrderId,
                         (long long)t.price, (unsigned long long)t.quantity);
            assert(false && "a trade printed at price 0");
        }
    }
    check(rec.filled(5) == 10 && book.getOrder(5) == nullptr && book.getAskLevelsCount() == 0,
          "sell: trades 10 @ 4000, remainder cancelled", book, rec, 5);
    std::puts("  audit h3 U1 sequence: no rest, no print at 0");
}

void minimumNotMetIsCancelledUntraded(Side side) {
    OrderBook book(1);
    book.setCircuitBreakerThreshold(1e9);
    Recorder rec;
    book.setEventListener(&rec);
    const Side resting = side == Side::Buy ? Side::Sell : Side::Buy;
    book.addOrder(1, 1, resting, 5000, 4, OrderType::Limit);
    book.addOrder(2, 1, resting, side == Side::Buy ? 6000 : 4000, 4, OrderType::Limit);

    // 8 available across two levels: a minimum of 8 is met, 9 is not.
    assert(std::holds_alternative<OrderId>(market(book, 3, side, 20, 9)));
    const OrderUpdate* u = rec.last(3);
    check(rec.filled(3) == 0 && book.getOrder(3) == nullptr && u &&
              u->status == OrderStatus::Cancelled,
          "not met: cancelled untraded", book, rec, 3);
    check(book.getBidLevelsCount() + book.getAskLevelsCount() == 2,
          "not met: the book is untouched", book, rec, 3);

    assert(std::holds_alternative<OrderId>(market(book, 4, side, 20, 8)));
    check(rec.filled(4) == 8 && book.getOrder(4) == nullptr,
          "met across two levels: sweeps both", book, rec, 4);
    std::printf("  %s: minimum counts every level; unmet is cancelled untraded\n",
                side == Side::Buy ? "buy" : "sell");
}

// With market protection on, match() stops at the collar (touch +/- pct), so
// the minimum must be judged on what lies inside it. Ask 4 @ 5000 and 4 @ 6000,
// collar 10%: a market buy reaches 5000..5500 only, 4 shares.
void minimumCountsOnlyInsideTheCollar() {
    OrderBook book(1);
    book.setCircuitBreakerThreshold(1e9);
    book.setMarketProtectionPct(0.10);
    Recorder rec;
    book.setEventListener(&rec);
    book.addOrder(1, 1, Side::Sell, 5000, 4, OrderType::Limit);
    book.addOrder(2, 1, Side::Sell, 6000, 4, OrderType::Limit);

    assert(std::holds_alternative<OrderId>(market(book, 3, Side::Buy, 20, 8)));
    check(rec.filled(3) == 0 && book.getOrder(3) == nullptr && book.getAskLevelsCount() == 2,
          "collar: a minimum of 8 with 4 inside the collar is cancelled untraded",
          book, rec, 3);
    std::puts("  market protection: the minimum counts only what the collar lets it reach");
}

}  // namespace

int main() {
    auditSequence();
    minimumNotMetIsCancelledUntraded(Side::Buy);
    minimumNotMetIsCancelledUntraded(Side::Sell);
    minimumCountsOnlyInsideTheCollar();
    std::puts("MarketMinQtyTest passed");
    return 0;
}
