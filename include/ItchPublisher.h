#pragma once

// ItchPublisher — converts engine events for one OrderBook into ITCH
// 5.0 wire frames.
//
// Per-book scope: each publisher is bound to a single OrderBook at
// construction so it knows its symbol and can look up resting orders
// to fetch the side/price the EventListener path doesn't surface.
// Real ITCH feeds aggregate across all symbols on the venue — to wire
// that up, fan out via MultiplexListener (one ItchPublisher per book,
// each writing to a shared sink).
//
// Sink shape `void(std::string_view)` is transport-agnostic: tests
// accumulate to a string; production wires to UDP multicast or the
// existing SHM market-data fan-out.
//
// Mapping (per ITCH 5.0):
//   onOrderUpdate(Accepted,   remaining>0) → 'A' (AddOrder)
//   onTrade                                → 'E' (Executed) per live side
//   onOrderUpdate(Filled,     remaining=0) → 'D' (Delete) — order is now
//                                             off the book
//   onOrderUpdate(Cancelled)               → 'D' (Delete)
//   onOrderUpdate(Rejected)                → nothing (rejects are private)
//   onOrderUpdate(PartiallyFilled)         → nothing (the 'E' was the
//                                             public event)
//
// The publisher tracks "live" order IDs internally so 'D' is only
// ever emitted for orders previously announced via 'A', and 'E' is
// only emitted for sides we publicly know about.

#include "EventListener.h"
#include "ItchProtocol.h"
#include "Order.h"
#include "OrderBook.h"
#include "Types.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace OrderMatcher {

class ItchPublisher : public EventListener {
public:
    using SendBytes = std::function<void(std::string_view)>;
    using ClockFn   = std::function<uint64_t()>;

    ItchPublisher(OrderBook& book, SendBytes send)
        : book_(book), send_(std::move(send)) {}

    // Inject a clock for deterministic test assertions. Default uses
    // steady_clock::now() in nanoseconds. Real ITCH expects
    // nanoseconds since UTC midnight — the production caller wires
    // that timestamp source.
    void setClock(ClockFn clock) { clock_ = std::move(clock); }

    // Emit a 'S' (SystemEvent) bookend manually. Subscribers expect
    // these at session boundaries (start/end of market, halts); the
    // publisher has no concept of "session" so it doesn't generate
    // these autonomously.
    void publishSystemEvent(char eventCode) {
        if (!send_) return;
        uint8_t buf[ITCH_SIZE_SYSTEM_EVENT];
        size_t n = encodeSystemEvent(buf, locateOf(), nextTracking(),
                                     now(), eventCode);
        send_(std::string_view(reinterpret_cast<const char*>(buf), n));
        ++messagesEmitted_;
    }

    // Emit a 'R' (Stock Directory) for this publisher's book. Real
    // venues publish these at the start of a trading session so
    // subscribers know which symbols are listed before any 'A'
    // arrives. Default field values reflect a typical equity
    // symbol; override per book if needed.
    void publishStockDirectory(char marketCategory = 'Q',
                               char financialStatus = 'N',
                               uint32_t roundLotSize = 100) {
        if (!send_) return;
        uint8_t buf[ITCH_SIZE_STOCK_DIRECTORY];
        size_t n = encodeStockDirectory(
            buf, locateOf(), nextTracking(), now(),
            book_.getSymbolId(), marketCategory,
            financialStatus, roundLotSize, 'N', 'C');
        send_(std::string_view(reinterpret_cast<const char*>(buf), n));
        ++directoriesEmitted_;
        ++messagesEmitted_;
    }

    // Emit a 'H' (Trading Action) reflecting the book's current
    // TradingState. Real venues send one each time a halt is
    // declared or lifted; the engine's TradingState transitions
    // drive these. Public API so the gateway / regulator-side
    // controller can fire it at the right time — the publisher
    // doesn't subscribe to TradingState changes (no listener
    // surface for that yet), so this remains a caller-driven event.
    //
    // Pass `reason` as a 4-char ASCII code (e.g. "MWC1" for
    // market-wide circuit breaker level 1, "T1" for volatility
    // halt, "    " for none).
    void publishTradingAction(const char reason4[4] = "    ") {
        if (!send_) return;
        char wireState = tradingStateToItch(book_.getTradingState());
        uint8_t buf[ITCH_SIZE_TRADING_ACTION];
        size_t n = encodeTradingAction(
            buf, locateOf(), nextTracking(), now(),
            book_.getSymbolId(), wireState, reason4);
        send_(std::string_view(reinterpret_cast<const char*>(buf), n));
        ++tradingActionsEmitted_;
        ++messagesEmitted_;
    }

    // Emit a 'Q' (Cross Trade) for an auction-uncross print.
    // Distinct from a continuous-matching trade — used when
    // reporting the aggregate volume and discovered price at the
    // open, close, or post-halt resumption.
    void publishCrossTrade(uint64_t shares, Price crossPrice,
                           uint64_t matchNumber, char crossType) {
        if (!send_) return;
        uint8_t buf[ITCH_SIZE_CROSS_TRADE];
        size_t n = encodeCrossTrade(buf, locateOf(), nextTracking(),
                                    now(), shares, book_.getSymbolId(),
                                    crossPrice, matchNumber, crossType);
        send_(std::string_view(reinterpret_cast<const char*>(buf), n));
        ++crossTradesEmitted_;
        ++messagesEmitted_;
    }

    uint64_t addsEmitted()              const { return addsEmitted_; }
    uint64_t executedEmitted()          const { return executedEmitted_; }
    uint64_t deletesEmitted()           const { return deletesEmitted_; }
    uint64_t tradingActionsEmitted()    const { return tradingActionsEmitted_; }
    uint64_t directoriesEmitted()       const { return directoriesEmitted_; }
    uint64_t crossTradesEmitted()       const { return crossTradesEmitted_; }
    uint64_t messagesEmitted()          const { return messagesEmitted_; }
    size_t   liveOrderCount()           const { return liveOrders_.size(); }

    // ── EventListener overrides ─────────────────────────────────────────

    void onOrderUpdate(const OrderUpdate& u) override {
        switch (u.status) {
        case OrderStatus::Accepted:
            // Public 'A' fires only when the order actually rests on
            // the book. An IOC that fully filled comes through as
            // Accepted with remaining=0 — no 'A' for that case.
            if (u.remainingQty > 0) emitAdd(u);
            break;
        case OrderStatus::Cancelled:
        case OrderStatus::Filled:
            emitDeleteIfLive(u.orderId);
            break;
        case OrderStatus::PartiallyFilled:
        case OrderStatus::Rejected:
        case OrderStatus::New:
            // No wire event — see header comment.
            break;
        }
    }

    void onTrade(const Trade& t) override {
        // Each side of the trade gets its own 'E' iff we previously
        // 'A'-announced that order. An aggressive order that fully
        // filled on entry was never on the public book, so the
        // subscriber view stays consistent — no 'E' for that order.
        emitExecutedIfLive(t.buyOrderId, t);
        emitExecutedIfLive(t.sellOrderId, t);
    }

    void onMarketData(const MarketDataUpdate&) override {
        // ITCH operates at the order level. Price-level aggregates
        // are redundant with the per-order A/E/X/D stream.
    }

private:
    struct LiveOrder {
        Side     side;
        Quantity shares;     // current leaves (decremented on fills)
        Price    price;
    };

    void emitAdd(const OrderUpdate& u) {
        if (!send_) return;
        // OrderUpdate doesn't carry side/price/symbol — fetch from the
        // book. If the order isn't there (race / non-resting type),
        // skip; nothing to advertise. The book is the source of truth
        // for what subscribers should see.
        const Order* o = book_.getOrder(u.orderId);
        if (!o) return;

        LiveOrder le{o->side, u.remainingQty, o->price};
        liveOrders_[u.orderId] = le;

        uint8_t buf[ITCH_SIZE_ADD_ORDER];
        size_t n = encodeAddOrder(buf, locateOf(), nextTracking(),
                                  now(), static_cast<uint64_t>(u.orderId),
                                  le.side, le.shares,
                                  book_.getSymbolId(), le.price);
        send_(std::string_view(reinterpret_cast<const char*>(buf), n));
        ++addsEmitted_;
        ++messagesEmitted_;
    }

    void emitExecutedIfLive(OrderId id, const Trade& t) {
        auto it = liveOrders_.find(id);
        if (it == liveOrders_.end()) return;

        Quantity prev = it->second.shares;
        Quantity filled = (t.quantity > prev) ? prev : t.quantity;
        it->second.shares = prev - filled;

        if (send_) {
            uint8_t buf[ITCH_SIZE_ORDER_EXECUTED];
            size_t n = encodeOrderExecuted(buf, locateOf(), nextTracking(),
                                           now(), static_cast<uint64_t>(id),
                                           filled, t.tradeId);
            send_(std::string_view(reinterpret_cast<const char*>(buf), n));
            ++executedEmitted_;
            ++messagesEmitted_;
        }
        // Do NOT erase here. The subsequent OrderUpdate{Filled} drives
        // the 'D' emission. Erasing here would make 'D' a no-op and
        // subscribers would never see the book removal.
    }

    void emitDeleteIfLive(OrderId id) {
        auto it = liveOrders_.find(id);
        if (it == liveOrders_.end()) return;

        if (send_) {
            uint8_t buf[ITCH_SIZE_ORDER_DELETE];
            size_t n = encodeOrderDelete(buf, locateOf(), nextTracking(),
                                         now(), static_cast<uint64_t>(id));
            send_(std::string_view(reinterpret_cast<const char*>(buf), n));
            ++deletesEmitted_;
            ++messagesEmitted_;
        }
        liveOrders_.erase(it);
    }

    uint16_t locateOf() const {
        return static_cast<uint16_t>(book_.getSymbolId());
    }

    uint16_t nextTracking() {
        // Per-publisher monotonic, wraps at 65536. Subscribers use
        // this for intra-publisher ordering checks; transport-level
        // sequence gaps (MoldUDP) are detected separately.
        return trackingCounter_++;
    }

    uint64_t now() const {
        if (clock_) return clock_();
        return static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
    }

    OrderBook&                                   book_;
    SendBytes                                    send_;
    ClockFn                                      clock_;
    std::unordered_map<OrderId, LiveOrder>       liveOrders_;
    uint16_t                                     trackingCounter_{0};
    uint64_t                                     addsEmitted_{0};
    uint64_t                                     executedEmitted_{0};
    uint64_t                                     deletesEmitted_{0};
    uint64_t                                     tradingActionsEmitted_{0};
    uint64_t                                     directoriesEmitted_{0};
    uint64_t                                     crossTradesEmitted_{0};
    uint64_t                                     messagesEmitted_{0};
};

}  // namespace OrderMatcher
