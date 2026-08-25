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

#include <memory>
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

// ─── The C1 scenario, exactly as reported ───────────────────────────────────
//
// Pre-fix: SelfTradeProtection::check returns NoSelfTrade for STPMode::DefaultCancelIncoming,
// match()'s switch had no case for it, control fell to `default:` which zeroed
// the incoming order, and post-match saw remainingQty == 0 and reported Filled
// with filledQty == initialQty — a full fill for an order that traded nothing.
TEST(StpSemantics, ModeNoneSelfCrossReportsCancelledBySTPNotFilled) {
    OrderBook book(1);
    book.setCircuitBreakerThreshold(0.99);
    CaptureListener lis;
    book.setEventListener(&lis);

    const ParticipantId P = 1;
    // Deliberately NOT configured — the unconfigured default is the whole point.
    ASSERT_EQ(book.getSTPMode(P), STPMode::DefaultCancelIncoming);

    ASSERT_TRUE(std::holds_alternative<OrderId>(
        book.addOrder(1, P, Side::Sell, PX, 100, OrderType::Limit)));
    lis.clear();

    // P crosses its own resting sell.
    book.addOrder(2, P, Side::Buy, PX, 100, OrderType::Limit);

    // (a) No fill reported.
    EXPECT_TRUE(lis.trades.empty())
        << "an STP-blocked self-cross traded nothing; " << lis.trades.size()
        << " trade(s) were emitted";
    EXPECT_EQ(lis.countStatus(2, OrderStatus::Filled), 0)
        << "this is the C1 phantom fill";
    EXPECT_EQ(lis.countStatus(2, OrderStatus::PartiallyFilled), 0);

    // (b) CancelledBySTP is what the client is told.
    const OrderUpdate* u = lis.lastFor(2);
    ASSERT_NE(u, nullptr) << "the incoming order must get a terminal update";
    EXPECT_EQ(u->status, OrderStatus::CancelledBySTP);
    EXPECT_EQ(u->filledQty, 0u)
        << "pre-fix this reported filledQty == initialQty (100)";
    EXPECT_FALSE(isExecution(u->status));

    // (c) Distinct from a client-initiated cancel.
    EXPECT_EQ(lis.countStatus(2, OrderStatus::Cancelled), 0)
        << "the client did not cancel this order";

    book.setEventListener(nullptr);
}

// The resting side is untouched when STP kills the incoming order.
TEST(StpSemantics, ModeNoneSelfCrossLeavesRestingOrderIntact) {
    OrderBook book(1);
    book.setCircuitBreakerThreshold(0.99);
    const ParticipantId P = 1;

    book.addOrder(1, P, Side::Sell, PX, 100, OrderType::Limit);
    book.addOrder(2, P, Side::Buy, PX, 100, OrderType::Limit);

    const Order* resting = book.getOrder(1);
    ASSERT_NE(resting, nullptr) << "the resting order must survive";
    EXPECT_EQ(resting->remainingQty, 100u) << "it traded nothing";
    EXPECT_EQ(book.getOrder(2), nullptr) << "the incoming order was cancelled";
}

// ─── No position accrual, and no OCO sibling cancellation ───────────────────
//
// The phantom fill's real damage: the OCO listener counts Filled as an
// execution, so the STP-cancelled leg "won" its group and cancelled the live
// sibling. Position accrual is trade-driven, so it must also stay at zero.
TEST(StpSemantics, ModeNoneSelfCrossNeitherAccruesPositionNorWinsOco) {
    MatchingEngine engine;
    engine.addSymbol(1);
    engine.getOrderBook(1)->setCircuitBreakerThreshold(1e9);
    engine.setPositionLimit(1, 10'000);   // arms position tracking
    engine.startAsync(1, 1024);

    const ParticipantId P = 1;

    // P rests a sell at 105 and a sell at 95, linked One-Cancels-Other.
    engine.processOrder(1, /*id=*/10, P, Side::Sell, 105, 10, OrderType::Limit);
    engine.processOrder(1, /*id=*/11, P, Side::Sell,  95, 10, OrderType::Limit);
    engine.waitForDrain();
    engine.registerOco(1, 10, 11);

    const int64_t positionBefore = engine.getPosition(P);

    // P now crosses its own resting sell at 95 with no explicit STP mode configured.
    engine.processOrder(1, /*id=*/12, P, Side::Buy, 95, 10, OrderType::Limit);
    engine.waitForDrain();

    const OrderBook* book = engine.getOrderBook(1);

    // The OCO sibling must still be live: no leg executed, so no leg won.
    EXPECT_NE(book->getOrder(10), nullptr)
        << "OCO sibling was cancelled by a phantom fill — this is C1's damage";
    EXPECT_NE(book->getOrder(11), nullptr)
        << "the self-crossed resting leg traded nothing and must survive";

    // No position accrued: nothing traded.
    EXPECT_EQ(engine.getPosition(P), positionBefore)
        << "an STP-cancelled order must not accrue position";

    engine.stopAsync();
}

// ─── Configured STP modes also report CancelledBySTP, not Cancelled ─────────

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

TEST(StpSemantics, CancelIncomingReportsCancelledBySTPForTheIncoming) {
    OrderBook book(1);
    book.setCircuitBreakerThreshold(0.99);
    CaptureListener lis;
    book.setEventListener(&lis);

    const ParticipantId P = 1;
    book.setSTPMode(P, STPMode::CancelIncoming);
    book.addOrder(1, P, Side::Sell, PX, 100, OrderType::Limit);
    lis.clear();
    book.addOrder(2, P, Side::Buy, PX, 100, OrderType::Limit);

    const OrderUpdate* u = lis.lastFor(2);
    ASSERT_NE(u, nullptr);
    EXPECT_EQ(u->status, OrderStatus::CancelledBySTP);
    EXPECT_EQ(u->filledQty, 0u);
    EXPECT_NE(book.getOrder(1), nullptr) << "resting side untouched";

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
    EXPECT_EQ(u->filledQty, 40u) << "the executed quantity must be reported";
    EXPECT_TRUE(isExecution(u->status))
        << "40 shares genuinely traded, so this leg DID execute and must be "
           "able to win an OCO group";
    EXPECT_NE(u->status, OrderStatus::Filled) << "it did not fully fill";

    book.setEventListener(nullptr);
}

// ═══════════════════════════════════════════════════════════════════════════
// Step 3 — pro-rata and auction uncross honour stpModes_ too.
//
// Both paths previously ignored the mode matrix entirely: pro-rata always
// killed the incoming order (and zeroed remainingQty, reproducing the C1
// phantom fill), and the uncross always cancelled the BUYER regardless of
// configuration. The review additionally recorded that STP CancelIncoming and
// CancelBoth were executed by NO test in the suite; every mode is covered here
// on both paths.
// ═══════════════════════════════════════════════════════════════════════════

namespace {

// A pro-rata book with the volatility breaker relaxed.
std::unique_ptr<OrderBook> proRataBook() {
    auto b = std::make_unique<OrderBook>(1, MatchAlgorithm::ProRata);
    b->setCircuitBreakerThreshold(0.99);
    return b;
}

}  // namespace

// ─── Pro-rata: every mode ───────────────────────────────────────────────────

TEST(StpProRata, DefaultCancelIncomingKillsTakerAndReportsCancelledBySTP) {
    auto book = proRataBook();
    CaptureListener lis;
    book->setEventListener(&lis);
    const ParticipantId P = 1;

    book->addOrder(1, P, Side::Sell, PX, 100, OrderType::Limit);
    lis.clear();
    book->addOrder(2, P, Side::Buy, PX, 100, OrderType::Limit);

    EXPECT_TRUE(lis.trades.empty()) << "self-cross must not trade";
    const OrderUpdate* u = lis.lastFor(2);
    ASSERT_NE(u, nullptr);
    EXPECT_EQ(u->status, OrderStatus::CancelledBySTP);
    EXPECT_EQ(u->filledQty, 0u)
        << "pre-fix pro-rata zeroed remainingQty, reporting a full fill";
    ASSERT_NE(book->getOrder(1), nullptr);
    EXPECT_EQ(book->getOrder(1)->remainingQty, 100u) << "resting side untouched";

    book->setEventListener(nullptr);
}

// Never executed by any test before this one (review §Coverage).
TEST(StpProRata, CancelIncomingKillsTaker) {
    auto book = proRataBook();
    CaptureListener lis;
    book->setEventListener(&lis);
    const ParticipantId P = 1;
    book->setSTPMode(P, STPMode::CancelIncoming);

    book->addOrder(1, P, Side::Sell, PX, 100, OrderType::Limit);
    lis.clear();
    book->addOrder(2, P, Side::Buy, PX, 100, OrderType::Limit);

    EXPECT_TRUE(lis.trades.empty());
    ASSERT_NE(lis.lastFor(2), nullptr);
    EXPECT_EQ(lis.lastFor(2)->status, OrderStatus::CancelledBySTP);
    EXPECT_NE(book->getOrder(1), nullptr) << "resting side survives";

    book->setEventListener(nullptr);
}

TEST(StpProRata, CancelRestingRemovesMakerAndLetsTakerContinue) {
    auto book = proRataBook();
    CaptureListener lis;
    book->setEventListener(&lis);
    const ParticipantId P = 1, OTHER = 2;
    book->setSTPMode(P, STPMode::CancelResting);

    // P's own 60 and OTHER's 40 sit at the same level.
    book->addOrder(1, P,     Side::Sell, PX, 60, OrderType::Limit);
    book->addOrder(2, OTHER, Side::Sell, PX, 40, OrderType::Limit);
    lis.clear();

    book->addOrder(3, P, Side::Buy, PX, 100, OrderType::Limit);

    EXPECT_EQ(lis.countStatus(1, OrderStatus::CancelledBySTP), 1)
        << "P's own resting order is removed by STP";
    EXPECT_EQ(book->getOrder(1), nullptr);
    // Having removed its own order, the taker still trades against OTHER —
    // pre-fix the whole sweep was abandoned with `return`.
    ASSERT_FALSE(lis.trades.empty())
        << "the taker must still fill against the other participant";
    EXPECT_EQ(lis.trades[0].quantity, 40u);

    book->setEventListener(nullptr);
}

// Never executed by any test before this one (review §Coverage).
TEST(StpProRata, CancelBothRemovesTakerAndMaker) {
    auto book = proRataBook();
    CaptureListener lis;
    book->setEventListener(&lis);
    const ParticipantId P = 1;
    book->setSTPMode(P, STPMode::CancelBoth);

    book->addOrder(1, P, Side::Sell, PX, 100, OrderType::Limit);
    lis.clear();
    book->addOrder(2, P, Side::Buy, PX, 100, OrderType::Limit);

    EXPECT_TRUE(lis.trades.empty());
    EXPECT_EQ(lis.countStatus(1, OrderStatus::CancelledBySTP), 1) << "maker removed";
    ASSERT_NE(lis.lastFor(2), nullptr);
    EXPECT_EQ(lis.lastFor(2)->status, OrderStatus::CancelledBySTP) << "taker removed";
    EXPECT_EQ(book->getOrder(1), nullptr);
    EXPECT_EQ(book->getOrder(2), nullptr);

    book->setEventListener(nullptr);
}

// NOTE on DecreaseAndCancel, verified identical on BOTH paths and on the
// price-time reference: the resting order is decremented but the INCOMING order
// never is, so the sweep re-triggers on the same resting order and decrements
// it again until it is exhausted and cancelled. Net effect: DecreaseAndCancel
// is behaviourally identical to CancelResting whenever the incoming order
// survives the first pass — 100 decremented by 30 ends at 0, not 70.
//
// That is a pre-existing defect of the price-time path, not of this change; the
// requirement here is to mirror match(), and these tests pin that the mirror is
// faithful. If the mode is ever given its intended semantics (decrement BOTH
// sides by the match quantity, cancelling whichever hits zero) these two
// expectations are what will need revisiting.
TEST(StpProRata, DecreaseRestingConvergesToMakerCancellation) {
    auto book = proRataBook();
    CaptureListener lis;
    book->setEventListener(&lis);
    const ParticipantId P = 1;
    book->setSTPMode(P, STPMode::DecreaseAndCancel);

    book->addOrder(1, P, Side::Sell, PX, 100, OrderType::Limit);
    lis.clear();
    book->addOrder(2, P, Side::Buy, PX, 30, OrderType::Limit);

    EXPECT_TRUE(lis.trades.empty()) << "a decrease is not a trade";
    EXPECT_EQ(lis.countStatus(1, OrderStatus::CancelledBySTP), 1)
        << "repeated decrements exhaust the maker, which is then cancelled";
    EXPECT_EQ(book->getOrder(1), nullptr);
    // Identical to the price-time reference — that is the property under test.
    EXPECT_FALSE(lis.trades.size() > 0);

    book->setEventListener(nullptr);
}

// ─── Auction uncross: every mode ────────────────────────────────────────────
//
// "Incoming" maps to the LATER-arriving order by timestamp, not to the buyer.
// Each test rests the two sides in a known order so the mapping is observable.

namespace {

// Rests a self-crossing pair in the auction, sell FIRST then buy, so the BUY is
// the newer order. Returns the book with the uncross already run.
void runAuctionSelfCross(OrderBook& book, CaptureListener& lis,
                         ParticipantId P, OrderId sellId, OrderId buyId) {
    book.setCircuitBreakerThreshold(0.99);
    book.setTradingState(TradingState::AuctionOpen);
    book.addOrder(sellId, P, Side::Sell, PX, 100, OrderType::Limit);
    book.addOrder(buyId,  P, Side::Buy,  PX, 100, OrderType::Limit);
    lis.clear();
    book.uncross();
}

}  // namespace

TEST(StpUncross, DefaultCancelIncomingRemovesTheNewerOrderNotAlwaysTheBuyer) {
    OrderBook book(1);
    CaptureListener lis;
    book.setEventListener(&lis);
    // Sell rests first, buy second => the BUY is newer and must be the one cut.
    runAuctionSelfCross(book, lis, /*P=*/1, /*sellId=*/1, /*buyId=*/2);

    EXPECT_TRUE(lis.trades.empty()) << "self-cross must not print in the auction";
    EXPECT_EQ(lis.countStatus(2, OrderStatus::CancelledBySTP), 1) << "newer order cut";
    EXPECT_EQ(lis.countStatus(2, OrderStatus::Cancelled), 0)
        << "pre-fix this reported a client-initiated Cancelled";
    EXPECT_NE(book.getOrder(1), nullptr) << "older order survives";

    book.setEventListener(nullptr);
}

TEST(StpUncross, NewerIsTheSellWhenTheSellArrivesSecond) {
    OrderBook book(1);
    CaptureListener lis;
    book.setEventListener(&lis);
    book.setCircuitBreakerThreshold(0.99);
    book.setTradingState(TradingState::AuctionOpen);
    // Buy rests FIRST this time, so the SELL is newer. Pre-fix the buyer was
    // always the casualty regardless of arrival order.
    book.addOrder(1, 1, Side::Buy,  PX, 100, OrderType::Limit);
    book.addOrder(2, 1, Side::Sell, PX, 100, OrderType::Limit);
    lis.clear();
    book.uncross();

    EXPECT_TRUE(lis.trades.empty());
    EXPECT_EQ(lis.countStatus(2, OrderStatus::CancelledBySTP), 1)
        << "the newer order is the SELL here; the buyer must survive";
    EXPECT_NE(book.getOrder(1), nullptr) << "older buy survives";

    book.setEventListener(nullptr);
}

TEST(StpUncross, CancelRestingRemovesTheOlderOrder) {
    OrderBook book(1);
    CaptureListener lis;
    book.setEventListener(&lis);
    book.setSTPMode(1, STPMode::CancelResting);
    runAuctionSelfCross(book, lis, /*P=*/1, /*sellId=*/1, /*buyId=*/2);

    EXPECT_TRUE(lis.trades.empty());
    EXPECT_EQ(lis.countStatus(1, OrderStatus::CancelledBySTP), 1)
        << "CancelResting cuts the OLDER order (the sell, resting first)";
    EXPECT_NE(book.getOrder(2), nullptr) << "the newer order survives";

    book.setEventListener(nullptr);
}

// Never executed by any test before this one (review §Coverage).
TEST(StpUncross, CancelBothRemovesBothSides) {
    OrderBook book(1);
    CaptureListener lis;
    book.setEventListener(&lis);
    book.setSTPMode(1, STPMode::CancelBoth);
    runAuctionSelfCross(book, lis, /*P=*/1, /*sellId=*/1, /*buyId=*/2);

    EXPECT_TRUE(lis.trades.empty());
    EXPECT_EQ(lis.countStatus(1, OrderStatus::CancelledBySTP), 1);
    EXPECT_EQ(lis.countStatus(2, OrderStatus::CancelledBySTP), 1);
    EXPECT_EQ(book.getOrder(1), nullptr);
    EXPECT_EQ(book.getOrder(2), nullptr);

    book.setEventListener(nullptr);
}

TEST(StpUncross, DecreaseRestingConvergesToOlderOrderCancellation) {
    OrderBook book(1);
    CaptureListener lis;
    book.setEventListener(&lis);
    book.setCircuitBreakerThreshold(0.99);
    book.setSTPMode(1, STPMode::DecreaseAndCancel);
    book.setTradingState(TradingState::AuctionOpen);
    book.addOrder(1, 1, Side::Sell, PX, 100, OrderType::Limit);  // older
    book.addOrder(2, 1, Side::Buy,  PX,  30, OrderType::Limit);  // newer
    lis.clear();
    book.uncross();

    EXPECT_TRUE(lis.trades.empty()) << "a decrease is not a trade";
    EXPECT_EQ(lis.countStatus(1, OrderStatus::CancelledBySTP), 1)
        << "repeated decrements exhaust the older order — same as price-time";
    EXPECT_EQ(book.getOrder(1), nullptr);

    book.setEventListener(nullptr);
}

// A self-cross in the auction must not disturb an unrelated participant's
// matching — the same purity property ManualTest asserts for continuous trading.
TEST(StpUncross, OtherParticipantsStillCrossNormally) {
    OrderBook book(1);
    CaptureListener lis;
    book.setEventListener(&lis);
    book.setCircuitBreakerThreshold(0.99);
    book.setTradingState(TradingState::AuctionOpen);

    book.addOrder(1, /*P=*/1, Side::Sell, PX, 50, OrderType::Limit);
    book.addOrder(2, /*P=*/1, Side::Buy,  PX, 50, OrderType::Limit);   // self-cross
    book.addOrder(3, /*OTHER=*/2, Side::Sell, PX, 50, OrderType::Limit);
    book.addOrder(4, /*THIRD=*/3, Side::Buy,  PX, 50, OrderType::Limit);
    lis.clear();
    book.uncross();

    ASSERT_FALSE(lis.trades.empty())
        << "participants 2 and 3 must still cross despite 1's self-cross";
    for (const auto& t : lis.trades)
        EXPECT_NE(t.buyerId, t.sellerId) << "no self-trade may print";

    book.setEventListener(nullptr);
}
