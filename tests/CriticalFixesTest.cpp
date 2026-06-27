// CriticalFixesTest — regression tests for the Critical correctness bugs found
// in the repository audit (AUDIT_REPORT.md, section 1). Each test is written to
// FAIL against the pre-fix code and PASS once the corresponding fix lands.
//
//   Fix #1 — notional overflow bypasses the order-notional risk cap (OrderBook.cpp:124)
//   Fix #2 — MIT cancel leaves a dangling pointer in stopOrders_ (OrderBook.cpp:1035)
//   Fix #3 — TrailingStop stopPrice underflow when trailAmount > ref (OrderBook.cpp:521/1311)

#include <gtest/gtest.h>
#include "OrderBook.h"
#include "MatchingEngine.h"
#include "FlatHashMap.h"
#include "Types.h"

#include <atomic>
#include <thread>
#include <variant>

using namespace OrderMatcher;

namespace {

// Counts how many Cancelled OrderUpdates were emitted for a given order id.
class UpdateCountingListener : public EventListener {
public:
    void onTrade(const Trade&) override {}
    void onOrderUpdate(const OrderUpdate& u) override { updates.push_back(u); }
    void onMarketData(const MarketDataUpdate&) override {}

    int cancelledFor(OrderId id) const {
        int n = 0;
        for (const auto& u : updates)
            if (u.orderId == id && u.status == OrderStatus::Cancelled) ++n;
        return n;
    }
    std::vector<OrderUpdate> updates;
};

} // namespace

// ─── Fix #1: notional overflow must not bypass the risk limit ────────────────
//
// checkRiskLimits computed `price * (Price)qty / PRICE_PRECISION` in int64.
// For a large order the multiply overflows int64 (UB) and wraps negative, so
// the naive comparison `notional > maxOrderNotional` is false and the cap is
// silently bypassed. price=1e11 × qty=1e8 = 1e19 > INT64_MAX; the TRUE notional
// is 1e15, far above the 1e9 cap, so the order MUST be rejected.
TEST(CriticalFixes, NotionalOverflowDoesNotBypassRiskLimit) {
    OrderBook book(0);

    RiskLimits limits;
    limits.maxOrderSize = 0;                 // no size cap
    limits.maxOrderNotional = 1'000'000'000; // 1e9 notional cap
    limits.maxPositionSize = 0;              // no position cap
    book.setRiskLimits(/*participantId=*/7, limits);

    // IOC so the (buggy) accept path cannot rest an absurd price in the book.
    auto result = book.addOrder(/*id=*/1, /*pid=*/7, Side::Buy,
                                /*price=*/100'000'000'000LL,
                                /*qty=*/100'000'000ULL,
                                OrderType::IOC);

    ASSERT_TRUE(std::holds_alternative<RejectReason>(result))
        << "order with notional 1e15 must be rejected, not accepted";
    EXPECT_EQ(std::get<RejectReason>(result), RejectReason::RiskLimitBreached);
}

// ─── Fix #2: cancelling a parked MIT must remove it from stopOrders_ ─────────
//
// MIT orders park in stopOrders_, but cancelOrderImpl only removed Stop/
// StopLimit — leaving a dangling pointer that checkStopOrders() re-fires
// (phantom Cancelled + double-deallocate; aborts under the pool's -UNDEBUG
// double-free assert). The decoy is cancelled second so its slot sits on top
// of the LIFO free list, preserving the MIT's slot so the dangling fire is
// deterministic.
TEST(CriticalFixes, CancellingMitOrderDoesNotDanglingFire) {
    OrderBook book(0);
    UpdateCountingListener lis;
    book.setEventListener(&lis);
    // resting buy that the aggressor will cross to produce a trade
    // (also establishes the circuit-breaker reference price at 105)
    book.addOrder(1, 1, Side::Buy, 105, 10, OrderType::Limit);
    // MIT sell, triggers on upward touch of 105
    book.addOrder(2, 2, Side::Sell, 0, 30, OrderType::MIT, /*stopPrice=*/105);
    // decoy resting sell, harmless, near the market so it is admitted (won't cross)
    book.addOrder(99, 3, Side::Sell, 107, 1, OrderType::Limit);
    // cancel the MIT (real Cancelled #1 for id 2), then cancel the decoy so the
    // decoy's slot sits on TOP of the LIFO free list, preserving the MIT's slot
    book.cancelOrder(2);
    book.cancelOrder(99);
    ASSERT_EQ(book.getOrder(2), nullptr);
    // aggressor sell crosses resting buy @105 -> trade@105 -> checkStopOrders(105).
    // Fixed: MIT was removed from stopOrders_ at cancel -> no phantom.
    // Buggy: dangling MIT re-fires -> double free (abort under -UNDEBUG), or a
    // SECOND Cancelled for id 2 if assertions are off.
    book.addOrder(4, 4, Side::Sell, 105, 10, OrderType::Limit);
    EXPECT_EQ(lis.cancelledFor(2), 1) << "cancelled MIT must not re-fire from stopOrders_";
}

// ─── Fix #3: sell TrailingStop price must not go negative ────────────────────
//
// stopPrice = trailRefPrice - trailAmount with trailAmount > trailRefPrice
// produced a negative (nonsensical) trigger price; pathological inputs are
// signed-overflow UB. Floor the stop at 0 at both the init (addOrder) and
// ratchet (updateTrailingStops) sites. stopPrice is observable via getOrder().
TEST(CriticalFixes, SellTrailingStopPriceDoesNotUnderflow) {
    OrderBook book(0);
    // sell trailing stop, no prior trade -> trailRefPrice = price = 100,
    // trailAmount = 150 > 100. Buggy: stopPrice = 100-150 = -50. Fixed: 0.
    // addOrder positional args: id,pid,side,price,qty,type,stopPrice,displayQty,
    //   tif,expiryTime,stopLimitPrice,pegType,pegOffset,trailAmount,...
    book.addOrder(1, 1, Side::Sell, 100, 10, OrderType::TrailingStop,
                  /*stopPrice=*/0, /*displayQty=*/0, TimeInForce::GTC,
                  /*expiryTime=*/0, /*stopLimitPrice=*/0, PegType::None,
                  /*pegOffset=*/0, /*trailAmount=*/150);
    const Order* o = book.getOrder(1);
    ASSERT_NE(o, nullptr);
    EXPECT_GE(o->stopPrice, 0) << "trailing-stop price must not go negative";
}

// ─── Perf #1: per-fill onTrade dispatch skipped when no listener ─────────────
//
// The fill hot path now skips listener_->onTrade / engineListener_->onTrade
// entirely when no real listener is registered (hasTradeListener() == false),
// eliminating two no-op vtable dispatches per fill. These guards confirm the
// optimization is behaviour-preserving: trades are still delivered when a
// listener IS registered, and a cross with NO listener still fills correctly.

namespace {
class TradeCountingListener : public EventListener {
public:
    std::vector<Trade> trades;
    void onTrade(const Trade& t) override { trades.push_back(t); }
};
} // namespace

TEST(PerfFixes, TradesStillDeliveredToRegisteredListener) {
    OrderBook book(0);
    TradeCountingListener lis;
    EXPECT_FALSE(book.hasTradeListener());   // no listener yet -> dispatch skipped
    book.setEventListener(&lis);
    EXPECT_TRUE(book.hasTradeListener());    // listener registered -> dispatch active

    book.addOrder(1, 1, Side::Sell, 100, 10, OrderType::Limit);
    book.addOrder(2, 2, Side::Buy, 100, 10, OrderType::Limit);   // crosses -> 1 trade

    ASSERT_EQ(lis.trades.size(), 1u) << "trade must still be delivered to the listener";
    EXPECT_EQ(lis.trades[0].price, 100);
    EXPECT_EQ(lis.trades[0].quantity, 10u);
}

TEST(PerfFixes, CrossWithNoListenerStillFills) {
    OrderBook book(0);
    EXPECT_FALSE(book.hasTradeListener());    // skip path active
    book.addOrder(1, 1, Side::Sell, 100, 10, OrderType::Limit);
    auto r = book.addOrder(2, 2, Side::Buy, 100, 10, OrderType::Limit);
    ASSERT_TRUE(std::holds_alternative<OrderId>(r));
    // Both sides fully filled -> neither rests; the skipped dispatch dropped no work.
    EXPECT_EQ(book.getOrder(1), nullptr);
    EXPECT_EQ(book.getOrder(2), nullptr);
}

// ─── Perf #2: bookLock_ swapped shared_mutex -> mutex ───────────────────────
//
// getSnapshot()/computeAuctionState() were shared (concurrent) readers; under
// a plain mutex they become exclusive. This guard confirms the reader/writer
// path stays safe and DEADLOCK-FREE after the swap (the real risk: a reader
// that re-locks would self-deadlock on a non-recursive plain mutex). A writer
// thread hammers addOrder/cancel with bids strictly below asks while the main
// thread reads snapshots; a consistent (non-torn) snapshot is never crossed.
TEST(PerfFixes, ConcurrentSnapshotStaysConsistentUnderWriter) {
    OrderBook book(0);
    std::atomic<bool> writerDone{false};

    // Bounded writer: add a bid<100 and an ask>100, then cancel both next
    // iteration (lag 1) so the book holds only a couple of resting orders --
    // keeps depth small (fast, pool never exhausts) while still hammering
    // bookLock_ concurrently with the reader.
    std::thread writer([&] {
        uint64_t prevBid = 0, prevAsk = 0;
        for (uint64_t id = 1; id <= 6000; id += 2) {
            book.addOrder(id,     1, Side::Buy,  90 + (id % 9),  5, OrderType::Limit);
            book.addOrder(id + 1, 1, Side::Sell, 110 + (id % 9), 5, OrderType::Limit);
            if (prevBid) { book.cancelOrder(prevBid); book.cancelOrder(prevAsk); }
            prevBid = id; prevAsk = id + 1;
        }
        writerDone.store(true, std::memory_order_release);
    });

    while (!writerDone.load(std::memory_order_acquire)) {
        auto s = book.getSnapshot(10);
        if (s.bidCount > 0 && s.askCount > 0) {
            // A non-torn snapshot is never crossed (best bid < best ask).
            EXPECT_LT(s.bids[0].price, s.asks[0].price);
        }
    }

    writer.join();  // returns => no deadlock under the plain-mutex readers
}

// ─── Perf #3: orderLookup_ never rehashes (allocates) during matching ───────
//
// orderLookup_ is sized to the order pool capacity. With the 50% load factor
// that gives a rehash threshold well above the pool's max live orders, so it
// can never rehash mid-addOrder. disallowRehash() makes that invariant a hard,
// asserted contract; rehashCount() lets us prove no allocation occurred.
TEST(PerfFixes, FlatHashMapDoesNotRehashWhenPreSizedAndFrozen) {
    // Sized like orderLookup_(INITIAL_CAPACITY = 200000).
    FlatHashMap<uint64_t, uint64_t> map(200000);
    const size_t cap = map.capacity();
    map.disallowRehash();
    for (uint64_t i = 1; i <= 200000; ++i) map.insert(i, i * 2);
    EXPECT_EQ(map.rehashCount(), 0u) << "pre-sized map must not rehash up to pool capacity";
    EXPECT_EQ(map.capacity(), cap) << "capacity (allocation) must be unchanged";
    EXPECT_EQ(map.size(), 200000u);
}

TEST(PerfFixes, FlatHashMapStillRehashesWhenNotPreSized) {
    // Control: a small, non-frozen map DOES rehash as it grows -- proves the
    // counter is real and the guard above is not vacuous.
    FlatHashMap<uint64_t, uint64_t> map(8);
    for (uint64_t i = 1; i <= 1000; ++i) map.insert(i, i);
    EXPECT_GT(map.rehashCount(), 0u);
}

// ─── Perf #1: OCO drain reuses scratch buffers (no per-cycle realloc) ───────
//
// driveOco drains executed/trades via persistent reserved scratch buffers
// (swap + clear, retaining capacity) instead of swapping into a local that
// frees the buffer every cycle. Behaviour-preserving guard: OCO sibling-cancel
// must keep working across many cycles, which repeatedly exercise the reused
// buffers. (A pure allocation optimisation has no functional RED; the existing
// OCO suite plus this multi-cycle guard protect correctness.)
TEST(PerfFixes, OcoSiblingCancelWorksAcrossManyCycles) {
    MatchingEngine engine;
    engine.addSymbol(1);
    engine.getOrderBook(1)->setCircuitBreakerThreshold(1e9);
    engine.startAsync(1, 1024);

    OrderId id = 1;
    for (int cycle = 0; cycle < 8; ++cycle) {
        OrderId hi = id++, lo = id++, agg = id++;
        engine.processOrder(1, hi, 1, Side::Sell, 105, 10, OrderType::Limit);
        engine.processOrder(1, lo, 1, Side::Sell, 95,  10, OrderType::Limit);
        engine.waitForDrain();
        engine.registerOco(1, hi, lo);
        engine.processOrder(1, agg, 2, Side::Buy, 95, 10, OrderType::Limit);  // fills lo
        engine.waitForDrain();
        const OrderBook* book = engine.getOrderBook(1);
        ASSERT_EQ(book->getOrder(lo), nullptr) << "filled leg gone (cycle " << cycle << ")";
        ASSERT_EQ(book->getOrder(hi), nullptr) << "OCO sibling cancelled (cycle " << cycle << ")";
    }
    engine.stopAsync();
}

// ─── Audit §1: OrderBook correctness (STP iceberg, GTD=0, Pegged, FOK) ───────

namespace {
// Put the volatility circuit-breaker out of the way so deliberately-crossing
// test orders are admitted rather than rejected.
void relaxBook(OrderBook& book) { book.setCircuitBreakerThreshold(0.99); }
constexpr Price PX = 1'000'000;
}  // namespace

// §1(a) GTD/DAY with expiryTime==0 means "no expiry" — must not expire.
TEST(AuditFixes, GTD_ExpiryZero_DoesNotExpireImmediately) {
    OrderBook book(1); relaxBook(book);
    auto r = book.addOrder(1, 1, Side::Buy, PX, 100, OrderType::Limit, 0, 0,
                           TimeInForce::GTD, /*expiryTime=*/0);
    ASSERT_TRUE(std::holds_alternative<OrderId>(r));
    ASSERT_NE(book.getOrder(1), nullptr);
    book.expireOrders(std::numeric_limits<uint64_t>::max());
    EXPECT_NE(book.getOrder(1), nullptr)
        << "GTD order with expiryTime==0 (no expiry) must not be expired";
}

TEST(AuditFixes, GTD_RealExpiry_StillExpiresWhenClockPasses) {
    OrderBook book(1); relaxBook(book);
    const uint64_t expiry = 5'000;
    book.addOrder(2, 1, Side::Buy, PX, 100, OrderType::Limit, 0, 0, TimeInForce::GTD, expiry);
    ASSERT_NE(book.getOrder(2), nullptr);
    book.expireOrders(expiry - 1);
    EXPECT_NE(book.getOrder(2), nullptr) << "must survive before its expiry";
    book.expireOrders(expiry);
    EXPECT_EQ(book.getOrder(2), nullptr) << "must expire once the clock reaches expiry";
}

// §1(d) Pegged addToBook() failure must not leave the order dangling in lookup.
TEST(AuditFixes, Pegged_AddToBookFailure_NotLeftSilentlyInBook) {
    OrderBook book(1); relaxBook(book);
    book.setMaxDepth(1);
    book.addOrder(10, 1, Side::Sell, PX + 10'000, 100, OrderType::Limit);
    ASSERT_NE(book.getOrder(10), nullptr);
    // Pegged sell at a second ask price; maxDepth==1 already used -> addToBook fails.
    book.addOrder(11, 2, Side::Sell, PX + 20'000, 50, OrderType::Pegged);
    EXPECT_EQ(book.getOrder(11), nullptr)
        << "Pegged order rejected by addToBook must not remain in the book";
}

// §1(a) STP DecreaseResting must keep iceberg visibleQty <= remainingQty
// (guard: the bad transient state must never escape to underflow a later fill).
TEST(AuditFixes, STP_DecreaseResting_Iceberg_NoUnderflow) {
    OrderBook book(1); relaxBook(book);
    const ParticipantId P1 = 1, P2 = 2;
    book.setSTPMode(P1, STPMode::DecreaseAndCancel);
    book.addOrder(100, P1, Side::Sell, PX, 100, OrderType::Iceberg, 0, /*displayQty=*/60);
    const Order* ice = book.getOrder(100);
    ASSERT_NE(ice, nullptr);
    ASSERT_EQ(ice->remainingQty, 100u);
    ASSERT_EQ(ice->visibleQty, 60u);
    book.addOrder(101, P1, Side::Buy, PX, 60, OrderType::Limit);   // self-cross -> STP
    const Order* after = book.getOrder(100);
    if (after) {
        EXPECT_LE(after->visibleQty, after->remainingQty);
        EXPECT_LE(after->remainingQty, after->initialQty);
    }
    book.addOrder(102, P2, Side::Buy, PX, 100, OrderType::Limit);  // consume the rest
    const Order* fin = book.getOrder(100);
    if (fin) {
        EXPECT_LE(fin->visibleQty, fin->remainingQty);
        EXPECT_LE(fin->remainingQty, fin->initialQty) << "remainingQty underflowed (UINT64 wrap)";
    }
}

// §1(a) FOK pre-check vs matcher consistency. Audit item refuted: match()
// refreshes iceberg slices within one call, so checkLiquidity counting
// remainingQty is correct; switching to visibleQty would reject a fillable FOK.
// This pins that the pre-check and matcher agree (FOK is never a partial fill).
TEST(AuditFixes, FOK_IcebergVisibleVsHidden_PrecheckMatchesFill) {
    OrderBook book(1); relaxBook(book);
    book.addOrder(200, 1, Side::Sell, PX, 100, OrderType::Iceberg, 0, /*displayQty=*/10);
    ASSERT_NE(book.getOrder(200), nullptr);
    auto r = book.addOrder(201, 2, Side::Buy, PX, 50, OrderType::FOK);
    const Order* ice = book.getOrder(200);
    ASSERT_NE(ice, nullptr);
    if (std::holds_alternative<RejectReason>(r)) {
        EXPECT_EQ(ice->remainingQty, 100u) << "rejected FOK must not touch the book";
    } else {
        EXPECT_EQ(ice->remainingQty, 50u) << "admitted FOK must fully fill 50";
    }
}
