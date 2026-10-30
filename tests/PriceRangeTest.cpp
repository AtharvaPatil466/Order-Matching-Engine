// PriceRangeTest — audit STP-8, roadmap 1.6-F6.
//
// WHAT WAS WRONG. The book's price band (a fixed 200,001-tick FlatPriceMap
// window) was centred on the first price to reach addToBook and never moved,
// and it was checked only inside addToBook — after admission, after the ack,
// after matching. So:
//   * one participant's first order fixed the band for the session: a 1-tick
//     bid, and every later order at a real price went Accepted -> Rejected
//     (OutOfPriceRange) -> Cancelled, three statuses for one order;
//   * an elected stop-limit priced outside the band traded, then went
//     Rejected -> Cancelled with its fills already done;
//   * a first order near INT64_MAX overflowed the band arithmetic (UBSan
//     signed overflow) and the book refused that very order;
//   * a replace to a price outside the band took the order off the book and
//     cancelled it.
//
// WHAT IS PINNED. A band can be configured up front (setPriceRange, from
// instrument.<id>.min_price/max_price in engine.conf), and the first order
// cannot move it. An order or replace priced outside a configured band is
// refused at admission with one Rejected(OutOfPriceRange) and nothing else; a
// StopLimit is checked on its stopLimitPrice. An unconfigured book still
// anchors on its first rest (requiring a band everywhere is an open decision),
// and that anchor saturates instead of overflowing.

#include "OrderBook.h"

#include <cassert>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <variant>
#include <vector>

using namespace OrderMatcher;

namespace {

struct Recorder : EventListener {
    std::vector<OrderUpdate> updates;
    std::vector<Trade> trades;
    void onOrderUpdate(const OrderUpdate& u) override { updates.push_back(u); }
    void onTrade(const Trade& t) override { trades.push_back(t); }
    std::vector<OrderUpdate> of(OrderId id) const {
        std::vector<OrderUpdate> out;
        for (const auto& u : updates) if (u.orderId == id) out.push_back(u);
        return out;
    }
};

bool rejectedWith(const AddOrderResult& r, RejectReason want) {
    return std::holds_alternative<RejectReason>(r) && std::get<RejectReason>(r) == want;
}

// Exactly one update for `id`, and it is Rejected(OutOfPriceRange).
void expectSingleRangeReject(const Recorder& rec, OrderId id, const char* what) {
    const auto us = rec.of(id);
    const bool ok = us.size() == 1 && us[0].status == OrderStatus::Rejected &&
                    us[0].rejectReason == RejectReason::OutOfPriceRange;
    if (!ok) {
        std::fprintf(stderr, "%s: order %llu got %zu update(s):", what,
                     (unsigned long long)id, us.size());
        for (const auto& u : us)
            std::fprintf(stderr, " [status=%d reason=%d filled=%llu]", (int)u.status,
                         (int)u.rejectReason, (unsigned long long)u.filledQty);
        std::fputc('\n', stderr);
        assert(false && "an out-of-range order must get one Rejected(OutOfPriceRange) at admission");
    }
}

void relaxed(OrderBook& b) { b.setCircuitBreakerThreshold(1e9); }

// A configured band is the band: the first order does not move it.
void configuredBand() {
    OrderBook book(1);
    relaxed(book);
    Recorder rec;
    book.setEventListener(&rec);
    book.setPriceRange(9'000, 11'000);

    // Audit T7: a 1-tick first bid anchored the band, and every later order at
    // a real price went Accepted -> Rejected -> Cancelled. Against a configured
    // band the 1-tick bid is the one refused.
    const auto low = book.addOrder(1, 1, Side::Buy, 1, 10, OrderType::Limit);
    expectSingleRangeReject(rec, 1, "configured band, first order below it");
    assert(rejectedWith(low, RejectReason::OutOfPriceRange));
    assert(std::holds_alternative<OrderId>(book.addOrder(2, 1, Side::Buy, 9'000, 10,
                                                         OrderType::Limit)));
    assert(std::holds_alternative<OrderId>(book.addOrder(3, 2, Side::Sell, 11'000, 10,
                                                         OrderType::Limit)));
    book.addOrder(4, 2, Side::Sell, 11'001, 10, OrderType::Limit);
    expectSingleRangeReject(rec, 4, "configured band, one tick above it");
    book.addOrder(5, 3, Side::Buy, 20'000, 10, OrderType::IOC);
    expectSingleRangeReject(rec, 5, "configured band, IOC above it");

    // Audit T4b: an elected stop-limit whose limit is outside the band used to
    // trade, then go Rejected -> Cancelled. It is refused before it parks.
    book.addOrder(6, 3, Side::Sell, 0, 30, OrderType::StopLimit, /*stopPrice=*/9'500, 0,
                  TimeInForce::GTC, 0, /*stopLimitPrice=*/8'000);
    expectSingleRangeReject(rec, 6, "StopLimit with stopLimitPrice below the band");

    // A replace to a price outside the band is refused; the order keeps resting.
    RejectReason reason = RejectReason::None;
    assert(!book.cancelReplace(2, 8'000, 10, reason) && reason == RejectReason::OutOfPriceRange);
    assert(book.getOrder(2) != nullptr && book.getBestBid() == 9'000 &&
           "a refused replace leaves the order where it was");

    // Configuring a band over live orders, or a band wider than the map, throws.
    bool threw = false;
    try { book.setPriceRange(1, 100); } catch (const std::logic_error&) { threw = true; }
    assert(threw && "a band cannot be moved under resting orders");
    OrderBook wide(2);
    threw = false;
    try { wide.setPriceRange(1, 10'000'000); } catch (const std::invalid_argument&) { threw = true; }
    assert(threw && "a band wider than the price map is a config error");
    std::puts("  configured band: first order cannot move it; orders, IOC, stop-limit "
              "and replace outside it refused");
}

// Audit T8: a first order at INT64_MAX overflowed minPrice + capacity - 1.
void anchorSaturates() {
    OrderBook book(1);
    relaxed(book);
    constexpr Price kMax = std::numeric_limits<Price>::max();
    const auto r = book.addOrder(1, 1, Side::Buy, kMax, 10, OrderType::Limit);
    assert(std::holds_alternative<OrderId>(r));
    if (book.getOrder(1) == nullptr || book.getBestBid() != kMax) {
        std::fprintf(stderr, "INT64_MAX bid: inBook=%d bestBid=%lld\n",
                     book.getOrder(1) != nullptr, (long long)book.getBestBid());
        assert(false && "the band anchored on INT64_MAX must contain INT64_MAX");
    }
    std::puts("  anchor at INT64_MAX saturates; the order rests");
}

}  // namespace

int main() {
    configuredBand();
    anchorSaturates();
    std::puts("PriceRangeTest passed");
    return 0;
}
