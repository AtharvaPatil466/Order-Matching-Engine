// AdmissionTifTest — time-in-force and expiry at admission (MR-6, MR-8).
//
// MR-6: a GTD order whose expiry time had already passed was accepted and
//   could trade until the next expiry sweep. It is refused (InvalidFieldValue).
// MR-8: an IOC or FOK sent with a DAY time-in-force and no expiry was refused
//   MissingRequiredField when no session end is scheduled (as in both
//   binaries). An order that never rests does not need a session end.

#include "EventListener.h"
#include "MatchingEngine.h"
#include "OrderBook.h"

#include <gtest/gtest.h>

#include <vector>

using namespace OrderMatcher;

namespace {
constexpr Price PX = 100000;
constexpr uint64_t kNow = 5'000'000'000ull;

struct Trades : EventListener {
    std::vector<Trade> t;
    void onTrade(const Trade& x) override { t.push_back(x); }
};

struct Engine {
    MatchingEngine e;
    Trades trades;
    Engine() {
        e.addSymbol(0);
        e.setExpiryClock([] { return kNow; });
        e.start();
        e.getOrderBook(0)->setEventListener(&trades);
        e.getOrderBook(0)->setCircuitBreakerThreshold(1e9);
    }
};
}  // namespace

TEST(AdmissionTif, AnOrderAlreadyPastItsExpiryIsRefused) {
    Engine x;
    const auto a = x.e.submitOrder(0, 1, 10, Side::Sell, PX, 10, OrderType::Limit, 0, 0,
                                   TimeInForce::GTD, kNow - 1);
    EXPECT_FALSE(a.isAccepted());
    EXPECT_EQ(a.rejectReason, RejectReason::InvalidFieldValue);
    x.e.submitOrder(0, 2, 11, Side::Buy, PX, 10, OrderType::Limit);
    EXPECT_TRUE(x.trades.t.empty()) << "an expired order traded";
}

TEST(AdmissionTif, AnOrderExpiringLaterIsAccepted) {
    Engine x;
    const auto a = x.e.submitOrder(0, 1, 10, Side::Sell, PX, 10, OrderType::Limit, 0, 0,
                                   TimeInForce::GTD, kNow + 1);
    EXPECT_TRUE(a.isAccepted());
}

TEST(AdmissionTif, IocAndFokWithDayNeedNoSessionEnd) {
    Engine x;
    x.e.setSessionEndScheduled(false);
    x.e.submitOrder(0, 1, 10, Side::Sell, PX, 20, OrderType::Limit);
    const auto ioc = x.e.submitOrder(0, 2, 11, Side::Buy, PX, 5, OrderType::IOC, 0, 0,
                                     TimeInForce::DAY, 0);
    EXPECT_TRUE(ioc.isAccepted()) << "reason " << static_cast<int>(ioc.rejectReason);
    const auto fok = x.e.submitOrder(0, 3, 11, Side::Buy, PX, 5, OrderType::FOK, 0, 0,
                                     TimeInForce::DAY, 0);
    EXPECT_TRUE(fok.isAccepted()) << "reason " << static_cast<int>(fok.rejectReason);
    EXPECT_EQ(x.trades.t.size(), 2u);
    // A resting DAY order still needs one.
    const auto day = x.e.submitOrder(0, 4, 11, Side::Buy, PX - 1, 5, OrderType::Limit, 0, 0,
                                     TimeInForce::DAY, 0);
    EXPECT_EQ(day.rejectReason, RejectReason::MissingRequiredField);
}
