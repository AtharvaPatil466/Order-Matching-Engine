#pragma once

// MultiplexListener — fan-out adapter for OrderBook's single-listener slot.
//
// OrderBook holds exactly one EventListener pointer. That's the right
// shape for hot-path performance (one vtable dispatch, no container
// iteration) but means that any setup with multiple interested
// subscribers — OUCH session + ITCH publisher + structured-log sink —
// would clobber each other.
//
// MultiplexListener composes N listeners behind a single EventListener
// interface. Install one of THESE on the book; register child
// listeners with add()/remove(). Every callback fans out in
// registration order.
//
// Caveats:
//   * Not thread-safe. Add/remove only when the book isn't actively
//     firing events. Typical usage: configure at startup, then run.
//   * Children must outlive this object — MultiplexListener stores
//     raw pointers, no ownership.
//   * Order matters for observers that care about ordering between
//     each other (rare). Registration order is delivery order.

#include "EventListener.h"

#include <algorithm>
#include <vector>

namespace OrderMatcher {

class MultiplexListener : public EventListener {
public:
    void add(EventListener* listener) {
        if (!listener) return;
        // Idempotent: don't register the same listener twice.
        if (std::find(listeners_.begin(), listeners_.end(), listener) ==
                listeners_.end()) {
            listeners_.push_back(listener);
        }
    }

    void remove(EventListener* listener) {
        listeners_.erase(
            std::remove(listeners_.begin(), listeners_.end(), listener),
            listeners_.end());
    }

    size_t size() const { return listeners_.size(); }

    void onTrade(const Trade& t) override {
        for (auto* l : listeners_) l->onTrade(t);
    }

    void onOrderUpdate(const OrderUpdate& u) override {
        for (auto* l : listeners_) l->onOrderUpdate(u);
    }

    void onMarketData(const MarketDataUpdate& u) override {
        for (auto* l : listeners_) l->onMarketData(u);
    }

    // Adds ('A') arrive on this channel, not onOrderUpdate — a fan-out that
    // forgets it silently produces a feed with no order additions at all.
    void onBookVisible(const BookVisibleUpdate& u) override {
        for (auto* l : listeners_) l->onBookVisible(u);
    }

private:
    std::vector<EventListener*> listeners_;
};

}  // namespace OrderMatcher
