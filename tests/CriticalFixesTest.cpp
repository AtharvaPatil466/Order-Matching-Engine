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
#include <limits>
#include <variant>

using namespace OrderMatcher;

namespace {

// Counts how many Cancelled OrderUpdates were emitted for a given order id.
class UpdateCountingListener : public EventListener {
public:
    void onTrade(const Trade&) override {}
    void onOrderUpdate(const OrderUpdate& u) override { updates.push_back(u); }
    void onMarketData(const MarketDataUpdate&) override {}

    // Counts terminal cancellations however they were caused. C1 split STP
    // removal out into its own status, so an STP sweep now reports
    // CancelledBySTP rather than Cancelled; both mean "removed, did not trade".
    int cancelledFor(OrderId id) const {
        int n = 0;
        for (const auto& u : updates)
            if (u.orderId == id && (u.status == OrderStatus::Cancelled ||
                                    u.status == OrderStatus::CancelledBySTP)) ++n;
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

// ─── C3: Iceberg with displayQty == 0 must be rejected, not livelock ─────────
//
// Pre-fix: the order rested with visibleQty == 0, so match() computed
// available == 0 -> fillQty == 0 and the refresh branch re-sliced
// min(remainingQty, 0) == 0 forever — `while (incoming->remainingQty > 0)`
// never terminated and the matching thread livelocked on one malformed order.
TEST(CriticalFixes, IcebergWithZeroDisplayQtyIsRejected) {
    OrderBook book(1); relaxBook(book);

    auto r = book.addOrder(/*id=*/1, /*pid=*/1, Side::Sell, PX, /*qty=*/100,
                           OrderType::Iceberg, /*stopPrice=*/0, /*displayQty=*/0);

    ASSERT_TRUE(std::holds_alternative<RejectReason>(r))
        << "iceberg with displayQty == 0 must be rejected at admission";
    EXPECT_EQ(std::get<RejectReason>(r), RejectReason::InvalidDisplayQty)
        << "must be its own reason, not the generic InvalidQuantity — the "
           "order quantity is fine, the display slice is not";
    EXPECT_EQ(book.getOrder(1), nullptr) << "rejected iceberg must not rest";

    // The livelock trigger: a crossing order against that iceberg. With the
    // reject in place there is nothing to cross, so this returns.
    auto buy = book.addOrder(/*id=*/2, /*pid=*/2, Side::Buy, PX, 50, OrderType::Limit);
    ASSERT_TRUE(std::holds_alternative<OrderId>(buy));
    EXPECT_EQ(book.getOrder(2)->remainingQty, 50u) << "no counterparty: must rest unfilled";
}

// displayQty > qty is nonsensical rather than fatal; it is clamped to the order
// size so the stored field keeps the displayQty <= initialQty invariant.
TEST(CriticalFixes, IcebergDisplayQtyClampedToOrderQty) {
    OrderBook book(1); relaxBook(book);

    ASSERT_TRUE(std::holds_alternative<OrderId>(
        book.addOrder(1, 1, Side::Sell, PX, /*qty=*/40, OrderType::Iceberg,
                      /*stopPrice=*/0, /*displayQty=*/500)));

    const Order* ice = book.getOrder(1);
    ASSERT_NE(ice, nullptr);
    EXPECT_EQ(ice->displayQty, 40u) << "displayQty must be clamped to the order size";
    EXPECT_EQ(ice->visibleQty, 40u);
    EXPECT_LE(ice->visibleQty, ice->remainingQty);
}

// ─── C2: touchedPrices must not overflow on a fill-free STP sweep ────────────
//
// Pre-fix: every level visited appended to a stack std::array<Price, 256>, but
// the only flush guard was `fillCount == kMaxFillsPerOrder`. STP=CancelResting
// removes the resting order and `continue`s without producing a fill, so
// fillCount stays 0 while touchedCount grows with the number of levels swept.
// >256 distinct price levels -> a write to touchedPrices[256], past the end,
// on the matching thread — reachable from public order semantics, no auth.
// Detected as a stack-buffer-overflow under the ASan lane; here the behavioural
// assertion is that all 257 levels are swept and the sweep terminates cleanly.
TEST(CriticalFixes, TouchedPricesDoesNotOverflowOnLongStpSweep) {
    // 300 > kMaxFillsPerOrder (256), so the sweep must flush and wrap the
    // touchedPrices buffer mid-sweep rather than run off the end of it.
    static constexpr int kLevels = 300;
    OrderBook book(1); relaxBook(book);

    const ParticipantId P = 1;
    book.setSTPMode(P, STPMode::CancelResting);

    UpdateCountingListener listener;
    book.setEventListener(&listener);

    // One share per level, 257 consecutive ticks, all owned by P.
    for (int i = 0; i < kLevels; ++i) {
        ASSERT_TRUE(std::holds_alternative<OrderId>(
            book.addOrder(/*id=*/100 + i, P, Side::Sell, PX + i, /*qty=*/1,
                          OrderType::Limit)))
            << "setup: sell level " << i << " must rest";
    }
    ASSERT_EQ(book.getAskLevelsCount(), static_cast<size_t>(kLevels));

    // One marketable buy from the same participant sweeps every level. Each hit
    // is an STP CancelResting: no fill, so the fillCount guard never fires.
    auto r = book.addOrder(/*id=*/9999, P, Side::Buy, PX + kLevels, /*qty=*/kLevels,
                           OrderType::Limit);
    ASSERT_TRUE(std::holds_alternative<OrderId>(r));

    EXPECT_EQ(book.getAskLevelsCount(), 0u) << "every resting level must be STP-cancelled";
    int cancelled = 0;
    for (int i = 0; i < kLevels; ++i)
        cancelled += listener.cancelledFor(100 + i);
    EXPECT_EQ(cancelled, kLevels) << "each resting order must be cancelled exactly once";

    const Order* buy = book.getOrder(9999);
    ASSERT_NE(buy, nullptr) << "the incoming buy filled nothing and must rest";
    EXPECT_EQ(buy->remainingQty, static_cast<Quantity>(kLevels));

    book.setEventListener(nullptr);
}

// ─── H2: cancelReplace must run the same admission checks as a new order ────
//
// It previously ran only the qty==0 / price<=0 sanity checks, so a replace
// bypassed the trading-state gate, risk limits (hence arbitrary quantity
// increases), the LULD price band, the circuit breaker, and the PostOnly
// would-cross test. A rejected replace must leave the original order resting
// and unmodified.
//
// (Pool pressure is NOT in this list: cancelReplace reuses the existing Order
// and never calls orderPool_.allocate(), so there is nothing to shed.)

namespace {
// A book with a resting buy owned by participant 1 at PX, breaker relaxed so
// each test can arm exactly the one control it is about.
OrderId seedReplaceTarget(OrderBook& book, Quantity qty = 100) {
    book.setCircuitBreakerThreshold(0.99);
    book.addOrder(1, 1, Side::Buy, PX, qty, OrderType::Limit);
    return 1;
}
}  // namespace

TEST(AuditFixes, CancelReplaceHonoursRiskLimits) {
    OrderBook book(1);
    seedReplaceTarget(book);

    RiskLimits limits;
    limits.maxOrderSize = 150;          // the replace below asks for 5000
    book.setRiskLimits(/*pid=*/1, limits);

    EXPECT_FALSE(book.cancelReplace(1, PX, /*newQty=*/5000))
        << "an arbitrary quantity increase must be bounded by maxOrderSize";
    const Order* o = book.getOrder(1);
    ASSERT_NE(o, nullptr) << "a rejected replace leaves the order resting";
    EXPECT_EQ(o->remainingQty, 100u) << "and unmodified";

    // Within the limit it still works.
    EXPECT_TRUE(book.cancelReplace(1, PX, /*newQty=*/120));
    EXPECT_EQ(book.getOrder(1)->remainingQty, 120u);
}

TEST(AuditFixes, CancelReplaceHonoursPriceBand) {
    OrderBook book(1);
    seedReplaceTarget(book);
    book.setPriceBandPct(0.02);          // +/- 2% around the reference

    EXPECT_FALSE(book.cancelReplace(1, /*newPrice=*/PX * 2, 100))
        << "a replace outside the LULD band must be refused";
    ASSERT_NE(book.getOrder(1), nullptr);
    EXPECT_EQ(book.getOrder(1)->price, PX) << "original price untouched";

    EXPECT_TRUE(book.cancelReplace(1, PX + PX / 100, 100)) << "in-band replace works";
}

TEST(AuditFixes, CancelReplaceHonoursCircuitBreaker) {
    OrderBook book(1);
    book.addOrder(1, 1, Side::Buy, PX, 100, OrderType::Limit);  // sets reference
    book.setCircuitBreakerThreshold(0.05);                      // 5%

    EXPECT_FALSE(book.cancelReplace(1, /*newPrice=*/PX + PX / 5, 100))
        << "a 20% reprice must trip the same breaker a new order would";
    ASSERT_NE(book.getOrder(1), nullptr);
    EXPECT_EQ(book.getOrder(1)->price, PX);
}

TEST(AuditFixes, CancelReplaceHonoursTradingState) {
    OrderBook book(1);
    seedReplaceTarget(book);

    book.setTradingState(TradingState::Halted);
    EXPECT_FALSE(book.cancelReplace(1, PX, 50))
        << "replace must be refused while halted, as a new order is";

    book.setTradingState(TradingState::PostClose);
    EXPECT_FALSE(book.cancelReplace(1, PX, 50))
        << "and after the close";

    book.setTradingState(TradingState::Continuous);
    EXPECT_TRUE(book.cancelReplace(1, PX, 50)) << "allowed again once trading";
}

// The sharpest of the five: a PostOnly order repriced across the spread used
// to MATCH AS AGGRESSOR — the exact inversion of what PostOnly means.
TEST(AuditFixes, CancelReplacePostOnlyThatWouldCrossIsRejectedNotMatched) {
    OrderBook book(1);
    book.setCircuitBreakerThreshold(0.99);

    UpdateCountingListener lis;
    book.setEventListener(&lis);

    // Resting ask from another participant at PX.
    book.addOrder(10, /*pid=*/2, Side::Sell, PX, 100, OrderType::Limit);
    // Our PostOnly buy rests safely below it.
    ASSERT_TRUE(std::holds_alternative<OrderId>(
        book.addOrder(11, /*pid=*/1, Side::Buy, PX - 1000, 100, OrderType::PostOnly)));
    lis.updates.clear();

    // Reprice it up onto the ask. This WOULD cross.
    EXPECT_FALSE(book.cancelReplace(11, PX, 100))
        << "a PostOnly replace that would cross must be rejected";

    // It neither traded nor moved.
    const Order* po = book.getOrder(11);
    ASSERT_NE(po, nullptr) << "the PostOnly order must still be resting";
    EXPECT_EQ(po->price, PX - 1000) << "at its original price";
    EXPECT_EQ(po->remainingQty, 100u) << "having traded nothing";

    const Order* ask = book.getOrder(10);
    ASSERT_NE(ask, nullptr) << "the resting ask must be untouched";
    EXPECT_EQ(ask->remainingQty, 100u)
        << "pre-fix the repriced PostOnly aggressed into it";

    book.setEventListener(nullptr);
}

// ─── H3: a crossing minQty order must never rest, locking the book ──────────
//
// checkMinQty counts liquidity at CROSSING prices only, so a false return
// still permits some crossable size — just less than minQty. Resting
// unconditionally parked the order at a price that crosses, leaving bid >= ask.
// The semantic is a minimum on the FIRST execution (minQty is checked once at
// admission and the stored field is never read again), so a non-crossing order
// may still rest and wait; a crossing one may not.

namespace {
// The book invariant under test: never locked (bid == ask) or crossed
// (bid > ask). Empty sides read as 0 / Price max, matching ManualTest's
// fuzz invariant.
void expectNotLockedOrCrossed(const OrderBook& book, const char* phase) {
    const Price bb = book.getBestBid();
    const Price ba = book.getBestAsk();
    if (bb == 0 || ba == std::numeric_limits<Price>::max()) return;  // a side is empty
    EXPECT_LT(bb, ba) << "book is " << (bb == ba ? "LOCKED" : "CROSSED")
                      << " after " << phase << " (bid=" << bb << " ask=" << ba << ")";
}
}  // namespace

TEST(AuditFixes, MinQtyCrossingAtSamePriceDoesNotLockTheBook) {
    OrderBook book(1); relaxBook(book);

    book.addOrder(1, 1, Side::Sell, PX, 30, OrderType::Limit);
    expectNotLockedOrCrossed(book, "setup");

    // minQty 50, but only 30 is available at a crossing price.
    book.addOrder(2, 2, Side::Buy, PX, 100, OrderType::Limit, 0, 0,
                  TimeInForce::GTC, 0, 0, PegType::None, 0, 0, /*minQty=*/50);

    expectNotLockedOrCrossed(book, "crossing minQty buy at the same price");
    EXPECT_EQ(book.getOrder(2), nullptr) << "it must be cancelled, not rested";
    ASSERT_NE(book.getOrder(1), nullptr) << "and nothing may have traded";
    EXPECT_EQ(book.getOrder(1)->remainingQty, 30u);
}

TEST(AuditFixes, MinQtyCrossingThroughDoesNotCrossTheBook) {
    OrderBook book(1); relaxBook(book);

    book.addOrder(1, 1, Side::Sell, PX, 30, OrderType::Limit);
    // Priced ABOVE the ask — resting this would leave bid > ask outright.
    book.addOrder(2, 2, Side::Buy, PX + 1000, 100, OrderType::Limit, 0, 0,
                  TimeInForce::GTC, 0, 0, PegType::None, 0, 0, /*minQty=*/50);

    expectNotLockedOrCrossed(book, "minQty buy priced through the ask");
    EXPECT_EQ(book.getOrder(2), nullptr);
}

// The useful case is preserved: a passive minQty order waits for liquidity.
TEST(AuditFixes, MinQtyNonCrossingStillRestsAndWaits) {
    OrderBook book(1); relaxBook(book);

    book.addOrder(1, 1, Side::Sell, PX, 30, OrderType::Limit);
    book.addOrder(2, 2, Side::Buy, PX - 1000, 100, OrderType::Limit, 0, 0,
                  TimeInForce::GTC, 0, 0, PegType::None, 0, 0, /*minQty=*/50);

    EXPECT_NE(book.getOrder(2), nullptr) << "a non-crossing minQty order rests";
    expectNotLockedOrCrossed(book, "non-crossing minQty buy");
}

// When the minimum IS satisfiable, the order matches as normal.
TEST(AuditFixes, MinQtySatisfiedStillMatches) {
    OrderBook book(1); relaxBook(book);

    book.addOrder(1, 1, Side::Sell, PX, 80, OrderType::Limit);
    book.addOrder(2, 2, Side::Buy, PX, 100, OrderType::Limit, 0, 0,
                  TimeInForce::GTC, 0, 0, PegType::None, 0, 0, /*minQty=*/50);

    EXPECT_EQ(book.getOrder(1), nullptr) << "80 available >= minQty 50, so it traded";
    ASSERT_NE(book.getOrder(2), nullptr) << "20 remains resting";
    EXPECT_EQ(book.getOrder(2)->remainingQty, 20u);
    expectNotLockedOrCrossed(book, "satisfiable minQty match");
}

// ─── H4: cancelLocOrders must sweep every unfilled LOC order ────────────────
//
// THESE ARE INVARIANT GUARDS, NOT REPRODUCERS. They pass against the pre-fix
// code too, and that is worth stating precisely, because the reported
// consequence — "LOC orders survive post-close uncross and leak as resting" —
// is NOT reachable as described. A randomised sweep of 3000 configurations
// (1-40 LOC orders, varying ask depth, marketable and non-marketable mixed)
// against the pre-fix code produced zero survivors.
//
// Why the old code worked, and why it should not have: it range-for'd over
// locActiveIds_ while cancelOrderImpl swap-erases from that same container.
// Range-for captures __end ONCE, at the original size_. erase_swap(i) moves
// the last element into slot i — skipping it, since the iterator has already
// passed — but never clears the vacated tail slot. So the loop's over-run past
// the shrunken size_ reads exactly the ids the swap skipped. The two errors are
// precise inverses for this access pattern, and the sweep completes by
// accident.
//
// It is correct only while it keeps reading logically-dead storage beyond
// size_. Clearing on erase, a bounds-checked index, a size_-respecting
// iterator, or a different container all break it silently. The fix removes
// that dependency; these tests pin the invariant so a future regression is
// caught whether or not the accident still holds.

TEST(AuditFixes, PostCloseCancelsEveryUnfilledLocOrder) {
    // Enough orders that the swap-erase skip is unmistakable; an odd count so
    // an off-by-one in the drain shows up too.
    constexpr int kLocOrders = 25;

    OrderBook book(1); relaxBook(book);
    book.setTradingState(TradingState::AuctionClose);

    // LOC buys priced far below any possible cross, so none of them can fill
    // and all must be cancelled by the post-close sweep.
    for (int i = 0; i < kLocOrders; ++i) {
        auto r = book.addOrder(static_cast<OrderId>(100 + i), /*pid=*/1, Side::Buy,
                               PX - 1000 - i, /*qty=*/10, OrderType::LOC);
        ASSERT_TRUE(std::holds_alternative<OrderId>(r))
            << "setup: LOC order " << i << " must be accepted";
    }

    // A crossing pair so the uncross actually runs its full path rather than
    // taking the no-cross early return.
    book.addOrder(1, 2, Side::Sell, PX, 50, OrderType::Limit);
    book.addOrder(2, 3, Side::Buy,  PX, 50, OrderType::Limit);

    book.uncross();

    int survivors = 0;
    for (int i = 0; i < kLocOrders; ++i)
        if (book.getOrder(static_cast<OrderId>(100 + i))) ++survivors;

    EXPECT_EQ(survivors, 0)
        << survivors << " of " << kLocOrders << " LOC orders survived the "
           "post-close sweep and are now orphaned — resting in the book with "
           "locActiveIds_ already cleared";
}

// The no-cross early return (uncross's `!res.hasCross` branch) reaches
// cancelLocOrders by a different path; it must sweep completely too.
TEST(AuditFixes, PostCloseCancelsEveryLocOrderWhenNothingCrosses) {
    constexpr int kLocOrders = 17;

    OrderBook book(1); relaxBook(book);
    book.setTradingState(TradingState::AuctionClose);

    for (int i = 0; i < kLocOrders; ++i) {
        ASSERT_TRUE(std::holds_alternative<OrderId>(
            book.addOrder(static_cast<OrderId>(200 + i), 1, Side::Buy,
                          PX - 1000 - i, 10, OrderType::LOC)));
    }
    // No opposing liquidity at all => discoverUncrossPrice reports no cross.
    book.uncross();

    int survivors = 0;
    for (int i = 0; i < kLocOrders; ++i)
        if (book.getOrder(static_cast<OrderId>(200 + i))) ++survivors;

    EXPECT_EQ(survivors, 0) << survivors << " LOC orders survived the "
                               "no-cross post-close path";
}

// The realistic case: SOME LOC orders fill at the uncross and some do not.
// A filled LOC is deallocated in the match path without ever calling
// cancelOrderImpl, so its id is left STALE in locActiveIds_. Stale ids do not
// erase (cancelOrderImpl early-returns on the orderLookup_ miss) while live
// ones do, which desynchronises the swap-erase from the captured __end and
// makes the skip real. A uniform all-cancel sweep happens to self-correct —
// the stale tail reads recover exactly the skipped ids — which is why this
// mixed case is the one that matters.
TEST(AuditFixes, PostCloseCancelsUnfilledLocOrdersWhenOthersFilled) {
    constexpr int kFilling = 8;    // marketable: these fill at the uncross
    constexpr int kResting = 17;   // far below: these must all be cancelled

    OrderBook book(1); relaxBook(book);
    book.setTradingState(TradingState::AuctionClose);

    // Ask liquidity for the filling LOCs to trade against.
    book.addOrder(1, 2, Side::Sell, PX, kFilling * 10, OrderType::Limit);

    // Marketable LOC buys, interleaved with non-marketable ones so the stale
    // and live entries are mixed through locActiveIds_ rather than segregated.
    for (int i = 0; i < kFilling + kResting; ++i) {
        const bool marketable = (i % 3 == 0) && (i / 3) < kFilling;
        const Price px = marketable ? PX + 1000 : PX - 5000 - i;
        ASSERT_TRUE(std::holds_alternative<OrderId>(
            book.addOrder(static_cast<OrderId>(300 + i), 1, Side::Buy, px, 10,
                          OrderType::LOC)));
    }

    book.uncross();

    // Every LOC that did not fill must be gone. Any survivor is orphaned:
    // still resting, with locActiveIds_ already cleared behind it.
    int survivors = 0;
    for (int i = 0; i < kFilling + kResting; ++i)
        if (book.getOrder(static_cast<OrderId>(300 + i))) ++survivors;

    EXPECT_EQ(survivors, 0)
        << survivors << " unfilled LOC orders survived the post-close sweep";
}

// ─── H15: a cancel is not a submission ──────────────────────────────────────
//
// cancelOrderImpl incremented ordersSubmitted on every cancel, so a
// quote-and-pull participant's order-to-trade ratio climbed with no order
// flow behind it — inflating the /otr admin endpoint and any throttle read
// off it. Submissions are counted once, at admission.
TEST(AuditFixes, CancelDoesNotInflateOrderToTradeRatio) {
    OrderBook book(1); relaxBook(book);
    const ParticipantId P = 42;

    for (OrderId id = 1; id <= 5; ++id)
        ASSERT_TRUE(std::holds_alternative<OrderId>(
            book.addOrder(id, P, Side::Buy, PX - 10'000, 10, OrderType::Limit)));
    EXPECT_EQ(book.getParticipantStats(P).ordersSubmitted, 5u);

    for (OrderId id = 1; id <= 5; ++id) book.cancelOrder(id);

    EXPECT_EQ(book.getParticipantStats(P).ordersSubmitted, 5u)
        << "cancelling 5 resting orders must not count as 5 more submissions";
}

// ─── H16: every notional cap is denominated in whole currency units ─────────
//
// price is fixed-point (PRICE_PRECISION ticks per unit) and qty is whole, so
// notional = price*qty/PRICE_PRECISION. OrderBook divided; the fat-finger and
// hierarchical checks did not, making those caps 10,000x too loose for the
// same configured number. All four now route through orderNotional().
TEST(AuditFixes, NotionalHelperIsWholeCurrencyUnitsAndDoesNotOverflow) {
    // $100.00 x 100 shares = $10,000.
    EXPECT_EQ(orderNotional(100 * PRICE_PRECISION, 100), __int128(10'000));
    // The overflow case from Fix #1: 1e11 x 1e8 = 1e19 (> INT64_MAX) -> 1e15.
    EXPECT_EQ(orderNotional(100'000'000'000LL, 100'000'000ULL), __int128(1'000'000'000'000'000LL));
}

TEST(AuditFixes, FatFingerNotionalCapAgreesWithOrderBookNotionalCap) {
    // One order, one number: $200 notional ($2.00 x 100). A $100 cap must
    // reject it on BOTH paths; pre-fix the fat-finger path compared 2,000,000
    // against 100 in mismatched units and rejected everything instead.
    const Price  px  = 2 * PRICE_PRECISION;
    const Quantity q = 100;
    ASSERT_EQ(orderNotional(px, q), __int128(200));

    OrderBook book(1); relaxBook(book);
    RiskLimits limits;
    limits.maxOrderNotional = 100;   // $100
    book.setRiskLimits(7, limits);
    auto over = book.addOrder(1, 7, Side::Buy, px, q, OrderType::IOC);
    EXPECT_TRUE(std::holds_alternative<RejectReason>(over)) << "$200 > $100 cap";

    // $50 notional ($0.50 x 100) is under the same cap and must be admitted.
    auto under = book.addOrder(2, 7, Side::Buy, PRICE_PRECISION / 2, q, OrderType::IOC);
    EXPECT_FALSE(std::holds_alternative<RejectReason>(under))
        << "$50 must pass a $100 cap";
}
