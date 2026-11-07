// RiskLedgerTest — roadmap 1.9 I2, I3, I6: risk on the admission path.
//
// One reservation ledger per participant and side: admission reserves an
// order's full quantity, a fill turns it into position, and whatever leaves
// without trading (cancel, IOC/FOK remainder, STP cut, kill, expiry, DAY close,
// replace, modify) gives it back through one hook in OrderBook. Each case runs
// on both engine paths: sync matches on the caller's thread, async on a worker.

#include "MatchingEngine.h"
#include "OrderBook.h"
#include "TempPath.h"
#include "Types.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace OrderMatcher;

namespace {

constexpr SymbolId kSym = 1;
constexpr Price kPx = 1'000'000;
constexpr ParticipantId kA = 1;
constexpr ParticipantId kB = 2;

}  // namespace

// ─── STP-9: checkRiskLimits is side-aware ───────────────────────────────────

TEST(BookPositionLimit, SellReducingALongIsNotTreatedAsIncreasingIt) {
    OrderBook book(kSym);
    book.setCircuitBreakerThreshold(1e9);
    book.setRiskLimits(kA, RiskLimits{0, 0, /*maxPositionSize=*/100});

    // A is long 80.
    ASSERT_TRUE(std::holds_alternative<OrderId>(
        book.addOrder(1, kB, Side::Sell, kPx, 80, OrderType::Limit)));
    ASSERT_TRUE(std::holds_alternative<OrderId>(
        book.addOrder(2, kA, Side::Buy, kPx, 80, OrderType::Limit)));
    ASSERT_EQ(book.getParticipantStats(kA).netPosition, 80);

    // Selling 150 leaves it short 70, inside the limit. Read side-blind it was
    // 80 + 150 = 230.
    EXPECT_TRUE(std::holds_alternative<OrderId>(
        book.addOrder(3, kA, Side::Sell, kPx + 100, 150, OrderType::Limit)));
    // Selling 200 leaves it short 120.
    EXPECT_FALSE(std::holds_alternative<OrderId>(
        book.addOrder(4, kA, Side::Sell, kPx + 100, 200, OrderType::Limit)));
    // Buying 30 takes it to 110.
    EXPECT_FALSE(std::holds_alternative<OrderId>(
        book.addOrder(5, kA, Side::Buy, kPx - 100, 30, OrderType::Limit)));
}

// ─── Both engine paths ─────────────────────────────────────────────────────

namespace {

constexpr SymbolId kSym2 = 2;

// A book with the breaker out of the way, so a deliberately
// crossing order is admitted.
class RiskPath : public ::testing::TestWithParam<bool> {
protected:
    MatchingEngine engine;
    void SetUp() override {
        for (SymbolId s : {kSym, kSym2}) {
            engine.addSymbol(s);
            engine.getOrderBook(s)->setCircuitBreakerThreshold(1e9);
        }
        if (async()) engine.startAsync(2, 4096); else engine.start();
    }
    bool async() const { return GetParam(); }
    void drain() { if (async()) engine.waitForDrain(); }
    OrderBook& book(SymbolId s = kSym) { return *engine.getOrderBook(s); }

    SubmitResult add(OrderId id, ParticipantId pid, Side side, Price px, Quantity qty,
                     OrderType type = OrderType::Limit) {
        return engine.submitOrder(kSym, id, pid, side, px, qty, type);
    }
    Quantity leaves(OrderId id, SymbolId s = kSym) {
        const Order* o = book(s).getOrder(id);
        return o ? o->remainingQty : 0;
    }
    // The async path acks at the queue, so only the sync path can answer here;
    // both end with the book unchanged, which each case checks after a drain.
    void expectRefused(const SubmitResult& r, RejectReason reason) {
        if (!async()) EXPECT_EQ(r.rejectReason, reason);
    }
};

INSTANTIATE_TEST_SUITE_P(BothPaths, RiskPath, ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& i) {
                             return i.param ? "Async" : "Sync";
                         });

}  // namespace

// ─── THR-3: reservation at admission ────────────────────────────────────────

// Two working orders on opposite sides do not net against each other: either
// can fill alone. Net of working, a participant could stack buy 100 / sell 100
// without end against a limit of 100.
TEST_P(RiskPath, WorkingOrdersDoNotNetAcrossSides) {
    engine.setPositionLimit(kA, 100);
    ASSERT_TRUE(add(1, kA, Side::Buy, kPx - 100, 100).isAccepted());
    ASSERT_TRUE(add(2, kA, Side::Sell, kPx + 100, 100).isAccepted());
    EXPECT_EQ(add(3, kA, Side::Buy, kPx - 100, 1).rejectReason,
              RejectReason::PositionLimitExceeded);
    EXPECT_EQ(add(4, kA, Side::Sell, kPx + 100, 1).rejectReason,
              RejectReason::PositionLimitExceeded);
    drain();
    EXPECT_EQ(leaves(1), 100u);
    EXPECT_EQ(leaves(2), 100u);
    EXPECT_EQ(leaves(3), 0u);
    EXPECT_EQ(leaves(4), 0u);
}

// Concurrent admissions: every producer can pass a check against the same
// position before any of them books. A buys and B sells across two symbols on
// two workers, so reservations race fills too. A only ever buys, so its filled
// position never falls and the final figure is its peak.
TEST_P(RiskPath, ConcurrentAdmissionsNeverExceedALimit) {
    constexpr int64_t kLimit = 100;
    constexpr int kThreads = 8;
    constexpr int kOrdersPerThread = 600;
    engine.setPositionLimit(kA, kLimit);
    engine.setPositionLimit(kB, kLimit);

    std::atomic<OrderId> nextId{1};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            while (!go.load(std::memory_order_acquire)) {}
            for (int i = 0; i < kOrdersPerThread; ++i) {
                const OrderId id = nextId.fetch_add(1, std::memory_order_relaxed);
                const SymbolId sym = (i % 2) ? kSym : kSym2;
                const ParticipantId pid = (t % 2) ? kA : kB;
                const Side side = (t % 2) ? Side::Buy : Side::Sell;
                (void)engine.submitOrder(sym, id, pid, side, kPx, 1, OrderType::Limit);
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();
    drain();

    int64_t aFilled = 0, bFilled = 0;
    for (SymbolId s : {kSym, kSym2}) {
        aFilled += book(s).getParticipantStats(kA).netPosition;
        bFilled += book(s).getParticipantStats(kB).netPosition;
    }
    EXPECT_LE(aFilled, kLimit);
    EXPECT_GE(bFilled, -kLimit);
    EXPECT_LE(engine.getPosition(kA), kLimit);
    EXPECT_GE(engine.getPosition(kB), -kLimit);
    // No order may rest past the limit either.
    Quantity aResting = 0, bResting = 0;
    for (SymbolId s : {kSym, kSym2}) {
        book(s).forEachOrderLocked([&](const Order& o) {
            (o.participantId == kA ? aResting : bResting) += o.remainingQty;
        });
    }
    EXPECT_LE(aFilled + static_cast<int64_t>(aResting), kLimit);
    EXPECT_LE(static_cast<int64_t>(bResting) - bFilled, kLimit);
}

// ─── STP-10: every removal path releases ────────────────────────────────────

TEST_P(RiskPath, ExpiryReleases) {
    engine.setPositionLimit(kA, 100);
    ASSERT_TRUE(engine.submitOrder(kSym, 1, kA, Side::Buy, kPx, 80, OrderType::Limit, 0, 0,
                                   TimeInForce::GTD, /*expiry=*/1000).isAccepted());
    drain();
    ASSERT_EQ(engine.getPosition(kA), 80);
    if (async()) {
        engine.stopAsync();   // the sync expiry entry point is the one under test
    }
    engine.expireOrders(2000);
    EXPECT_EQ(leaves(1), 0u);
    EXPECT_EQ(engine.getPosition(kA), 0);
}

TEST_P(RiskPath, StpCutOfARestingOrderReleases) {
    engine.setPositionLimit(kA, 100);
    book().setSTPMode(kA, STPMode::CancelResting);
    ASSERT_TRUE(add(1, kA, Side::Sell, kPx, 50).isAccepted());
    ASSERT_TRUE(add(2, kA, Side::Buy, kPx, 30).isAccepted());
    drain();
    EXPECT_EQ(leaves(1), 0u);
    EXPECT_EQ(leaves(2), 30u);
    EXPECT_EQ(engine.getPosition(kA), 30);
}

TEST_P(RiskPath, StpDecreaseShrinksBothReservations) {
    engine.setPositionLimit(kA, 100);
    book().setSTPMode(kA, STPMode::DecreaseAndCancel);
    ASSERT_TRUE(add(1, kA, Side::Sell, kPx, 50).isAccepted());
    ASSERT_TRUE(add(2, kA, Side::Buy, kPx, 20).isAccepted());
    drain();
    EXPECT_EQ(leaves(1), 30u);
    EXPECT_EQ(leaves(2), 0u);
    EXPECT_EQ(engine.getPosition(kA), -30);
}

TEST_P(RiskPath, IocRemainderReleases) {
    engine.setPositionLimit(kA, 100);
    ASSERT_TRUE(add(1, kB, Side::Sell, kPx, 30).isAccepted());
    ASSERT_TRUE(add(2, kA, Side::Buy, kPx, 100, OrderType::IOC).isAccepted());
    drain();
    EXPECT_EQ(engine.getPosition(kA), 30);
    EXPECT_TRUE(add(3, kA, Side::Buy, kPx - 100, 70).isAccepted());
    drain();
    EXPECT_EQ(engine.getPosition(kA), 100);
}

TEST_P(RiskPath, RejectedFokRollsBack) {
    engine.setPositionLimit(kA, 100);
    ASSERT_TRUE(add(1, kB, Side::Sell, kPx, 30).isAccepted());
    (void)add(2, kA, Side::Buy, kPx, 100, OrderType::FOK);
    drain();
    EXPECT_EQ(engine.getPosition(kA), 0);
    EXPECT_TRUE(add(3, kA, Side::Buy, kPx - 100, 100).isAccepted());
}

// A parked order holds its reservation; electing it must not count it twice.
TEST_P(RiskPath, ElectedStopDoesNotCountTwice) {
    engine.setPositionLimit(kA, 100);
    ASSERT_TRUE(engine.submitOrder(kSym, 1, kA, Side::Buy, 0, 10, OrderType::Stop,
                                   /*stopPrice=*/kPx).isAccepted());
    drain();
    ASSERT_EQ(engine.getPosition(kA), 10);
    // Liquidity for the stop, then the print at kPx that elects it.
    ASSERT_TRUE(add(2, kB, Side::Sell, kPx, 15).isAccepted());
    ASSERT_TRUE(add(3, kB + 1, Side::Buy, kPx, 5).isAccepted());
    drain();
    EXPECT_EQ(leaves(1), 0u);
    EXPECT_EQ(engine.getPosition(kA), 10);
}

TEST_P(RiskPath, ElectedStopRemainderReleases) {
    engine.setPositionLimit(kA, 100);
    ASSERT_TRUE(engine.submitOrder(kSym, 1, kA, Side::Buy, 0, 50, OrderType::Stop,
                                   /*stopPrice=*/kPx).isAccepted());
    ASSERT_TRUE(add(2, kB, Side::Sell, kPx, 15).isAccepted());
    ASSERT_TRUE(add(3, kB + 1, Side::Buy, kPx, 5).isAccepted());
    drain();
    // 10 of the 15 were left for the stop; the rest of it cancels as a market order.
    EXPECT_EQ(leaves(1), 0u);
    EXPECT_EQ(engine.getPosition(kA), 10);
}

TEST_P(RiskPath, AuctionRemainderReleases) {
    engine.setPositionLimit(kA, 100);
    engine.setPositionLimit(kB, 100);
    ASSERT_EQ(engine.setTradingStateBatch({kSym}, TradingState::AuctionOpen), 1u);
    ASSERT_TRUE(add(1, kA, Side::Buy, 0, 40, OrderType::Market).isAccepted());
    ASSERT_TRUE(add(2, kB, Side::Sell, kPx, 15).isAccepted());
    drain();
    ASSERT_EQ(engine.getPosition(kA), 40);
    ASSERT_EQ(engine.setTradingStateBatch({kSym}, TradingState::Continuous), 1u);
    // 15 crossed; the parked market order's other 25 cancelled.
    EXPECT_EQ(engine.getPosition(kA), 15);
    EXPECT_EQ(engine.getPosition(kB), -15);
}

TEST_P(RiskPath, OnCloseOrderDroppedAtSessionEndReleases) {
    engine.setPositionLimit(kA, 100);
    ASSERT_TRUE(add(1, kA, Side::Buy, kPx, 40, OrderType::LOC).isAccepted());
    drain();
    ASSERT_EQ(engine.getPosition(kA), 40);
    ASSERT_EQ(engine.setTradingStateBatch({kSym}, TradingState::PostClose), 1u);
    EXPECT_EQ(leaves(1), 0u);
    EXPECT_EQ(engine.getPosition(kA), 0);
}

// OCO cancels the sibling through a path that never reported the exposure.
TEST(RiskLedgerOco, SiblingCancelReleases) {
    MatchingEngine engine;
    engine.addSymbol(kSym);
    engine.getOrderBook(kSym)->setCircuitBreakerThreshold(1e9);
    engine.setPositionLimit(kA, 100);
    engine.startAsync(1, 1024);
    engine.processOrder(kSym, 1, kA, Side::Sell, kPx + 100, 10, OrderType::Limit);
    engine.processOrder(kSym, 2, kA, Side::Sell, kPx, 10, OrderType::Limit);
    engine.waitForDrain();
    engine.registerOco(kSym, 1, 2);
    engine.processOrder(kSym, 3, kB, Side::Buy, kPx, 10, OrderType::Limit);
    engine.waitForDrain();
    EXPECT_EQ(engine.getOrderBook(kSym)->getOrder(1), nullptr);
    EXPECT_EQ(engine.getPosition(kA), -10);
    engine.stopAsync();
}

// ─── STP-10 property: ledger == live working quantity at every step ─────────

namespace {

// The ground truth: what the books hold right now, and what they say each
// participant has executed. Compared with the ledger after every operation.
void expectLedgerMatchesBooks(MatchingEngine& engine, const std::vector<ParticipantId>& pids,
                              const std::string& where) {
    std::map<std::pair<ParticipantId, Side>, int64_t> working;
    for (SymbolId s : {SymbolId{1}, SymbolId{2}}) {
        engine.getOrderBook(s)->forEachOrderLocked([&](const Order& o) {
            working[{o.participantId, o.side}] += static_cast<int64_t>(o.remainingQty);
        });
    }
    for (ParticipantId p : pids) {
        int64_t filled = 0;
        for (SymbolId s : {SymbolId{1}, SymbolId{2}})
            filled += engine.getOrderBook(s)->getParticipantStats(p).netPosition;
        ASSERT_EQ(engine.getWorkingQty(p, Side::Buy), (working[{p, Side::Buy}])) << "buys of " << p << " " << where;
        ASSERT_EQ(engine.getWorkingQty(p, Side::Sell), (working[{p, Side::Sell}])) << "sells of " << p << " " << where;
        ASSERT_EQ(engine.getFilledPosition(p), filled) << "filled of " << p << " " << where;
    }
}

// Random flow over every order type, every removal path and every session
// transition. Rejects, refused transitions and misses are all fine: only the
// ledger is under test.
class LedgerFlow {
public:
    LedgerFlow(MatchingEngine& engine, uint32_t seed, bool canExpire, bool pegs = true)
        : engine_(engine), rng_(seed), canExpire_(canExpire), pegs_(pegs) {}

    void step(std::string* what) {
        const int roll = pick(0, 99);
        if (roll < 50) { add(what); return; }
        if (roll < 65) { cancel(what); return; }
        if (roll < 85) { amend(what); return; }
        if (roll < 88) {
            const ParticipantId p = pid();
            *what = "kill " + std::to_string(p);
            engine_.killSwitch(p);
            return;
        }
        if (roll == 88) {
            *what = "kill all";
            engine_.setKillSwitch(true);
            engine_.setKillSwitch(false);
            return;
        }
        if (roll < 92) {
            if (!canExpire_) return;
            clock_ += 5;
            *what = "expire";
            engine_.expireOrders(clock_);
            return;
        }
        if (roll < 96) { oco(what); return; }
        session(what);
    }

private:
    int pick(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng_); }
    ParticipantId pid() { return static_cast<ParticipantId>(pick(1, 3)); }
    SymbolId sym() { return static_cast<SymbolId>(pick(1, 2)); }
    Price px() { return kPx + 100 * pick(-4, 4); }
    Quantity qty() { return static_cast<Quantity>(pick(1, 40)); }
    OrderId known() {
        return nextId_ == 1 ? 1 : static_cast<OrderId>(pick(std::max<int>(1, static_cast<int>(nextId_) - 60),
                                                            static_cast<int>(nextId_) - 1));
    }

    void add(std::string* what) {
        const OrderId id = nextId_++;
        const SymbolId s = sym();
        const ParticipantId p = pid();
        const Side side = pick(0, 1) ? Side::Buy : Side::Sell;
        Price price = px();
        const Quantity q = qty();
        OrderType type = OrderType::Limit;
        Price stop = 0, stopLimit = 0, trail = 0, pegOffset = 0;
        Quantity display = 0, minQty = 0;
        PegType peg = PegType::None;
        TimeInForce tif = TimeInForce::GTC;
        uint64_t expiry = 0;
        switch (pick(0, 15)) {
        case 0: type = OrderType::IOC; break;
        case 1: type = OrderType::FOK; break;
        case 2: type = OrderType::Market; price = 0; break;
        case 3: type = OrderType::PostOnly; break;
        case 4: type = OrderType::Iceberg; display = std::max<Quantity>(1, q / 4); break;
        case 5: type = OrderType::Hidden; break;
        case 6: type = OrderType::Stop; price = 0; stop = px(); break;
        case 7: type = OrderType::StopLimit; price = 0; stop = px(); stopLimit = px(); break;
        case 8: type = OrderType::MIT; price = 0; stop = px(); break;
        case 9: type = OrderType::TrailingStop; trail = 200; break;
        case 10:
            if (!pegs_) break;
            type = OrderType::Pegged;
            peg = pick(0, 1) ? PegType::MidPeg : PegType::PrimaryPeg;
            break;
        case 11: type = OrderType::MOC; price = 0; break;
        case 12: type = OrderType::LOC; break;
        case 13: minQty = std::max<Quantity>(1, q / 2); if (pick(0, 1)) type = OrderType::IOC; break;
        case 14: tif = TimeInForce::GTD; expiry = clock_ + static_cast<uint64_t>(pick(1, 20)); break;
        default: tif = TimeInForce::DAY; break;
        }
        *what = "add " + std::to_string(id) + " type " + std::to_string(static_cast<int>(type));
        (void)engine_.submitOrder(s, id, p, side, price, q, type, stop, display, tif, expiry,
                                  stopLimit, peg, pegOffset, trail, minQty);
        symbolOf_[id] = s;
    }

    void cancel(std::string* what) {
        const OrderId id = known();
        *what = "cancel " + std::to_string(id);
        (void)engine_.submitCancel(symbolOf_[id], id);
    }

    void amend(std::string* what) {
        const OrderId id = known();
        if (pick(0, 1)) {
            *what = "modify " + std::to_string(id);
            (void)engine_.submitModify(symbolOf_[id], id, qty());
        } else {
            *what = "replace " + std::to_string(id);
            (void)engine_.submitCancelReplace(symbolOf_[id], id, px(), qty());
        }
    }

    void oco(std::string* what) {
        const OrderId a = known(), b = known();
        if (a == b || symbolOf_[a] != symbolOf_[b]) return;
        *what = "oco";
        engine_.registerOco(symbolOf_[a], a, b);
    }

    // Back towards continuous trading, one legal transition at a time, so the
    // flow spends most of its time where orders trade.
    void backToContinuous(SymbolId s) {
        for (int i = 0; i < 4; ++i) {
            OrderBook& book = *engine_.getOrderBook(s);
            switch (book.getTradingState()) {
            case TradingState::Continuous: return;
            case TradingState::PostClose:
                (void)engine_.beginTradingSession({s}, ++tradingDate_);
                break;
            case TradingState::AuctionClose:
                (void)engine_.setTradingStateBatch({s}, TradingState::PostClose);
                break;
            case TradingState::Halted:
                (void)engine_.setTradingStateBatch({s}, book.preHaltState());
                break;
            default:
                (void)engine_.setTradingStateBatch({s}, TradingState::Continuous);
            }
        }
    }

    void session(std::string* what) {
        const SymbolId s = sym();
        if (engine_.getOrderBook(s)->getTradingState() != TradingState::Continuous && pick(0, 1)) {
            *what = "back to continuous";
            backToContinuous(s);
            return;
        }
        static constexpr TradingState kStates[] = {
            TradingState::AuctionOpen, TradingState::AuctionClose, TradingState::Continuous,
            TradingState::VolatilityAuction, TradingState::Halted, TradingState::PostClose};
        switch (pick(0, 8)) {
        case 0:
            *what = "uncross";
            engine_.uncross(s);
            break;
        case 1:
            *what = "end session";
            (void)engine_.endTradingSession({s});
            break;
        case 2:
            *what = "begin session";
            (void)engine_.beginTradingSession({s}, ++tradingDate_);
            break;
        default: {
            const TradingState next = kStates[pick(0, 5)];
            *what = "state " + std::to_string(static_cast<int>(next));
            (void)engine_.setTradingStateBatch({s}, next);
            // Halted resumes only where it came from.
            if (next == TradingState::Halted && pick(0, 1))
                (void)engine_.setTradingStateBatch({s}, engine_.getOrderBook(s)->preHaltState());
        }
        }
    }

    MatchingEngine& engine_;
    std::mt19937 rng_;
    bool canExpire_;
    bool pegs_;
    OrderId nextId_ = 1;
    std::map<OrderId, SymbolId> symbolOf_;
    uint64_t clock_ = 1000;
    uint32_t tradingDate_ = 20000;
};

void configureLedgerEngine(MatchingEngine& engine, uint32_t seed) {
    engine.addSymbol(1);
    engine.addSymbol(2, MatchAlgorithm::ProRata);
    static constexpr STPMode kModes[] = {STPMode::DefaultCancelIncoming, STPMode::CancelResting,
                                         STPMode::CancelBoth, STPMode::DecreaseAndCancel};
    for (SymbolId s : {SymbolId{1}, SymbolId{2}}) {
        OrderBook& book = *engine.getOrderBook(s);
        book.setCircuitBreakerThreshold(1e9);
        for (ParticipantId p : {ParticipantId{1}, ParticipantId{2}, ParticipantId{3}})
            book.setSTPMode(p, kModes[(seed + p + s) % 4]);
    }
    // A tight limit, a wide one, and none: reservation refusals are in the flow.
    engine.setPositionLimit(1, 60);
    engine.setPositionLimit(2, 400);
    engine.setPositionLimit(3, 0);
}

constexpr int kFlowSeeds = 4;
constexpr int kFlowSteps = 1500;

}  // namespace

TEST(RiskLedgerProperty, SyncLedgerEqualsLiveWorkingQuantityAtEveryStep) {
    for (uint32_t seed = 1; seed <= kFlowSeeds; ++seed) {
        MatchingEngine engine;
        configureLedgerEngine(engine, seed);
        engine.start();
        LedgerFlow flow(engine, seed, /*canExpire=*/true);
        for (int i = 0; i < kFlowSteps; ++i) {
            std::string what = "(nothing)";
            flow.step(&what);
            expectLedgerMatchesBooks(engine, {1, 2, 3},
                                     "seed " + std::to_string(seed) + " step " + std::to_string(i) +
                                         " after " + what);
            if (::testing::Test::HasFatalFailure()) return;
        }
        // Not vacuous: the tight limit refused something and orders traded.
        EXPECT_GT(engine.getPositionRejectCount(), 0u) << "seed " << seed;
        uint64_t trades = 0;
        for (SymbolId s : {SymbolId{1}, SymbolId{2}})
            trades += engine.getOrderBook(s)->getParticipantStats(1).tradesExecuted;
        EXPECT_GT(trades, 0u) << "seed " << seed;
    }
}

TEST(RiskLedgerProperty, AsyncLedgerEqualsLiveWorkingQuantityAtEveryStep) {
    for (uint32_t seed = 1; seed <= kFlowSeeds; ++seed) {
        MatchingEngine engine;
        configureLedgerEngine(engine, seed);
        engine.startAsync(2, 4096);
        LedgerFlow flow(engine, seed, /*canExpire=*/true);
        for (int i = 0; i < kFlowSteps; ++i) {
            std::string what = "(nothing)";
            flow.step(&what);
            engine.waitForDrain();
            expectLedgerMatchesBooks(engine, {1, 2, 3},
                                     "seed " + std::to_string(seed) + " step " + std::to_string(i) +
                                         " after " + what);
            if (::testing::Test::HasFatalFailure()) return;
        }
        // Not vacuous: the tight limit refused something and orders traded.
        EXPECT_GT(engine.getPositionRejectCount(), 0u) << "seed " << seed;
        uint64_t trades = 0;
        for (SymbolId s : {SymbolId{1}, SymbolId{2}})
            trades += engine.getOrderBook(s)->getParticipantStats(1).tradesExecuted;
        EXPECT_GT(trades, 0u) << "seed " << seed;
    }
}

// Replay and a checkpoint both drive the books directly, so the ledger is
// rebuilt from them: each restored order reserved exactly once, and removing
// it gives back exactly that.
TEST(RiskLedgerProperty, RestartRebuildsTheLedgerAndCancellingEmptiesIt) {
    for (const bool checkpoint : {false, true}) {
        const std::string path = uniqueTempPath(std::string("risk_ledger_") +
                                                (checkpoint ? "ckpt" : "plain") + ".wal");
        std::remove(path.c_str());
        std::map<std::pair<ParticipantId, Side>, int64_t> before;
        {
            MatchingEngine engine;
            configureLedgerEngine(engine, 7);
            ASSERT_TRUE(engine.enableJournal(path, Journal::SyncPolicy::Immediate));
            engine.start();
            // No pegs: a checkpoint does not restore a resting peg yet (lane Y).
            LedgerFlow flow(engine, 7, /*canExpire=*/true, /*pegs=*/false);
            std::string what;
            for (int i = 0; i < 800; ++i) flow.step(&what);
            expectLedgerMatchesBooks(engine, {1, 2, 3}, "before restart");
            for (ParticipantId p : {ParticipantId{1}, ParticipantId{2}, ParticipantId{3}})
                for (Side side : {Side::Buy, Side::Sell})
                    before[{p, side}] = engine.getWorkingQty(p, side);
            if (checkpoint) engine.checkpoint();
            engine.stop();
        }
        MatchingEngine restarted;
        configureLedgerEngine(restarted, 7);
        ASSERT_TRUE(restarted.enableJournal(path, Journal::SyncPolicy::Immediate));
        restarted.start();
        restarted.replayJournal();
        expectLedgerMatchesBooks(restarted, {1, 2, 3}, "after restart");
        for (ParticipantId p : {ParticipantId{1}, ParticipantId{2}, ParticipantId{3}})
            for (Side side : {Side::Buy, Side::Sell})
                EXPECT_EQ(restarted.getWorkingQty(p, side), (before[{p, side}]))
                    << "participant " << p << (checkpoint ? " checkpoint" : " plain");

        for (ParticipantId p : {ParticipantId{1}, ParticipantId{2}, ParticipantId{3}})
            restarted.killSwitch(p);
        for (ParticipantId p : {ParticipantId{1}, ParticipantId{2}, ParticipantId{3}}) {
            EXPECT_EQ(restarted.getWorkingQty(p, Side::Buy), 0);
            EXPECT_EQ(restarted.getWorkingQty(p, Side::Sell), 0);
        }
        restarted.stop();
        std::remove(path.c_str());
    }
}

// ─── GW-2: replace and modify run the risk controls ─────────────────────────

TEST_P(RiskPath, ReplaceOverThePositionLimitIsRefused) {
    engine.setPositionLimit(kA, 100);
    ASSERT_TRUE(add(1, kA, Side::Buy, kPx, 60).isAccepted());
    expectRefused(engine.submitCancelReplace(kSym, 1, kPx, 150),
                  RejectReason::PositionLimitExceeded);
    drain();
    EXPECT_EQ(leaves(1), 60u);
    EXPECT_EQ(engine.getPosition(kA), 60);
    EXPECT_GE(engine.getPositionRejectCount(), 1u);
}

TEST_P(RiskPath, ReplaceAdjustsTheReservationByTheDelta) {
    engine.setPositionLimit(kA, 200);
    ASSERT_TRUE(add(1, kA, Side::Buy, kPx, 60).isAccepted());
    EXPECT_TRUE(engine.submitCancelReplace(kSym, 1, kPx, 150).isAccepted());
    drain();
    EXPECT_EQ(leaves(1), 150u);
    EXPECT_EQ(engine.getPosition(kA), 150);
    // Grown: the room left is 50.
    EXPECT_EQ(add(2, kA, Side::Buy, kPx - 100, 60).rejectReason,
              RejectReason::PositionLimitExceeded);
    // Shrunk: there is room again (once the worker has applied it).
    EXPECT_TRUE(engine.submitCancelReplace(kSym, 1, kPx, 80).isAccepted());
    drain();
    EXPECT_TRUE(add(3, kA, Side::Buy, kPx - 100, 60).isAccepted());
    drain();
    EXPECT_EQ(engine.getPosition(kA), 140);
}

TEST_P(RiskPath, ReplaceThatTradesDoesNotCountTwice) {
    engine.setPositionLimit(kA, 100);
    ASSERT_TRUE(add(1, kA, Side::Buy, kPx - 100, 10).isAccepted());
    ASSERT_TRUE(add(2, kB, Side::Sell, kPx, 4).isAccepted());
    EXPECT_TRUE(engine.submitCancelReplace(kSym, 1, kPx, 10).isAccepted());
    drain();
    EXPECT_EQ(leaves(1), 6u);
    EXPECT_EQ(engine.getPosition(kA), 10);
    EXPECT_EQ(engine.getPosition(kB), -4);
}

TEST_P(RiskPath, ReplaceIsHeldToTheFatFingerControls) {
    engine.setReferencePrice(kSym, kPx);
    engine.setFatFingerLimits(kSym, /*maxQty=*/100, /*devPct=*/0.10, /*maxNotional=*/0);
    ASSERT_TRUE(add(1, kA, Side::Buy, kPx, 60).isAccepted());
    expectRefused(engine.submitCancelReplace(kSym, 1, kPx, 150), RejectReason::FatFingerReject);
    expectRefused(engine.submitCancelReplace(kSym, 1, kPx * 2, 60), RejectReason::FatFingerReject);
    drain();
    ASSERT_EQ(leaves(1), 60u);
    EXPECT_EQ(book().getOrder(1)->price, kPx);
    EXPECT_GE(engine.getFatFingerRejectCount(), 2u);
}

TEST_P(RiskPath, ModifyReleasesTheDelta) {
    engine.setPositionLimit(kA, 100);
    ASSERT_TRUE(add(1, kA, Side::Buy, kPx, 100).isAccepted());
    EXPECT_TRUE(engine.submitModify(kSym, 1, 40).isAccepted());
    drain();
    EXPECT_EQ(leaves(1), 40u);
    EXPECT_EQ(engine.getPosition(kA), 40);
    EXPECT_TRUE(add(2, kA, Side::Buy, kPx - 100, 60).isAccepted());
}

// A modify only shrinks, so what it can break is a cap tightened since entry.
TEST_P(RiskPath, ModifyStillOverACapIsRefused) {
    ASSERT_TRUE(add(1, kA, Side::Buy, kPx, 100).isAccepted());
    drain();
    engine.setFatFingerLimits(kSym, /*maxQty=*/50, /*devPct=*/0.0, /*maxNotional=*/0);
    expectRefused(engine.submitModify(kSym, 1, 80), RejectReason::FatFingerReject);
    drain();
    EXPECT_EQ(leaves(1), 100u);
    EXPECT_TRUE(engine.submitModify(kSym, 1, 40).isAccepted());
    drain();
    EXPECT_EQ(leaves(1), 40u);
}

// ─── GW-4: the sync path runs the hierarchical gate too ─────────────────────

namespace {
constexpr uint64_t kFirm = 1000;
}  // namespace

TEST_P(RiskPath, HierarchicalOrderCapIsEnforced) {
    engine.mapParticipant(kA, 10, 100, kFirm);
    engine.setRiskTierLimits(RiskTier::Trader, kA, TierLimits{/*maxOrderSize=*/10, 0, 0, 0.0});
    expectRefused(add(1, kA, Side::Buy, kPx, 20), RejectReason::RiskLimitBreached);
    EXPECT_TRUE(add(2, kA, Side::Buy, kPx, 10).isAccepted());
    drain();
    EXPECT_EQ(leaves(1), 0u);
    EXPECT_EQ(leaves(2), 10u);
}

TEST_P(RiskPath, HierarchicalNetPositionAccruesFills) {
    engine.mapParticipant(kA, 10, 100, kFirm);
    engine.setRiskTierLimits(RiskTier::Firm, kFirm, TierLimits{0, 0, /*maxNetPosition=*/100, 0.0});
    ASSERT_TRUE(add(1, kB, Side::Sell, kPx, 60).isAccepted());
    ASSERT_TRUE(add(2, kA, Side::Buy, kPx, 60).isAccepted());
    drain();
    EXPECT_EQ(engine.riskNetPosition(RiskTier::Firm, kFirm), 60);
    expectRefused(add(3, kA, Side::Buy, kPx - 100, 60), RejectReason::RiskLimitBreached);
    drain();
    EXPECT_EQ(leaves(3), 0u);
}
