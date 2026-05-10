#pragma once

namespace OrderMatcher {

// Forward declarations
struct Trade;
struct OrderUpdate;
struct MarketDataUpdate;

// Virtual interface replacing std::function callbacks.
// Zero-cost dispatch via vtable — no heap allocation, no type erasure overhead.
// The matching engine and order book call through this interface on every trade,
// order update, and market data change.
//
// Users subclass and override only the events they care about.
// Default implementations are no-ops for convenience.

class EventListener {
public:
    virtual ~EventListener() = default;

    // Called on every trade execution
    virtual void onTrade(const Trade& /*trade*/) {}

    // Called on every order status change (accept, fill, cancel, reject)
    virtual void onOrderUpdate(const OrderUpdate& /*update*/) {}

    // Called on every book change (add, modify, delete price level)
    virtual void onMarketData(const MarketDataUpdate& /*update*/) {}
};

// Null listener singleton — avoids nullptr checks on hot path.
// All methods are no-ops (base class defaults).
inline EventListener& nullListener() {
    static EventListener instance;
    return instance;
}

} // namespace OrderMatcher
