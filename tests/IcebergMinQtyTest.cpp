// IcebergMinQtyTest — an iceberg that rests through the minQty path must rest
// with a DISPLAYED SLICE, not with nothing on show.
//
// WHAT WAS WRONG. An order with minQty > 0 whose minimum cannot be met, and
// which does not cross, rests inside screenMinQty via a direct addToBook. The
// ordinary rest path, finalizeRemainingQty, slices an iceberg first:
//
//     order->visibleQty = std::min(order->remainingQty, order->displayQty);
//
// screenMinQty never did. Orders are allocated with visibleQty = 0, so an
// iceberg taking that route entered the book with ZERO displayed quantity while
// holding its full remainingQty. addToBook's own comment names the assumption
// that broke — "the iceberg slice is sized just before the call" — which held
// for ten of its eleven callers.
//
// This is the second route to the MATCH-1 livelock. The first, modify-to-zero,
// was closed in OrderBook::modifyOrder. Both reach the same state: an order
// linked into a level that contributes nothing to it. And, as with the first,
// the two match algorithms fail differently from the one cause, so this test
// runs both:
//
//   PriceTime   available = visibleQty = 0, so the engine publishes a TRADE OF
//               QUANTITY ZERO before the refresh branch re-slices the iceberg.
//               A zero-size print reaches the tape and both participants.
//
//   ProRata     the level's total is 0, so the loop takes
//                   if (totalLevelQty == 0) { opposite.eraseBest(); continue; }
//               and eraseBest declines to deactivate a level that still holds
//               orders. It is a no-op, `continue` recomputes the same zero, and
//               the matching thread spins forever — one symbol dead.
//
// The level also advertises nothing: one order, zero quantity, visible to every
// market-data consumer as a price level with no size.
//
// WHAT IS PINNED. The invariant, not a return code: the quantity the book
// advertises is the quantity an aggressor receives, and no zero-size execution
// is ever published. alarm() turns the pro-rata livelock into a failure instead
// of a hung suite.
//
// The route needs no crossing liquidity at all, which is what makes it
// reachable by one client on its own: a buy iceberg with minQty against an
// empty ask side fails the minQty screen (nothing crosses), does not cross, and
// rests.

#include "OrderBook.h"

#include <cassert>
#include <cstdio>
#include <unistd.h>
#include <variant>
#include <vector>

using namespace OrderMatcher;

namespace {

constexpr Price         kPrice   = 10000;
constexpr Quantity      kTotal   = 100;
constexpr Quantity      kDisplay = 10;
constexpr Quantity      kMinQty  = 50;
constexpr ParticipantId kMaker   = 7;
constexpr ParticipantId kTaker   = 8;

struct TradeRecorder : EventListener {
    std::vector<Trade> trades;
    void onTrade(const Trade& t) override { trades.push_back(t); }

    Quantity total() const {
        Quantity q = 0;
        for (const auto& t : trades) q += t.quantity;
        return q;
    }
    bool anyZeroQuantity() const {
        for (const auto& t : trades)
            if (t.quantity == 0) return true;
        return false;
    }
};

// Rest a buy iceberg through the minQty path: the ask side is empty, so the
// minimum cannot be met and nothing crosses.
void restIcebergThroughMinQty(OrderBook& book) {
    const auto rested = book.addOrder(1, kMaker, Side::Buy, kPrice, kTotal,
                                      OrderType::Iceberg, /*stopPrice=*/0,
                                      /*displayQty=*/kDisplay, TimeInForce::GTC,
                                      /*expiryTime=*/0, /*stopLimitPrice=*/0,
                                      PegType::None, /*pegOffset=*/0,
                                      /*trailAmount=*/0, /*minQty=*/kMinQty);
    assert(std::holds_alternative<OrderId>(rested) &&
           "an iceberg with an unmet minQty that does not cross must rest");

    // The level must advertise the displayed slice — not zero.
    const auto snap = book.getSnapshot();
    assert(snap.bidCount == 1 && "the iceberg is not resting");
    assert(snap.bids[0].orderCount == 1);
    assert(snap.bids[0].totalQuantity == kDisplay &&
           "the iceberg rested with NO displayed quantity — the minQty path "
           "skipped the slice, and the level advertises one order of size zero");
}

// Aggress for exactly the displayed slice and require exactly that back.
void aggressAndCheck(OrderBook& book, const TradeRecorder& tape, const char* algo) {
    const auto aggressor = book.addOrder(2, kTaker, Side::Sell, kPrice, kDisplay,
                                         OrderType::Limit);
    assert(std::holds_alternative<OrderId>(aggressor));

    assert(!tape.anyZeroQuantity() &&
           "a trade of quantity 0 was published — a zero-size print reaches the "
           "tape, market data and both participants as though it were an execution");
    assert(tape.total() == kDisplay &&
           "the aggressor did not receive the quantity the book displayed");

    std::printf("  %s: displayed %llu, delivered %llu, no zero-size print\n", algo,
                static_cast<unsigned long long>(kDisplay),
                static_cast<unsigned long long>(tape.total()));
}

void priceTime() {
    OrderBook book(1, MatchAlgorithm::PriceTime);
    TradeRecorder tape;
    book.setEventListener(&tape);
    restIcebergThroughMinQty(book);
    aggressAndCheck(book, tape, "price-time");
}

void proRata() {
    OrderBook book(1, MatchAlgorithm::ProRata);
    TradeRecorder tape;
    book.setEventListener(&tape);
    restIcebergThroughMinQty(book);
    // Before the fix this call never returned: a zero-total level that
    // eraseBest will not deactivate, recomputed forever.
    aggressAndCheck(book, tape, "pro-rata");
}

}  // namespace

int main() {
    // A livelock must fail, not hang.
    alarm(30);

    priceTime();
    proRata();

    std::puts("IcebergMinQtyTest passed");
    return 0;
}
