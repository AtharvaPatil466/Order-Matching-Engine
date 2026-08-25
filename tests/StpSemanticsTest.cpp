// StpSemanticsTest — C1. What OrderStatus::CancelledBySTP is allowed to mean.
//
// The C1 finding was not a crash but a misclassification: an order removed by
// self-trade prevention reported Filled at full quantity, so the OCO listener
// counted it as an execution, its group "won", and live sibling orders were
// cancelled — for an order that traded nothing. The defence against that
// returning is not a comment; it is these assertions.
//
// Step 1 pins the classification only. The matching-path behaviour that
// produces the status arrives in Step 2, the pro-rata and auction paths in
// Step 3, and the outbound protocol mapping in Step 4.

#include <gtest/gtest.h>

#include "AuditLog.h"
#include "Types.h"

using namespace OrderMatcher;

// ─── The core invariant: STP removal is not an execution ────────────────────

TEST(StpSemantics, CancelledBySTPIsNotAnExecution) {
    EXPECT_FALSE(isExecution(OrderStatus::CancelledBySTP))
        << "an STP-cancelled order traded nothing; counting it as an execution "
           "is exactly the C1 phantom fill";
    // The only two statuses that may ever count as a fill.
    EXPECT_TRUE(isExecution(OrderStatus::Filled));
    EXPECT_TRUE(isExecution(OrderStatus::PartiallyFilled));
    EXPECT_FALSE(isExecution(OrderStatus::Cancelled));
    EXPECT_FALSE(isExecution(OrderStatus::Rejected));
    EXPECT_FALSE(isExecution(OrderStatus::Accepted));
    EXPECT_FALSE(isExecution(OrderStatus::New));
}

// ─── Distinct from client-initiated cancellation ────────────────────────────

TEST(StpSemantics, CancelledBySTPIsDistinctFromCancelled) {
    EXPECT_NE(OrderStatus::CancelledBySTP, OrderStatus::Cancelled)
        << "the client did not ask for this cancellation; collapsing the two "
           "loses the only signal that STP fired";
    EXPECT_NE(OrderStatus::CancelledBySTP, OrderStatus::Filled);
    EXPECT_NE(OrderStatus::CancelledBySTP, OrderStatus::Rejected);
}

// ─── Terminal: the order is gone and will not update again ──────────────────

TEST(StpSemantics, CancelledBySTPIsTerminal) {
    EXPECT_TRUE(isTerminalStatus(OrderStatus::CancelledBySTP));
    // The audit trail must close the record out, not leave it open forever.
    AuditRecord r;
    r.finalStatus = OrderStatus::CancelledBySTP;
    EXPECT_TRUE(r.isTerminal())
        << "an STP-cancelled order never updates again; an audit record left "
           "non-terminal never closes";
}

TEST(StpSemantics, NonTerminalStatusesStayNonTerminal) {
    EXPECT_FALSE(isTerminalStatus(OrderStatus::New));
    EXPECT_FALSE(isTerminalStatus(OrderStatus::Accepted));
    // A partial fill is still working — the remainder rests.
    EXPECT_FALSE(isTerminalStatus(OrderStatus::PartiallyFilled));
}

// ─── The OCO gate is driven by isExecution, so STP cannot win a group ───────
//
// MatchingEngine::OcoBookListener::onOrderUpdate buffers an id only when
// isExecution(u.status). This asserts the predicate that gate depends on;
// the end-to-end "sibling is not cancelled" check lands in Step 2, once the
// matching path actually emits the status.

TEST(StpSemantics, OnlyExecutionsCanTriggerOco) {
    for (auto s : {OrderStatus::New, OrderStatus::Accepted, OrderStatus::Cancelled,
                   OrderStatus::CancelledBySTP, OrderStatus::Rejected}) {
        EXPECT_FALSE(isExecution(s))
            << "status " << static_cast<int>(s)
            << " must not be able to trigger an OCO group execution";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Step 2 — the price-time match() path actually emits the status.
// ═══════════════════════════════════════════════════════════════════════════

#include "MatchingEngine.h"
#include "OrderBook.h"

#include <vector>

namespace {

constexpr Price PX = 1'000'000;

class CaptureListener : public EventListener {
public:
    void onTrade(const Trade& t) override { trades.push_back(t); }
    void onOrderUpdate(const OrderUpdate& u) override { updates.push_back(u); }
    void onMarketData(const MarketDataUpdate&) override {}

    void clear() { trades.clear(); updates.clear(); }

    // Terminal (last) update seen for an order, or nullptr.
    const OrderUpdate* lastFor(OrderId id) const {
        const OrderUpdate* found = nullptr;
        for (const auto& u : updates) if (u.orderId == id) found = &u;
        return found;
    }
    int countStatus(OrderId id, OrderStatus s) const {
        int n = 0;
        for (const auto& u : updates) if (u.orderId == id && u.status == s) ++n;
        return n;
    }

    std::vector<Trade> trades;
    std::vector<OrderUpdate> updates;
};

}  // namespace

// ─── STPMode::None means NO prevention: a self-cross trades ─────────────────
//
// The mode's own contract says so (STPMode::None = "No self-trade prevention",
// Action::NoSelfTrade = "proceed normally", and isSelfTrade() returns false for
// it). Pre-fix the engine entered the STP path anyway, got NoSelfTrade back,
// and fell through a `default:` that killed the order while reporting a FULL
// FILL — so the default configuration silently contradicted its own header.
TEST(StpSemantics, ModeNoneSelfCrossTradesNormally) {
    OrderBook book(1);
    book.setCircuitBreakerThreshold(0.99);
    CaptureListener lis;
    book.setEventListener(&lis);

    const ParticipantId P = 1;
    ASSERT_EQ(book.getSTPMode(P), STPMode::None) << "default mode is the point";

    ASSERT_TRUE(std::holds_alternative<OrderId>(
        book.addOrder(1, P, Side::Sell, PX, 100, OrderType::Limit)));
    lis.clear();

    book.addOrder(2, P, Side::Buy, PX, 100, OrderType::Limit);

    // It really traded — a real trade, for the real quantity, at the real price.
    ASSERT_EQ(lis.trades.size(), 1u)
        << "no prevention is configured, so the self-cross must execute";
    EXPECT_EQ(lis.trades[0].quantity, 100u);
    EXPECT_EQ(lis.trades[0].price, PX);

    // Both sides are gone from the book, both reported Filled.
    EXPECT_EQ(book.getOrder(1), nullptr);
    EXPECT_EQ(book.getOrder(2), nullptr);
    EXPECT_EQ(lis.countStatus(2, OrderStatus::Filled), 1);
    EXPECT_EQ(lis.countStatus(1, OrderStatus::Filled), 1);

    // And nothing is mislabelled as an STP action, because STP never engaged.
    EXPECT_EQ(lis.countStatus(1, OrderStatus::CancelledBySTP), 0);
    EXPECT_EQ(lis.countStatus(2, OrderStatus::CancelledBySTP), 0);

    book.setEventListener(nullptr);
}

// ─── The C1 phantom fill, under a mode that actually prevents ───────────────
//
// With prevention configured the incoming order is genuinely killed. Pre-fix
// it was killed by zeroing remainingQty, which post-match read as "fully
// filled" and reported as Filled at filledQty == initialQty.
TEST(StpSemantics, CancelIncomingReportsCancelledBySTPNotFilled) {
    OrderBook book(1);
    book.setCircuitBreakerThreshold(0.99);
    CaptureListener lis;
    book.setEventListener(&lis);

    const ParticipantId P = 1;
    book.setSTPMode(P, STPMode::CancelIncoming);

    ASSERT_TRUE(std::holds_alternative<OrderId>(
        book.addOrder(1, P, Side::Sell, PX, 100, OrderType::Limit)));
    lis.clear();

    book.addOrder(2, P, Side::Buy, PX, 100, OrderType::Limit);

    // (a) No fill reported.
    EXPECT_TRUE(lis.trades.empty())
        << "STP blocked the cross; " << lis.trades.size() << " trade(s) emitted";
    EXPECT_EQ(lis.countStatus(2, OrderStatus::Filled), 0)
        << "this is the C1 phantom fill";
    EXPECT_EQ(lis.countStatus(2, OrderStatus::PartiallyFilled), 0);

    // (b) CancelledBySTP is what the client is told.
    const OrderUpdate* u = lis.lastFor(2);
    ASSERT_NE(u, nullptr);
    EXPECT_EQ(u->status, OrderStatus::CancelledBySTP);
    EXPECT_EQ(u->filledQty, 0u)
        << "pre-fix this reported filledQty == initialQty (100)";
    EXPECT_FALSE(isExecution(u->status));

    // (c) Distinct from a client-initiated cancel.
    EXPECT_EQ(lis.countStatus(2, OrderStatus::Cancelled), 0);

    // (d) Resting side untouched.
    const Order* resting = book.getOrder(1);
    ASSERT_NE(resting, nullptr);
    EXPECT_EQ(resting->remainingQty, 100u);

    book.setEventListener(nullptr);
}

// ─── No position accrual, and no OCO sibling cancellation ───────────────────
//
// The phantom fill's real damage: the OCO listener counts Filled as an
// execution, so the STP-cancelled leg "won" its group and cancelled the live
// sibling. Position accrual is trade-driven, so it must also stay at zero.
TEST(StpSemantics, StpCancelNeitherAccruesPositionNorWinsOco) {
    MatchingEngine engine;
    engine.addSymbol(1);
    engine.getOrderBook(1)->setCircuitBreakerThreshold(1e9);
    engine.getOrderBook(1)->setSTPMode(1, STPMode::CancelIncoming);
    engine.setPositionLimit(1, 10'000);   // arms position tracking
    engine.startAsync(1, 1024);

    const ParticipantId P = 1;

    engine.processOrder(1, /*id=*/10, P, Side::Sell, 105, 10, OrderType::Limit);
    engine.processOrder(1, /*id=*/11, P, Side::Sell,  95, 10, OrderType::Limit);
    engine.waitForDrain();
    engine.registerOco(1, 10, 11);

    const int64_t positionBefore = engine.getPosition(P);

    // P crosses its own resting sell at 95 with prevention configured.
    engine.processOrder(1, /*id=*/12, P, Side::Buy, 95, 10, OrderType::Limit);
    engine.waitForDrain();

    const OrderBook* book = engine.getOrderBook(1);

    EXPECT_NE(book->getOrder(10), nullptr)
        << "OCO sibling was cancelled by a phantom fill — this is C1's damage";
    EXPECT_NE(book->getOrder(11), nullptr)
        << "the self-crossed resting leg traded nothing and must survive";
    EXPECT_EQ(engine.getPosition(P), positionBefore)
        << "an STP-cancelled order must not accrue position";

    engine.stopAsync();
}

// ─── Configured STP modes report CancelledBySTP, not Cancelled ──────────────

TEST(StpSemantics, CancelRestingReportsCancelledBySTPForTheResting) {
    OrderBook book(1);
    book.setCircuitBreakerThreshold(0.99);
    CaptureListener lis;
    book.setEventListener(&lis);

    const ParticipantId P = 1;
    book.setSTPMode(P, STPMode::CancelResting);
    book.addOrder(1, P, Side::Sell, PX, 100, OrderType::Limit);
    lis.clear();
    book.addOrder(2, P, Side::Buy, PX, 100, OrderType::Limit);

    EXPECT_EQ(lis.countStatus(1, OrderStatus::CancelledBySTP), 1)
        << "the resting order was removed by STP, not by its owner";
    EXPECT_EQ(lis.countStatus(1, OrderStatus::Cancelled), 0);
    EXPECT_TRUE(lis.trades.empty());

    book.setEventListener(nullptr);
}

// A self-cross that follows REAL fills against other participants must still
// report those fills — STP only stopped the remainder.
TEST(StpSemantics, PartialFillThenSelfCrossStillReportsTheRealFill) {
    OrderBook book(1);
    book.setCircuitBreakerThreshold(0.99);
    CaptureListener lis;
    book.setEventListener(&lis);

    const ParticipantId P = 1, OTHER = 2;
    book.setSTPMode(P, STPMode::CancelIncoming);
    // Best ask is OTHER's 40 @ PX; behind it sits P's own 60 @ PX.
    book.addOrder(1, OTHER, Side::Sell, PX, 40, OrderType::Limit);
    book.addOrder(2, P,     Side::Sell, PX, 60, OrderType::Limit);
    lis.clear();

    // P buys 100: fills 40 against OTHER, then hits its own resting 60.
    book.addOrder(3, P, Side::Buy, PX, 100, OrderType::Limit);

    ASSERT_EQ(lis.trades.size(), 1u) << "the 40 against OTHER is a real trade";
    EXPECT_EQ(lis.trades[0].quantity, 40u);

    const OrderUpdate* u = lis.lastFor(3);
    ASSERT_NE(u, nullptr);
    EXPECT_EQ(u->filledQty, 40u)
        << "pre-fix remainingQty was zeroed, so this reported 100";
    EXPECT_TRUE(isExecution(u->status))
        << "40 shares genuinely traded, so this leg DID execute and must be "
           "able to win an OCO group";
    EXPECT_NE(u->status, OrderStatus::Filled) << "it did not fully fill";

    book.setEventListener(nullptr);
}
