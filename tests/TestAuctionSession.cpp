// TestAuctionSession — auction price discovery, NOII-style indicative/imbalance,
// and the LULD volatility-auction lifecycle.
//
// What this proves:
//   1. computeAuctionState() reports the SAME clearing price uncross() then
//      executes at — the published indicative cannot diverge from the cross.
//   2. The tie-break cascade is venue-correct: when several prices tie on max
//      executable volume, the engine resolves by minimum imbalance, then by
//      proximity to the reference price — NOT the old "lowest price" behavior.
//   3. Imbalance qty/side are still reported for a one-sided (non-crossing) book.
//   4. A circuit-breaker breach enters TradingState::VolatilityAuction (not a
//      hard Halt); orders then accumulate without matching; resumeVolatility-
//      Auction() runs the reopening cross and returns the book to Continuous.

#include "OrderBook.h"
#include "Types.h"

#include <cassert>
#include <iostream>

using namespace OrderMatcher;

static int tests_passed = 0;
#define SECTION(name) std::cout << "\n  " << name << "..." << std::flush
#define PASS()        do { ++tests_passed; std::cout << " PASS" << std::endl; } while (0)

namespace {
// Add a plain limit order; tests assert on resulting book state, not the ack.
void addLimit(OrderBook& b, OrderId id, ParticipantId p, Side s, Price px, Quantity q) {
    b.addOrder(id, p, s, px, q, OrderType::Limit);
}
} // namespace

// 1. The indicative price equals the price the cross actually executes at.
void test_indicative_matches_uncross() {
    SECTION("computeAuctionState price == uncross executed price");
    OrderBook book(1);
    book.setCircuitBreakerThreshold(1e9);            // isolate auction logic
    book.setTradingState(TradingState::AuctionOpen);
    addLimit(book, 1, 1, Side::Buy,  102, 100);
    addLimit(book, 2, 2, Side::Sell, 100, 100);

    AuctionResult r = book.computeAuctionState();
    assert(r.hasCross);
    assert(r.pairedVolume == 100);
    const Price indicative = r.indicativePrice;

    book.uncross();
    assert(book.getTradeCount() > 0);
    assert(book.getSnapshot().lastTradePrice == indicative);
    PASS();
}

// 2. Tie-break cascade: max volume -> min imbalance -> reference proximity.
void test_tiebreak_cascade() {
    SECTION("tie-break resolves by imbalance then reference, not lowest price");
    OrderBook book(1);
    book.setCircuitBreakerThreshold(1e9);
    book.setTradingState(TradingState::AuctionOpen);
    // Volume plateau: prices 103 and 105 each clear 150 at zero imbalance.
    addLimit(book, 1, 1, Side::Buy,  110, 100);      // first order -> ref = 110
    addLimit(book, 2, 1, Side::Buy,  105, 50);
    addLimit(book, 3, 2, Side::Sell, 100, 100);
    addLimit(book, 4, 2, Side::Sell, 103, 50);

    // With reference 110 the tied {103,105} resolve to 105 (nearer the ref) —
    // and crucially NOT 103, which the old lowest-price rule would have picked.
    AuctionResult r = book.computeAuctionState();
    assert(r.hasCross);
    assert(r.pairedVolume == 150);
    assert(r.indicativePrice == 105);
    assert(r.indicativePrice != 103);

    // Drop the reference below the plateau: proximity now pulls the pick to 103.
    book.setReferencePrice(103);
    AuctionResult r2 = book.computeAuctionState();
    assert(r2.pairedVolume == 150);
    assert(r2.indicativePrice == 103);
    PASS();
}

// 3. One-sided book: no cross, but imbalance qty/side are still reported.
void test_one_sided_imbalance() {
    SECTION("one-sided book reports imbalance with no cross");
    OrderBook book(1);
    book.setCircuitBreakerThreshold(1e9);
    book.setTradingState(TradingState::AuctionOpen);
    addLimit(book, 1, 1, Side::Buy, 100, 100);
    addLimit(book, 2, 1, Side::Buy, 101, 50);

    AuctionResult r = book.computeAuctionState();
    assert(!r.hasCross);
    assert(r.indicativePrice == 0);
    assert(r.imbalanceQty == 150);
    assert(r.imbalanceSide == Side::Buy);
    PASS();
}

// 4. Volatility auction: breach -> auction (not halt) -> accumulate -> reopen.
void test_volatility_auction_lifecycle() {
    SECTION("circuit-breaker breach auctions and reopens via a cross");
    OrderBook book(1);
    book.setCircuitBreakerThreshold(0.05);           // 5% band
    book.setTradingState(TradingState::Continuous);

    // Establish reference at 100 (the first limit order sets referencePrice_).
    addLimit(book, 1, 1, Side::Buy, 100, 100);
    assert(book.getTradingState() == TradingState::Continuous);

    // A sell 100% away from reference trips the breaker -> volatility auction,
    // NOT a hard halt. The triggering order itself is rejected.
    addLimit(book, 2, 2, Side::Sell, 200, 100);
    assert(book.getTradingState() == TradingState::VolatilityAuction);

    // Orders now accumulate without continuous matching. This in-band sell
    // crosses the resting buy on paper but must NOT trade until the reopen.
    const uint64_t tradesBefore = book.getTradeCount();
    addLimit(book, 3, 3, Side::Sell, 100, 100);
    assert(book.getTradingState() == TradingState::VolatilityAuction);
    assert(book.getTradeCount() == tradesBefore);    // no continuous match

    // resumeVolatilityAuction runs the reopening cross and returns to continuous.
    const bool reopened = book.resumeVolatilityAuction();
    assert(reopened);
    assert(book.getTradingState() == TradingState::Continuous);
    assert(book.getTradeCount() > tradesBefore);     // the reopening cross printed
    assert(book.getSnapshot().lastTradePrice == 100);

    // Not auctioning anymore: a further call is a no-op.
    assert(!book.resumeVolatilityAuction());
    PASS();
}

// 5. Empty auction is a safe no-op.
void test_empty_auction_noop() {
    SECTION("empty-book uncross and indicative are safe no-ops");
    OrderBook book(1);
    book.setTradingState(TradingState::AuctionOpen);
    AuctionResult r = book.computeAuctionState();
    assert(!r.hasCross);
    assert(r.imbalanceQty == 0);
    book.uncross();                                  // must not crash or trade
    assert(book.getTradeCount() == 0);
    PASS();
}

int main() {
    std::cout << "Running auction/session tests..." << std::endl;
    test_indicative_matches_uncross();
    test_tiebreak_cascade();
    test_one_sided_imbalance();
    test_volatility_auction_lifecycle();
    test_empty_auction_noop();
    std::cout << "\nAll " << tests_passed << " auction test sections passed." << std::endl;
    return 0;
}
