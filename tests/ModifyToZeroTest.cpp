// ModifyToZeroTest — a modify down to zero quantity must be REFUSED, not applied.
//
// WHAT WAS WRONG. OrderBook::modifyOrder is the one function every amendment
// route lands in: the 2-arg overload, the engine's sync path, the async worker,
// journal replay, the follower, the sharded book. It reduced in place whenever
// `newQty < order->remainingQty`, and 0 satisfies that. So a modify to zero set
// remainingQty = 0 and LEFT THE ORDER LINKED IN ITS PRICE LEVEL.
//
// cancelReplace, thirty lines further down the same file, has always had
//
//     if (newQty == 0) { reason = RejectReason::InvalidQuantity; return false; }
//
// which is what makes this an oversight rather than a design: the same engine
// refuses the same quantity on the neighbouring path.
//
// WHY A ZERO-QUANTITY RESTING ORDER IS NOT MERELY UNTIDY. It reaches the match
// loop, and the two match algorithms fail differently — which is why this test
// exercises BOTH. Either one alone would have looked like a cosmetic defect.
//
//   PriceTime: `available` is 0, so `fillQty` is 0, and the engine publishes a
//   TRADE OF QUANTITY ZERO — to the tape, to market data, to both participants
//   — then marks the order Filled and removes it. The aggressor that should
//   have traded 50 gets nothing and rests instead. Liquidity the book had
//   displayed evaporates on contact, and a zero-quantity print goes out over
//   the wire as though it were an execution.
//
//   ProRata: the level's total is 0, so the allocation path takes
//
//       if (totalLevelQty == 0) { opposite.eraseBest(); continue; }
//
//   and FlatPriceMap::eraseBest refuses to erase a level that still holds
//   orders ("Only erase a best level that the caller has already drained
//   empty") — correctly, since deactivating a populated level would orphan its
//   nodes. So eraseBest is a no-op, `continue` recomputes the same zero total,
//   and the matching thread spins forever. In a thread-per-symbol engine that
//   is one symbol permanently dead and its queue filling behind it. The
//   callee's defensive guard is what converts the caller's bad assumption from
//   memory corruption into a hang: better, but still a wedged venue.
//
// The alarm() below exists for that: a livelock must FAIL this test, not hang
// the suite until ctest's timeout reports something vague.
//
// WHAT IS PINNED, AND WHY IT IS NOT JUST THE REJECT CODE. Asserting
// "modifyOrder returns InvalidQuantity" would pass against an engine that
// returns the right code and still corrupts the book. So each case asserts the
// INVARIANT the defect broke — that the liquidity the book advertises is the
// liquidity an aggressor actually receives — and checks the reject code second.
// This codebase's recurring failure is a test blind in the same direction as
// the code it covers; a reject-code assertion here would have been exactly that.
//
// tests/RefMatcher.h:165 already refuses newQty == 0, so the differential
// fuzzer's corpus case for this defect stops diverging once the engine agrees;
// it is promoted from .known.flow to .flow in the same commit.

#include "MatchingEngine.h"
#include "OrderBook.h"

#include <cassert>
#include <cstdio>
#include <unistd.h>
#include <variant>
#include <vector>

using namespace OrderMatcher;

namespace {

constexpr Price         kPrice   = 10100;
constexpr Quantity      kResting = 50;
constexpr ParticipantId kMaker   = 7;
constexpr ParticipantId kTaker   = 8;

// Records every trade the book publishes, so a zero-quantity print is
// observable rather than merely absent from a total.
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

// Rest one sell order, then try to modify it to zero.
void restAndAttemptModifyToZero(OrderBook& book) {
    const auto rested = book.addOrder(1, kMaker, Side::Sell, kPrice, kResting, OrderType::Limit);
    assert(std::holds_alternative<OrderId>(rested) && "the resting sell was not accepted");

    RejectReason reason = RejectReason::None;
    const bool applied = book.modifyOrder(1, 0, reason, kMaker);

    assert(!applied &&
           "modify to zero was APPLIED — the order is now resting at zero quantity, "
           "which prints a zero-quantity trade under price-time and livelocks pro-rata");
    assert(reason == RejectReason::InvalidQuantity &&
           "a refused modify must say why; cancelReplace already answers "
           "InvalidQuantity for the same quantity");
}

// The invariant, under price-time: displayed liquidity is deliverable
// liquidity, and nothing publishes a zero-quantity execution.
void priceTimeKeepsTheLiquidityItAdvertises() {
    OrderBook book(1, MatchAlgorithm::PriceTime);
    TradeRecorder tape;
    book.setEventListener(&tape);

    restAndAttemptModifyToZero(book);

    // The level must still advertise what it holds.
    const auto snap = book.getSnapshot();
    assert(snap.askCount == 1 && "the resting order vanished from the book");
    assert(snap.asks[0].totalQuantity == kResting &&
           "the level no longer advertises the quantity it holds");
    assert(snap.asks[0].orderCount == 1);

    // And an aggressor must actually receive it.
    const auto aggressor =
        book.addOrder(2, kTaker, Side::Buy, kPrice, kResting, OrderType::Limit);
    assert(std::holds_alternative<OrderId>(aggressor));

    assert(!tape.anyZeroQuantity() &&
           "a trade of quantity 0 was published — that reaches the tape, market "
           "data and both participants as though it were an execution");
    assert(tape.trades.size() == 1 &&
           "expected exactly one fill against the one resting order");
    assert(tape.total() == kResting &&
           "the aggressor did not receive the quantity the book displayed");

    std::puts("  price-time: liquidity delivered in full, no zero-quantity print");
}

// The same invariant under pro-rata — the algorithm that livelocks on a
// zero-total level rather than printing zero.
void proRataDoesNotSpinOnAZeroTotalLevel() {
    OrderBook book(1, MatchAlgorithm::ProRata);
    TradeRecorder tape;
    book.setEventListener(&tape);

    restAndAttemptModifyToZero(book);

    // At HEAD this call never returns: totalLevelQty == 0, eraseBest() declines
    // to deactivate a populated level, and `continue` recomputes the same zero.
    const auto aggressor =
        book.addOrder(2, kTaker, Side::Buy, kPrice, kResting, OrderType::Limit);
    assert(std::holds_alternative<OrderId>(aggressor));

    assert(!tape.anyZeroQuantity() && "pro-rata published a zero-quantity trade");
    assert(tape.total() == kResting &&
           "the aggressor did not receive the quantity the book displayed");

    std::puts("  pro-rata: matched and returned — no spin on a zero-total level");
}

// The async engine path must refuse SYNCHRONOUSLY. Without a check in
// submitModify the client is told "accepted", the request is queued, the worker
// refuses it, and nothing ever comes back: an accept that silently means no.
void asyncSubmitRefusesBeforeQueueing() {
    MatchingEngine engine;
    engine.addSymbol(1);
    engine.startAsync(1, 1 << 12);

    const auto added =
        engine.submitOrder(1, 10, kMaker, Side::Sell, kPrice, kResting, OrderType::Limit);
    assert(added.isAccepted());
    engine.waitForDrain();

    const auto modified = engine.submitModify(1, 10, 0, kMaker);
    assert(!modified.isAccepted() &&
           "submitModify ACCEPTED a modify to zero — the client is told yes, the "
           "worker then refuses it, and no further message ever arrives");
    assert(modified.rejectReason == RejectReason::InvalidQuantity);

    engine.waitForDrain();
    engine.stopAsync();
    std::puts("  async: submitModify refused before the request was queued");
}

}  // namespace

int main() {
    // A livelock must fail, not hang. Generous enough that a loaded CI box
    // cannot trip it: every case here is a handful of operations.
    alarm(30);

    priceTimeKeepsTheLiquidityItAdvertises();
    proRataDoesNotSpinOnAZeroTotalLevel();
    asyncSubmitRefusesBeforeQueueing();

    std::puts("ModifyToZeroTest passed");
    return 0;
}
