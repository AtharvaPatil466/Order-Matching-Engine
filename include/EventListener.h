#pragma once

namespace OrderMatcher {

// Forward declarations
struct Trade;
struct OrderUpdate;
struct MarketDataUpdate;
struct BookVisibleUpdate;

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

    // Called when the PUBLICLY DISPLAYED book changes at order granularity.
    //
    // Distinct from onOrderUpdate, which is the private owner-facing channel:
    // that one fires for hidden orders and reports true size, because the
    // order's owner is entitled to both. This one is the public projection —
    // hidden orders never appear on it, and icebergs report only their current
    // slice. An order-level market-data feed (ITCH) must be driven from HERE,
    // not from onOrderUpdate, or it rebroadcasts private state to the world.
    virtual void onBookVisible(const BookVisibleUpdate& /*update*/) {}
};

// Null listener singleton — avoids nullptr checks on hot path.
// All methods are no-ops (base class defaults).
inline EventListener& nullListener() {
    static EventListener instance;
    return instance;
}

} // namespace OrderMatcher
