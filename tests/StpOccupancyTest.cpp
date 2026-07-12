// StpOccupancyTest — parity check for the O(1) self-trade-protection occupancy
// counter (OrderBook::stpResting_) that replaced the former O(depth) pre-scan.
//
// The counter is maintained by stpNoteAdded/stpNoteRemoved at every book
// entry/exit. Correctness = the counter ALWAYS equals a ground-truth scan of
// bids_/asks_ for every participant. This test drives a diverse workload through
// EVERY resting-order lifecycle path — rest, partial fill, full fill, cancel,
// self-trade STP removal, GTD expiry, iceberg refresh (the reorder path that must
// NOT decrement), pro-rata batch fill, and auction uncross — and asserts parity
// after every mutating call. Any missed or wrong instrumentation site breaks
// parity and names the phase.

#include "OrderBook.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

using namespace OrderMatcher;

namespace {

// The core invariant: for every participant id, the O(1) counter must equal a
// fresh scan of the book. Checked after each operation.
void assertParity(const OrderBook& book, const char* phase) {
    constexpr ParticipantId kMaxPidChecked = 16;  // > any pid this test uses
    for (ParticipantId pid = 0; pid <= kMaxPidChecked; ++pid) {
        const uint32_t counter = book.stpRestingCount(pid);
        const uint32_t scan = book.debugScanRestingCount(pid);
        if (counter != scan) {
            std::fprintf(stderr,
                         "PARITY FAIL @%s pid=%llu counter=%u scan=%u\n", phase,
                         static_cast<unsigned long long>(pid), counter, scan);
        }
        assert(counter == scan &&
               "STP occupancy counter must equal ground-truth resting scan");
    }
}

// ── FIFO book: rest / partial / full fill / cancel / STP / GTD / iceberg ──────
void testFifoLifecyclesKeepParity() {
    std::printf("Running testFifoLifecyclesKeepParity...\n");
    OrderBook book(1, MatchAlgorithm::PriceTime);
    book.setTradingState(TradingState::Continuous);

    // Rest a buy; partial-fill it; fully fill it.
    book.addOrder(1, /*pid=*/1, Side::Buy, toPrice(100.0), 10, OrderType::Limit);
    assertParity(book, "rest-buy");
    assert(book.stpRestingCount(1) == 1);

    book.addOrder(2, /*pid=*/2, Side::Sell, toPrice(100.0), 4, OrderType::Limit);
    assertParity(book, "partial-fill");  // pid1 still resting (6 left), pid2 gone
    assert(book.stpRestingCount(1) == 1);

    book.addOrder(3, /*pid=*/2, Side::Sell, toPrice(100.0), 6, OrderType::Limit);
    assertParity(book, "full-fill");     // pid1's buy fully filled -> removed
    assert(book.stpRestingCount(1) == 0);

    // Cancel path.
    book.addOrder(4, /*pid=*/3, Side::Buy, toPrice(99.0), 5, OrderType::Limit);
    assertParity(book, "rest-for-cancel");
    assert(book.stpRestingCount(3) == 1);
    book.cancelOrder(4);
    assertParity(book, "after-cancel");
    assert(book.stpRestingCount(3) == 0);

    // Self-trade: STP removes the resting order on a self-cross.
    book.setSTPMode(4, STPMode::CancelResting);
    book.addOrder(5, /*pid=*/4, Side::Buy, toPrice(101.0), 5, OrderType::Limit);
    assertParity(book, "stp-rest");
    assert(book.stpRestingCount(4) == 1);
    book.addOrder(6, /*pid=*/4, Side::Sell, toPrice(101.0), 5, OrderType::Limit);
    assertParity(book, "stp-selfcross");  // outcome varies; parity must hold

    // GTD expiry.
    book.addOrder(7, /*pid=*/5, Side::Buy, toPrice(98.0), 5, OrderType::Limit,
                  /*stopPrice=*/0, /*displayQty=*/0, TimeInForce::GTD,
                  /*expiryTime=*/100);
    assertParity(book, "gtd-rest");
    assert(book.stpRestingCount(5) == 1);
    book.expireOrders(/*currentTime=*/200);
    assertParity(book, "after-expiry");
    assert(book.stpRestingCount(5) == 0);

    // Iceberg refresh: the visible slice fills, the order refreshes and stays
    // resting (a remove+reinsert REORDER, which must NOT decrement the counter).
    book.addOrder(8, /*pid=*/6, Side::Buy, toPrice(103.0), 10, OrderType::Iceberg,
                  /*stopPrice=*/0, /*displayQty=*/2);
    assertParity(book, "iceberg-rest");
    assert(book.stpRestingCount(6) == 1);
    book.addOrder(9, /*pid=*/7, Side::Sell, toPrice(103.0), 2, OrderType::Limit);
    assertParity(book, "iceberg-refresh");  // still resting after reorder
    assert(book.stpRestingCount(6) == 1 && "iceberg reorder must not drop occupancy");

    std::printf("testFifoLifecyclesKeepParity PASSED\n");
}

// ── Pro-rata book: batch fill of multiple resting orders at one level ─────────
void testProRataBatchFillKeepsParity() {
    std::printf("Running testProRataBatchFillKeepsParity...\n");
    OrderBook book(2, MatchAlgorithm::ProRata);
    book.setTradingState(TradingState::Continuous);

    book.addOrder(30, /*pid=*/8, Side::Buy, toPrice(100.0), 10, OrderType::Limit);
    book.addOrder(31, /*pid=*/9, Side::Buy, toPrice(100.0), 10, OrderType::Limit);
    assertParity(book, "prorata-rest");
    assert(book.stpRestingCount(8) == 1 && book.stpRestingCount(9) == 1);

    // One large sell crosses both resting buys at the level (batch removal path).
    book.addOrder(32, /*pid=*/1, Side::Sell, toPrice(100.0), 20, OrderType::Limit);
    assertParity(book, "prorata-batchfill");
    assert(book.stpRestingCount(8) == 0 && book.stpRestingCount(9) == 0);

    std::printf("testProRataBatchFillKeepsParity PASSED\n");
}

// ── Auction uncross: parked orders cross and leave the book ───────────────────
void testAuctionUncrossKeepsParity() {
    std::printf("Running testAuctionUncrossKeepsParity...\n");
    OrderBook book(3, MatchAlgorithm::PriceTime);
    book.setTradingState(TradingState::AuctionClose);

    book.addOrder(40, /*pid=*/10, Side::Buy, toPrice(100.0), 10, OrderType::Limit);
    book.addOrder(41, /*pid=*/11, Side::Sell, toPrice(100.0), 10, OrderType::Limit);
    assertParity(book, "auction-accumulate");  // resting during the auction

    book.uncross();
    assertParity(book, "after-uncross");        // crossed & removed

    std::printf("testAuctionUncrossKeepsParity PASSED\n");
}

}  // namespace

int main() {
    std::printf("\n=== STP Occupancy Parity Tests ===\n");
    testFifoLifecyclesKeepParity();
    testProRataBatchFillKeepsParity();
    testAuctionUncrossKeepsParity();
    std::printf("\nStpOccupancyTest passed\n");
    return 0;
}
