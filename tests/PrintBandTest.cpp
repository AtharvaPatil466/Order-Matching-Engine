// PrintBandTest — one volatility check, asked of every continuous print.
//
// WHAT WAS WRONG. The breaker tested the first fill of an order arriving
// through addOrder and nothing else:
//   - MR-2: an elected stop called match() directly, so one in-band 1-lot
//     trigger cascaded 66 stop-markets through a bid ladder, 97 trades down to
//     6.70 against a reference of 10.00, and the book stayed Continuous.
//   - FORMAL-1: a crossing cancelReplace matched with no print check at all;
//     replacing a 4.00 bid to 9.60 traded at 5.00, 50% from the reference.
//   - MR-1: the reference was seeded from the first priced order seen, before
//     the FOK/PostOnly refusals, so a rejected 1-lot FOK at 0.0001 fixed it and
//     the first honest trade put the symbol into a five-minute auction.
//   - A sweep whose first fill was in band ran on through the band.
//
// WHAT IS PINNED. Every print in Continuous — a fresh order's, a replace's, an
// elected stop's, a latched election's after the reopen — is tested at its own
// price against the reference before it prints. A print outside the threshold
// does not happen: the book enters VolatilityAuction, and the order that would
// have printed it is treated as an auction arrival with what it has left. The
// reference comes from a print, an auction or setReferencePrice, never from an
// order. Each case also checks that in-band flow on the same path still trades.

#include "EventListener.h"
#include "MatchingEngine.h"
#include "OrderBook.h"
#include "TempPath.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <memory>
#include <variant>
#include <vector>

using namespace OrderMatcher;

namespace {

constexpr Price kRef = 100000;                 // 10.0000
constexpr Price kLo = kRef - kRef / 20;        // 9.5000, the band's edge at 5%

struct Recorder : EventListener {
    std::vector<Trade> trades;
    std::map<OrderId, OrderUpdate> last;
    void onTrade(const Trade& t) override { trades.push_back(t); }
    void onOrderUpdate(const OrderUpdate& u) override { last[u.orderId] = u; }
};

bool accepted(const AddOrderResult& r) { return std::holds_alternative<OrderId>(r); }

struct Book {
    Recorder rec;
    Recorder engine;
    std::unique_ptr<OrderBook> book;
    explicit Book(MatchAlgorithm algo = MatchAlgorithm::PriceTime)
        : book(std::make_unique<OrderBook>(0, algo)) {
        book->setEventListener(&rec);
        book->setEngineListener(&engine);
        book->setPriceRange(1, 2 * kRef);
    }
    OrderBook* operator->() { return book.get(); }
    Price reference() const { return book->checkpointPricesLocked().reference; }
};

// A print at kRef sets the reference, as a trade does.
void tradeAtReference(Book& b, OrderId& id) {
    ASSERT_TRUE(accepted(b->addOrder(id++, 1, Side::Sell, kRef, 1, OrderType::Limit)));
    ASSERT_TRUE(accepted(b->addOrder(id++, 2, Side::Buy, kRef, 1, OrderType::Limit)));
    ASSERT_EQ(b.reference(), kRef);
}

// ─── MR-2: elected stops ────────────────────────────────────────────────────

// The r2 reproduction: bids 10 lots every 0.10 from 9.90 to 5.00, 66 sell
// stop-markets of 5 at 9.95 and a 1-lot market sell that prints 9.90.
TEST(PrintBand, StopCascadeStopsAtTheBand) {
    for (MatchAlgorithm algo : {MatchAlgorithm::PriceTime, MatchAlgorithm::ProRata}) {
        Book b(algo);
        OrderId id = 1;
        tradeAtReference(b, id);
        for (Price p = 99000; p >= 50000; p -= 1000)
            ASSERT_TRUE(accepted(b->addOrder(id++, 20, Side::Buy, p, 10, OrderType::Limit)));
        for (int i = 0; i < 66; ++i)
            ASSERT_TRUE(accepted(b->addOrder(1000 + i, 30 + (i % 50), Side::Sell, 0, 5,
                                             OrderType::Stop, 99500)));
        b.rec.trades.clear();

        ASSERT_TRUE(accepted(b->addOrder(3000, 40, Side::Sell, 0, 1, OrderType::Market)));

        ASSERT_FALSE(b.rec.trades.empty()) << "the in-band trigger and stops did not trade";
        for (const Trade& t : b.rec.trades)
            EXPECT_GE(t.price, kLo) << "an elected stop printed outside the band";
        EXPECT_EQ(b->getTradingState(), TradingState::VolatilityAuction)
            << "the cascade reached the band and the breaker did not trip";
        int live = 0;
        for (int i = 0; i < 66; ++i)
            if (b->getOrder(1000 + i)) ++live;
        EXPECT_GT(live, 0) << "stops past the band were lost instead of kept for the reopen";
    }
}

// Elections latched while the book was in the auction fire at the reopen
// itself, not at the next incoming order, and their prints are tested against
// the reopening reference.
TEST(PrintBand, LatchedElectionsAreCheckedWhenTheyFire) {
    Book b;
    OrderId id = 1;
    tradeAtReference(b, id);
    for (Price p = 99000; p >= 50000; p -= 1000)
        ASSERT_TRUE(accepted(b->addOrder(id++, 20, Side::Buy, p, 10, OrderType::Limit)));
    for (int i = 0; i < 66; ++i)
        ASSERT_TRUE(accepted(b->addOrder(1000 + i, 30 + (i % 50), Side::Sell, 0, 5,
                                         OrderType::Stop, 99500)));
    ASSERT_TRUE(accepted(b->addOrder(3000, 40, Side::Sell, 0, 1, OrderType::Market)));
    ASSERT_EQ(b->getTradingState(), TradingState::VolatilityAuction);

    // Still parked as stops: elected (latched) but not executed. One that the
    // breaker stopped mid-execution is a market order parked for the cross.
    const auto parkedStops = [&] {
        int n = 0;
        for (int i = 0; i < 66; ++i)
            if (const Order* o = b->getOrder(1000 + i); o && o->type == OrderType::Stop) ++n;
        return n;
    };
    const int latched = parkedStops();
    ASSERT_GT(latched, 0);
    b.rec.trades.clear();

    ASSERT_TRUE(b->resumeVolatilityAuction());
    const Price reopen = b.reference();
    ASSERT_GT(reopen, 0);
    EXPECT_LT(parkedStops(), latched) << "the latched elections waited for the next order";
    ASSERT_FALSE(b.rec.trades.empty()) << "the latched elections never fired";
    const Price lo = reopen - reopen / 20;
    for (const Trade& t : b.rec.trades)
        EXPECT_GE(t.price, lo) << "a latched election printed outside the reopened band";
}

// A stop whose trigger the opening cross printed through is elected at the
// open, as one an incoming order printed through would be. It used to wait,
// armed, for the first order after the open.
TEST(PrintBand, TheOpeningCrossElectsAtTheOpen) {
    Book b;
    OrderId id = 1;
    tradeAtReference(b, id);
    ASSERT_TRUE(accepted(b->addOrder(id++, 20, Side::Buy, 98000, 10, OrderType::Limit)));
    ASSERT_TRUE(accepted(b->addOrder(500, 30, Side::Sell, 0, 5, OrderType::Stop, 99500)));
    b->setTradingState(TradingState::AuctionOpen);
    ASSERT_TRUE(accepted(b->addOrder(id++, 21, Side::Buy, 99000, 3, OrderType::Limit)));
    ASSERT_TRUE(accepted(b->addOrder(id++, 22, Side::Sell, 99000, 3, OrderType::Limit)));
    b.rec.trades.clear();

    b->setTradingState(TradingState::Continuous);   // crosses 3 at 9.90
    ASSERT_EQ(b->getTradingState(), TradingState::Continuous);
    EXPECT_EQ(b->getOrder(500), nullptr) << "the stop the open printed through is still armed";
    Quantity soldByStop = 0;
    for (const Trade& t : b.rec.trades)
        if (t.sellOrderId == 500) soldByStop += t.quantity;
    EXPECT_EQ(soldByStop, 5u);
    EXPECT_EQ(b.rec.last[500].status, OrderStatus::Filled);
}

TEST(PrintBand, InBandElectionStillTrades) {
    Book b;
    OrderId id = 1;
    tradeAtReference(b, id);
    ASSERT_TRUE(accepted(b->addOrder(id++, 20, Side::Buy, 99000, 10, OrderType::Limit)));
    ASSERT_TRUE(accepted(b->addOrder(id++, 20, Side::Buy, 98000, 10, OrderType::Limit)));
    ASSERT_TRUE(accepted(b->addOrder(500, 30, Side::Sell, 0, 15, OrderType::Stop, 99500)));
    b.rec.trades.clear();
    ASSERT_TRUE(accepted(b->addOrder(id++, 40, Side::Sell, 0, 1, OrderType::Market)));
    // 1 from the trigger, 15 from the stop: 9 at 9.90 then 6 at 9.80.
    Quantity sold = 0;
    for (const Trade& t : b.rec.trades) sold += t.quantity;
    EXPECT_EQ(sold, 16u);
    EXPECT_EQ(b->getOrder(500), nullptr);
    EXPECT_EQ(b->getTradingState(), TradingState::Continuous);
}

// ─── FORMAL-1: cancelReplace ────────────────────────────────────────────────

TEST(PrintBand, CrossingReplaceDoesNotPrintThroughTheBand) {
    for (MatchAlgorithm algo : {MatchAlgorithm::PriceTime, MatchAlgorithm::ProRata}) {
        Book b(algo);
        b->setReferencePrice(kRef);
        ASSERT_TRUE(accepted(b->addOrder(1, 10, Side::Sell, 50000, 10, OrderType::Limit)));
        ASSERT_TRUE(accepted(b->addOrder(2, 20, Side::Buy, 40000, 10, OrderType::Limit)));

        RejectReason why = RejectReason::None;
        ASSERT_TRUE(b->cancelReplace(2, 96000, 10, why, 20)) << static_cast<int>(why);
        EXPECT_TRUE(b.rec.trades.empty()) << "the replace printed 50% from the reference";
        EXPECT_EQ(b->getTradingState(), TradingState::VolatilityAuction)
            << "the out-of-band print did not trip the breaker, as a fresh order's does";
        ASSERT_NE(b->getOrder(2), nullptr) << "the replaced order must wait for the reopen";
        EXPECT_EQ(b->getOrder(2)->price, 96000);
        EXPECT_EQ(b->getOrder(2)->remainingQty, 10u);
    }
}

TEST(PrintBand, InBandReplaceStillTrades) {
    Book b;
    b->setReferencePrice(kRef);
    ASSERT_TRUE(accepted(b->addOrder(1, 10, Side::Sell, 99000, 10, OrderType::Limit)));
    ASSERT_TRUE(accepted(b->addOrder(2, 20, Side::Buy, 40000, 10, OrderType::Limit)));
    ASSERT_TRUE(b->cancelReplace(2, 99500, 10, 20));
    ASSERT_EQ(b.rec.trades.size(), 1u);
    EXPECT_EQ(b.rec.trades[0].price, 99000);
    EXPECT_EQ(b->getTradingState(), TradingState::Continuous);
}

// A replace is a fresh admission decision: far from the market and passive,
// it is admitted, as a fresh order at that price is.
TEST(PrintBand, FarPassiveReplaceIsAdmitted) {
    Book b;
    b->setReferencePrice(kRef);
    ASSERT_TRUE(accepted(b->addOrder(1, 10, Side::Buy, kRef, 10, OrderType::Limit)));
    RejectReason why = RejectReason::None;
    EXPECT_TRUE(b->cancelReplace(1, kRef / 2, 10, why, 10)) << static_cast<int>(why);
    EXPECT_EQ(b->getTradingState(), TradingState::Continuous);
}

// ─── Every print of a sweep, not only the first ─────────────────────────────

TEST(PrintBand, SweepStopsAtTheBandAndTheRemainderJoinsTheAuction) {
    for (MatchAlgorithm algo : {MatchAlgorithm::PriceTime, MatchAlgorithm::ProRata}) {
        {   // a limit: fills in band, the rest rests for the reopen
            Book b(algo);
            b->setReferencePrice(kRef);
            ASSERT_TRUE(accepted(b->addOrder(1, 1, Side::Sell, kRef + 100, 10, OrderType::Limit)));
            ASSERT_TRUE(accepted(b->addOrder(2, 1, Side::Sell, 2 * kRef, 10, OrderType::Limit)));
            ASSERT_TRUE(accepted(b->addOrder(3, 2, Side::Buy, 2 * kRef, 20, OrderType::Limit)));
            ASSERT_EQ(b.rec.trades.size(), 1u);
            EXPECT_EQ(b.rec.trades[0].price, kRef + 100);
            EXPECT_EQ(b->getTradingState(), TradingState::VolatilityAuction);
            ASSERT_NE(b->getOrder(3), nullptr);
            EXPECT_EQ(b->getOrder(3)->remainingQty, 10u);
        }
        {   // a market order: the rest is parked for the reopening cross
            Book b(algo);
            b->setReferencePrice(kRef);
            b->setMarketProtectionPct(0.0);
            ASSERT_TRUE(accepted(b->addOrder(1, 1, Side::Sell, kRef + 100, 10, OrderType::Limit)));
            ASSERT_TRUE(accepted(b->addOrder(2, 1, Side::Sell, 2 * kRef, 10, OrderType::Limit)));
            ASSERT_TRUE(accepted(b->addOrder(3, 2, Side::Buy, 0, 20, OrderType::Market)));
            ASSERT_EQ(b.rec.trades.size(), 1u);
            EXPECT_EQ(b->getTradingState(), TradingState::VolatilityAuction);
            ASSERT_NE(b->getOrder(3), nullptr) << "the market remainder was not parked";
            ASSERT_TRUE(b->resumeVolatilityAuction());
            Quantity bought = 0;
            for (const Trade& t : b.rec.trades) bought += t.quantity;
            EXPECT_EQ(bought, 20u) << "the parked remainder missed the reopening cross";
        }
        {   // an IOC: the rest is cancelled
            Book b(algo);
            b->setReferencePrice(kRef);
            ASSERT_TRUE(accepted(b->addOrder(1, 1, Side::Sell, kRef + 100, 10, OrderType::Limit)));
            ASSERT_TRUE(accepted(b->addOrder(2, 1, Side::Sell, 2 * kRef, 10, OrderType::Limit)));
            b->addOrder(3, 2, Side::Buy, 2 * kRef, 20, OrderType::IOC);
            ASSERT_EQ(b.rec.trades.size(), 1u);
            EXPECT_EQ(b->getOrder(3), nullptr);
            EXPECT_EQ(b.rec.last[3].status, OrderStatus::Cancelled);
            EXPECT_EQ(b.rec.last[3].filledQty, 10u);
        }
        {   // a FOK fills in full inside the band or not at all
            Book b(algo);
            b->setReferencePrice(kRef);
            ASSERT_TRUE(accepted(b->addOrder(1, 1, Side::Sell, kRef + 100, 10, OrderType::Limit)));
            ASSERT_TRUE(accepted(b->addOrder(2, 1, Side::Sell, 2 * kRef, 10, OrderType::Limit)));
            auto r = b->addOrder(3, 2, Side::Buy, 2 * kRef, 20, OrderType::FOK);
            ASSERT_TRUE(std::holds_alternative<RejectReason>(r));
            EXPECT_EQ(std::get<RejectReason>(r), RejectReason::FOKInsufficientLiquidity);
            EXPECT_TRUE(b.rec.trades.empty()) << "a FOK filled part of its size";
            auto ok = b->addOrder(4, 2, Side::Buy, 2 * kRef, 10, OrderType::FOK);
            EXPECT_TRUE(accepted(ok));
            EXPECT_EQ(b.rec.trades.size(), 1u);
            EXPECT_EQ(b->getTradingState(), TradingState::Continuous);
        }
    }
}

// ─── MR-1: the reference comes from a print ─────────────────────────────────

TEST(PrintBand, RejectedOrderDoesNotSeedTheReference) {
    Book b;
    auto fok = b->addOrder(1, 66, Side::Sell, 1, 1, OrderType::FOK);  // 0.0001, no bids
    ASSERT_TRUE(std::holds_alternative<RejectReason>(fok));
    EXPECT_EQ(b.reference(), 0) << "a rejected order set the breaker reference";
    ASSERT_TRUE(accepted(b->addOrder(2, 10, Side::Sell, kRef, 100, OrderType::Limit)));
    EXPECT_EQ(b.reference(), 0) << "a resting order is not a print";
    ASSERT_TRUE(accepted(b->addOrder(3, 11, Side::Buy, kRef, 100, OrderType::Limit)));
    EXPECT_EQ(b.rec.trades.size(), 1u) << "an honest cross at a normal price did not trade";
    EXPECT_EQ(b->getTradingState(), TradingState::Continuous);
    EXPECT_EQ(b.reference(), kRef) << "the first print sets the reference";

    // A rejected PostOnly does not move it either.
    ASSERT_TRUE(accepted(b->addOrder(4, 10, Side::Sell, kRef + 100, 10, OrderType::Limit)));
    EXPECT_FALSE(accepted(b->addOrder(5, 12, Side::Buy, kRef + 100, 10, OrderType::PostOnly)));
    EXPECT_EQ(b.reference(), kRef);
}

// With no reference the first print sets it, and the same sweep's later prints
// are tested against it.
TEST(PrintBand, FirstPrintSeedsTheReferenceForTheRestOfItsSweep) {
    Book b;
    ASSERT_TRUE(accepted(b->addOrder(1, 1, Side::Sell, kRef, 10, OrderType::Limit)));
    ASSERT_TRUE(accepted(b->addOrder(2, 1, Side::Sell, 2 * kRef, 10, OrderType::Limit)));
    ASSERT_TRUE(accepted(b->addOrder(3, 2, Side::Buy, 2 * kRef, 20, OrderType::Limit)));
    ASSERT_EQ(b.rec.trades.size(), 1u);
    EXPECT_EQ(b.reference(), kRef);
    EXPECT_EQ(b->getTradingState(), TradingState::VolatilityAuction);
}

// ─── Replay ─────────────────────────────────────────────────────────────────

// A trip mid-sweep lands at the same fill on replay: the order's in-band fills
// are kept and the rest waits in the auction.
TEST(PrintBand, ReplayReproducesAMidSweepTrip) {
    const std::filesystem::path path = uniqueTempPath("print_band.journal");
    std::filesystem::remove(path);
    const SymbolId sym = 0;
    {
        MatchingEngine e;
        e.addSymbol(sym);
        ASSERT_TRUE(e.enableJournal(path.string()));
        e.start();
        e.submitOrder(sym, 1, 1, Side::Sell, kRef, 1, OrderType::Limit);
        e.submitOrder(sym, 2, 2, Side::Buy, kRef, 1, OrderType::Limit);
        e.submitOrder(sym, 3, 1, Side::Sell, kRef + 100, 10, OrderType::Limit);
        e.submitOrder(sym, 4, 1, Side::Sell, 2 * kRef, 10, OrderType::Limit);
        e.submitOrder(sym, 5, 2, Side::Buy, 2 * kRef, 20, OrderType::Limit);
        e.waitForDrain();
        OrderBook* book = e.getOrderBook(sym);
        ASSERT_EQ(book->getTradingState(), TradingState::VolatilityAuction);
        ASSERT_EQ(book->getTradeCount(), 2u);
        e.stop();
    }
    MatchingEngine r;
    r.addSymbol(sym);
    ASSERT_TRUE(r.enableJournal(path.string()));
    r.replayJournal();
    OrderBook* book = r.getOrderBook(sym);
    EXPECT_EQ(book->getTradingState(), TradingState::VolatilityAuction);
    EXPECT_EQ(book->getTradeCount(), 2u) << "replay did not stop the sweep where it stopped live";
    ASSERT_NE(book->getOrder(5), nullptr);
    EXPECT_EQ(book->getOrder(5)->remainingQty, 10u);
    std::filesystem::remove(path);
}

// ─── A reference near the top of the price range ───────────────────────────

// Found by fuzz_gateway_engine. The band's upper edge was reference + 5% with
// no guard: for a reference this high the sum passes the largest Price, which
// is undefined (UBSan aborts) and in a plain build wraps below zero, so the
// next print at the reference itself tripped the breaker.
TEST(PrintBand, TheUpperEdgeSaturatesAtTheTopOfThePriceRange) {
    constexpr Price kHuge = 8863084066665146128;  // the fuzzer's reference
    OrderBook book(0, MatchAlgorithm::PriceTime);
    Recorder rec;
    book.setEventListener(&rec);
    OrderId id = 1;
    // The first print sets the reference; the second is tested against it.
    for (int print = 0; print < 2; ++print) {
        ASSERT_TRUE(accepted(book.addOrder(id++, 1, Side::Sell, kHuge, 1, OrderType::Limit)));
        ASSERT_TRUE(accepted(book.addOrder(id++, 2, Side::Buy, kHuge, 1, OrderType::Limit)));
    }
    EXPECT_EQ(rec.trades.size(), 2u);
    EXPECT_EQ(book.getTradingState(), TradingState::Continuous)
        << "a print at the reference price tripped the breaker";
}

}  // namespace
