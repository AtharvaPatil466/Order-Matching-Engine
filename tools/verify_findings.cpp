#include "OrderBook.h"
#include "EventListener.h"
#include <cstdio>
#include <vector>
#include <string>

using namespace OrderMatcher;

struct Capture : EventListener {
    std::vector<OrderUpdate> updates;
    std::vector<Trade> trades;
    void onOrderUpdate(const OrderUpdate& u) override { updates.push_back(u); }
    void onTrade(const Trade& t) override { trades.push_back(t); }
};

static const char* statusName(OrderStatus s) {
    switch (s) {
        case OrderStatus::Accepted: return "Accepted";
        case OrderStatus::PartiallyFilled: return "PartiallyFilled";
        case OrderStatus::Filled: return "Filled";
        case OrderStatus::Cancelled: return "Cancelled";
        case OrderStatus::Rejected: return "Rejected";
        default: return "Other";
    }
}

// ── Finding 1: pro-rata self-trade reports a phantom full fill ──
static void finding1() {
    printf("\n=== FINDING 1: Pro-rata SMP phantom fill ===\n");
    OrderBook ob(0, MatchAlgorithm::ProRata);
    Capture cap;
    ob.setEventListener(&cap);

    const ParticipantId P = 42;
    // Default STP mode is None -> checkSMP still returns true (detection always on)
    // Resting sell from P at 100, qty 10
    ob.addOrder(1, P, Side::Sell, 100, 10, OrderType::Limit);
    cap.updates.clear();
    cap.trades.clear();

    // Aggressive buy from SAME participant P, qty 5, crosses
    ob.addOrder(2, P, Side::Buy, 100, 5, OrderType::Limit);

    printf("trades executed: %zu (expect 0 for self-trade)\n", cap.trades.size());
    bool sawPhantomFill = false;
    for (auto& u : cap.updates) {
        if (u.orderId == 2) {
            printf("  order 2 update: status=%s filledQty=%llu remainingQty=%llu lastFillPrice=%lld\n",
                   statusName(u.status), (unsigned long long)u.filledQty,
                   (unsigned long long)u.remainingQty, (long long)u.lastFillPrice);
            if (u.status == OrderStatus::Filled) sawPhantomFill = true;
        }
    }
    printf("RESULT: order 2 reported as Filled with no trade? %s\n",
           (sawPhantomFill && cap.trades.empty()) ? "YES (BUG CONFIRMED)" : "no");
}

// ── Finding 2: stop cascade does not chain within one event ──
static void finding2() {
    printf("\n=== FINDING 2: Stop cascade single-pass ===\n");
    OrderBook ob(0, MatchAlgorithm::PriceTime);
    Capture cap;
    ob.setEventListener(&cap);

    // Build a sell-side ladder so that buy stops triggering walk price UP.
    // Resting asks: 101 (qty 10), 105 (qty 10), 110 (qty 10)
    ob.addOrder(10, 1, Side::Sell, 101, 10, OrderType::Limit);
    ob.addOrder(11, 1, Side::Sell, 105, 10, OrderType::Limit);
    ob.addOrder(12, 1, Side::Sell, 110, 10, OrderType::Limit);

    // Buy STOP A: stopPrice 101 -> fires when lastTradePrice >= 101, becomes limit @ buy.
    // We give it a limit-after-trigger price high enough to lift to 105.
    // Stop type becomes a plain Limit at order->price (the original price arg).
    ob.addOrder(20, 2, Side::Buy, 106, 10, OrderType::Stop, /*stopPrice=*/101);
    // Buy STOP B: stopPrice 105 -> should only fire AFTER stop A lifts price to 105.
    ob.addOrder(21, 3, Side::Buy, 111, 10, OrderType::Stop, /*stopPrice=*/105);

    cap.updates.clear();
    cap.trades.clear();

    // Trigger: a trade at 101 to set lastTradePrice_ = 101.
    // Incoming sell? No — we want a buy that lifts 101 to create a print at 101.
    ob.addOrder(30, 4, Side::Buy, 101, 5, OrderType::Limit);

    printf("trades after trigger event: %zu\n", cap.trades.size());
    for (auto& t : cap.trades)
        printf("  trade px=%lld qty=%llu buyId=%llu sellId=%llu\n",
               (long long)t.price, (unsigned long long)t.quantity,
               (unsigned long long)t.buyOrderId, (unsigned long long)t.sellOrderId);

    // Did stop B (order 21) trade in THIS event? If cascade is single-pass,
    // stop B will NOT have fired because it is evaluated against the stale
    // lastTradePrice (101), not the post-stop-A price (105).
    bool stopBTraded = false;
    for (auto& t : cap.trades)
        if (t.buyOrderId == 21) stopBTraded = true;
    printf("RESULT: stop B (needs px>=105) cascaded in same event? %s\n",
           stopBTraded ? "YES" : "NO (single-pass confirmed)");
}

int main() {
    finding1();
    finding2();
    printf("\n=== done ===\n");
    return 0;
}
