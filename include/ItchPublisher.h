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
//
// ── Threading / decoupling (P3-1) ───────────────────────────────────
// By DEFAULT the publisher is synchronous: onOrderUpdate/onTrade and the
// publishXxx control calls serialize the ITCH frame and invoke the sink
// INLINE on the calling (matching) thread. That puts ITCH serialization
// and the downstream UDP send on the matching engine's critical path.
//
// Call enableAsyncPublishing() to DECOUPLE that path. In async mode the
// engine-facing calls only capture a small POD RawEvent and push it into
// a lock-free MPSC ring (MpscQueue) — a non-blocking enqueue, no encode,
// no send on the caller thread. A dedicated publisher thread owned by
// this object drains the ring, runs the live-order state machine,
// serializes each frame, and invokes the sink (UDP + journal). The ring
// is MPSC because the matching thread (A/E/D) and any control thread
// (S/R/H/Q) are independent producers; the single consumer is our
// worker thread, so the live-order map, tracking counter, and sink are
// only ever touched by that one thread — no locks on the hot path.
//
// Lifecycle contract: stop the event sources (detach this listener,
// quiesce control callers) BEFORE stopAsyncPublishing()/destruction, so
// no producer races the ring teardown. The worker drains anything still
// buffered on stop, so no captured event is lost.

#include "EventListener.h"
#include "ItchProtocol.h"
#include "MpscQueue.h"
#include "Order.h"
#include "OrderBook.h"
#include "Types.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

namespace OrderMatcher {

class ItchPublisher : public EventListener {
public:
    using SendBytes = std::function<void(std::string_view)>;
    using ClockFn   = std::function<uint64_t()>;

    ItchPublisher(OrderBook& book, SendBytes send)
        : book_(book), send_(std::move(send)) {}

    ~ItchPublisher() override { stopAsyncPublishing(); }

    ItchPublisher(const ItchPublisher&)            = delete;
    ItchPublisher& operator=(const ItchPublisher&) = delete;

    // Inject a clock for deterministic test assertions. Default uses
    // steady_clock::now() in nanoseconds. Real ITCH expects
    // nanoseconds since UTC midnight — the production caller wires
    // that timestamp source. The timestamp is captured on the caller
    // thread at event time (so async serialization preserves it); set
    // the clock before any traffic / before enableAsyncPublishing().
    void setClock(ClockFn clock) { clock_ = std::move(clock); }

    // ── P3-1: off-thread (decoupled) publishing ─────────────────────
    // Spin up the dedicated publisher thread and route every subsequent
    // event through the lock-free ring instead of serializing inline.
    // Idempotent. `ringCapacity` must be a power of two (MpscQueue
    // requirement); the default is generous enough that a keeping-up
    // consumer never drops under normal load.
    void enableAsyncPublishing(size_t ringCapacity = (size_t{1} << 16)) {
        if (async_.load(std::memory_order_acquire)) return;
        ring_ = std::make_unique<MpscQueue<RawEvent>>(ringCapacity);
        running_.store(true, std::memory_order_release);
        async_.store(true, std::memory_order_release);
        worker_ = std::thread(&ItchPublisher::publisherLoop, this);
    }

    // Stop the worker and revert to inline publishing. Drains whatever
    // is still queued (single-threaded once the worker is joined) so no
    // captured event is lost. Safe to call when async was never enabled.
    void stopAsyncPublishing() {
        if (!async_.load(std::memory_order_acquire)) return;
        running_.store(false, std::memory_order_release);
        if (worker_.joinable()) worker_.join();
        if (ring_) {
            RawEvent e;
            while (ring_->pop(e)) serializeAndSend(e);
            ring_.reset();
        }
        async_.store(false, std::memory_order_release);
    }

    bool     asyncEnabled()   const { return async_.load(std::memory_order_acquire); }
    // Events the matching/control thread could not enqueue because the
    // ring was full (consumer fell behind). Non-zero means market data
    // was dropped — size the ring larger or speed up the sink.
    uint64_t droppedEvents()  const { return droppedEvents_.load(std::memory_order_relaxed); }
    // Events drained + serialized by the worker (async) or inline (sync).
    uint64_t eventsProcessed() const { return eventsProcessed_.load(std::memory_order_relaxed); }

    // Emit a 'S' (SystemEvent) bookend manually. Subscribers expect
    // these at session boundaries (start/end of market, halts); the
    // publisher has no concept of "session" so it doesn't generate
    // these autonomously.
    void publishSystemEvent(char eventCode) {
        if (!send_) return;
        RawEvent e;
        e.kind = RawEvent::Kind::SystemEvent;
        e.c0   = eventCode;
        e.ts   = now();
        dispatch(e);
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
        RawEvent e;
        e.kind = RawEvent::Kind::StockDirectory;
        e.c0   = marketCategory;
        e.c1   = financialStatus;
        e.u32  = roundLotSize;
        e.ts   = now();
        dispatch(e);
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
    // halt, "    " for none). The wire trading-state is resolved from
    // the book on the CALLER thread (the book is only safely readable
    // there) and carried in the RawEvent.
    void publishTradingAction(const char reason4[4] = "    ") {
        if (!send_) return;
        RawEvent e;
        e.kind      = RawEvent::Kind::TradingAction;
        e.c0        = tradingStateToItch(book_.getTradingState());
        e.reason4[0] = reason4[0];
        e.reason4[1] = reason4[1];
        e.reason4[2] = reason4[2];
        e.reason4[3] = reason4[3];
        e.ts        = now();
        dispatch(e);
    }

    // Emit a 'Q' (Cross Trade) for an auction-uncross print.
    // Distinct from a continuous-matching trade — used when
    // reporting the aggregate volume and discovered price at the
    // open, close, or post-halt resumption.
    void publishCrossTrade(uint64_t shares, Price crossPrice,
                           uint64_t matchNumber, char crossType) {
        if (!send_) return;
        RawEvent e;
        e.kind         = RawEvent::Kind::CrossTrade;
        e.shares       = shares;
        e.price        = crossPrice;
        e.tradeOrMatch = matchNumber;
        e.c0           = crossType;
        e.ts           = now();
        dispatch(e);
    }

    uint64_t addsEmitted()              const { return addsEmitted_.load(std::memory_order_relaxed); }
    uint64_t executedEmitted()          const { return executedEmitted_.load(std::memory_order_relaxed); }
    uint64_t deletesEmitted()           const { return deletesEmitted_.load(std::memory_order_relaxed); }
    uint64_t tradingActionsEmitted()    const { return tradingActionsEmitted_.load(std::memory_order_relaxed); }
    uint64_t directoriesEmitted()       const { return directoriesEmitted_.load(std::memory_order_relaxed); }
    uint64_t crossTradesEmitted()       const { return crossTradesEmitted_.load(std::memory_order_relaxed); }
    uint64_t messagesEmitted()          const { return messagesEmitted_.load(std::memory_order_relaxed); }
    // NOTE: liveOrderCount() reflects worker-thread-owned state in async
    // mode; read it only after the ring has drained (tests quiesce first).
    size_t   liveOrderCount()           const { return liveOrders_.size(); }

    // ── EventListener overrides ─────────────────────────────────────────

    void onOrderUpdate(const OrderUpdate& u) override {
        if (!send_) return;
        switch (u.status) {
        case OrderStatus::Accepted: {
            // Public 'A' fires only when the order actually rests on
            // the book. An IOC that fully filled comes through as
            // Accepted with remaining=0 — no 'A' for that case.
            if (u.remainingQty == 0) break;
            // OrderUpdate doesn't carry side/price — fetch from the book
            // HERE, on the matching thread, while the order is still
            // resting. The worker thread must not touch the book (it may
            // have moved on), so we snapshot side/price into the event.
            const Order* o = book_.getOrder(u.orderId);
            if (!o) break;
            RawEvent e;
            e.kind    = RawEvent::Kind::Add;
            e.orderId = u.orderId;
            e.side    = o->side;
            e.shares  = u.remainingQty;
            e.price   = o->price;
            e.ts      = now();
            dispatch(e);
            break;
        }
        case OrderStatus::Cancelled:
        case OrderStatus::Filled: {
            RawEvent e;
            e.kind    = RawEvent::Kind::Delete;
            e.orderId = u.orderId;
            e.ts      = now();
            dispatch(e);
            break;
        }
        case OrderStatus::PartiallyFilled:
        case OrderStatus::Rejected:
        case OrderStatus::New:
            // No wire event — see header comment.
            break;
        }
    }

    void onTrade(const Trade& t) override {
        if (!send_) return;
        // Each side of the trade gets its own 'E' iff we previously
        // 'A'-announced that order. Liveness is resolved at serialize
        // time (the worker owns the live-order map), so both sides are
        // always enqueued; the state machine drops the ones we never
        // advertised. Order preserved: buy side then sell side.
        RawEvent buy;
        buy.kind         = RawEvent::Kind::Executed;
        buy.orderId      = t.buyOrderId;
        buy.shares       = t.quantity;
        buy.tradeOrMatch = t.tradeId;
        buy.ts           = now();
        dispatch(buy);

        RawEvent sell;
        sell.kind         = RawEvent::Kind::Executed;
        sell.orderId      = t.sellOrderId;
        sell.shares       = t.quantity;
        sell.tradeOrMatch = t.tradeId;
        sell.ts           = buy.ts;
        dispatch(sell);
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

    // POD event carried through the ring. One flat struct (no union) —
    // trivially copyable, ~56 bytes, which the MPSC queue copies by
    // value. Per-kind fields are documented inline.
    struct RawEvent {
        enum class Kind : uint8_t {
            Add, Executed, Delete,
            SystemEvent, StockDirectory, TradingAction, CrossTrade
        };
        Kind     kind{Kind::Add};
        Side     side{Side::Buy};    // Add
        char     c0{' '};            // SystemEvent code / StockDir marketCat /
                                     // TradingAction wireState / CrossTrade type
        char     c1{' '};            // StockDir financialStatus
        char     reason4[4]{' ', ' ', ' ', ' '};  // TradingAction reason
        uint32_t u32{0};             // StockDir roundLotSize
        OrderId  orderId{0};         // Add / Executed / Delete order ref
        Quantity shares{0};          // Add leaves / Executed trade qty /
                                     // CrossTrade shares
        Price    price{0};           // Add price / CrossTrade cross price
        uint64_t tradeOrMatch{0};    // Executed tradeId / CrossTrade matchNumber
        uint64_t ts{0};              // timestamp captured at event time
    };

    // Route an event: non-blocking ring push when decoupled, else inline.
    void dispatch(const RawEvent& e) {
        if (async_.load(std::memory_order_acquire)) {
            if (!ring_->push(e)) {
                droppedEvents_.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            serializeAndSend(e);
        }
    }

    // Worker loop (async mode): single consumer of the MPSC ring.
    void publisherLoop() {
        RawEvent e;
        while (running_.load(std::memory_order_acquire)) {
            if (ring_->pop(e)) {
                serializeAndSend(e);
            } else {
                std::this_thread::sleep_for(kIdleSleep);
            }
        }
    }

    // The only place the live-order map / tracking counter / sink are
    // touched. Runs on the caller thread in sync mode, on the worker
    // thread in async mode — never both concurrently.
    void serializeAndSend(const RawEvent& e) {
        switch (e.kind) {
        case RawEvent::Kind::Add: {
            liveOrders_[e.orderId] = LiveOrder{e.side, e.shares, e.price};
            uint8_t buf[ITCH_SIZE_ADD_ORDER];
            size_t n = encodeAddOrder(buf, locateOf(), nextTracking(), e.ts,
                                      static_cast<uint64_t>(e.orderId),
                                      e.side, e.shares, book_.getSymbolId(),
                                      e.price);
            emit(buf, n);
            addsEmitted_.fetch_add(1, std::memory_order_relaxed);
            messagesEmitted_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        case RawEvent::Kind::Executed: {
            auto it = liveOrders_.find(e.orderId);
            if (it == liveOrders_.end()) break;
            Quantity prev   = it->second.shares;
            Quantity filled = (e.shares > prev) ? prev : e.shares;
            it->second.shares = prev - filled;
            uint8_t buf[ITCH_SIZE_ORDER_EXECUTED];
            size_t n = encodeOrderExecuted(buf, locateOf(), nextTracking(),
                                           e.ts, static_cast<uint64_t>(e.orderId),
                                           filled, e.tradeOrMatch);
            emit(buf, n);
            executedEmitted_.fetch_add(1, std::memory_order_relaxed);
            messagesEmitted_.fetch_add(1, std::memory_order_relaxed);
            // Do NOT erase here — the subsequent Delete drives removal.
            break;
        }
        case RawEvent::Kind::Delete: {
            auto it = liveOrders_.find(e.orderId);
            if (it == liveOrders_.end()) break;
            uint8_t buf[ITCH_SIZE_ORDER_DELETE];
            size_t n = encodeOrderDelete(buf, locateOf(), nextTracking(),
                                         e.ts, static_cast<uint64_t>(e.orderId));
            emit(buf, n);
            deletesEmitted_.fetch_add(1, std::memory_order_relaxed);
            messagesEmitted_.fetch_add(1, std::memory_order_relaxed);
            liveOrders_.erase(it);
            break;
        }
        case RawEvent::Kind::SystemEvent: {
            uint8_t buf[ITCH_SIZE_SYSTEM_EVENT];
            size_t n = encodeSystemEvent(buf, locateOf(), nextTracking(),
                                         e.ts, e.c0);
            emit(buf, n);
            messagesEmitted_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        case RawEvent::Kind::StockDirectory: {
            uint8_t buf[ITCH_SIZE_STOCK_DIRECTORY];
            size_t n = encodeStockDirectory(buf, locateOf(), nextTracking(),
                                            e.ts, book_.getSymbolId(),
                                            e.c0, e.c1, e.u32, 'N', 'C');
            emit(buf, n);
            directoriesEmitted_.fetch_add(1, std::memory_order_relaxed);
            messagesEmitted_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        case RawEvent::Kind::TradingAction: {
            uint8_t buf[ITCH_SIZE_TRADING_ACTION];
            size_t n = encodeTradingAction(buf, locateOf(), nextTracking(),
                                           e.ts, book_.getSymbolId(),
                                           e.c0, e.reason4);
            emit(buf, n);
            tradingActionsEmitted_.fetch_add(1, std::memory_order_relaxed);
            messagesEmitted_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        case RawEvent::Kind::CrossTrade: {
            uint8_t buf[ITCH_SIZE_CROSS_TRADE];
            size_t n = encodeCrossTrade(buf, locateOf(), nextTracking(),
                                        e.ts, e.shares, book_.getSymbolId(),
                                        e.price, e.tradeOrMatch, e.c0);
            emit(buf, n);
            crossTradesEmitted_.fetch_add(1, std::memory_order_relaxed);
            messagesEmitted_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        }
        eventsProcessed_.fetch_add(1, std::memory_order_relaxed);
    }

    void emit(const uint8_t* buf, size_t n) {
        send_(std::string_view(reinterpret_cast<const char*>(buf), n));
    }

    uint16_t locateOf() const {
        return static_cast<uint16_t>(book_.getSymbolId());
    }

    uint16_t nextTracking() {
        // Per-publisher monotonic, wraps at 65536. Touched only by the
        // active serialize thread (caller in sync, worker in async), so
        // it needs no synchronization.
        return trackingCounter_++;
    }

    uint64_t now() const {
        if (clock_) return clock_();
        return static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
    }

    static constexpr std::chrono::microseconds kIdleSleep{50};

    OrderBook&                                   book_;
    SendBytes                                    send_;
    ClockFn                                      clock_;
    std::unordered_map<OrderId, LiveOrder>       liveOrders_;
    uint16_t                                     trackingCounter_{0};

    // Async decoupling state.
    std::unique_ptr<MpscQueue<RawEvent>>         ring_;
    std::thread                                  worker_;
    std::atomic<bool>                            async_{false};
    std::atomic<bool>                            running_{false};
    std::atomic<uint64_t>                        droppedEvents_{0};
    std::atomic<uint64_t>                        eventsProcessed_{0};

    // Counters are atomic so external observers (tests / metrics) can
    // read them while the worker thread writes.
    std::atomic<uint64_t>                        addsEmitted_{0};
    std::atomic<uint64_t>                        executedEmitted_{0};
    std::atomic<uint64_t>                        deletesEmitted_{0};
    std::atomic<uint64_t>                        tradingActionsEmitted_{0};
    std::atomic<uint64_t>                        directoriesEmitted_{0};
    std::atomic<uint64_t>                        crossTradesEmitted_{0};
    std::atomic<uint64_t>                        messagesEmitted_{0};
};

}  // namespace OrderMatcher
