#pragma once

// ItchPublisher — converts one OrderBook's engine events into ITCH 5.0 wire
// frames. Bound to a single OrderBook at construction (so it knows its symbol
// and can look up resting orders for the side/price the EventListener path
// doesn't surface); fan out via MultiplexListener for a full multi-symbol feed.
// Sink `void(std::string_view)` is transport-agnostic: tests accumulate to a
// string, production wires to UDP multicast or the SHM fan-out.
//
// Mapping (per ITCH 5.0):
//   onBookVisible(Rest), id unknown       → 'A' AddOrder
//   onBookVisible(Rest), id already live  → 'D' then 'A' (iceberg slice refresh:
//                                            the new slice is a new arrival at
//                                            the back of the queue)
//   onBookVisible(Reduce)                 → 'X' OrderCancel (shares removed)
//   onTrade                               → 'E' Executed (per live side)
//   onOrderUpdate(Filled, remaining=0)    → 'D' Delete (order off the book)
//   onOrderUpdate(Cancelled)              → 'D' Delete
//   onOrderUpdate(Rejected)               → nothing (rejects are private)
//   onOrderUpdate(PartiallyFilled)        → nothing ('E' was the public event)
//
// Adds are driven by onBookVisible, NOT onOrderUpdate(Accepted). Accepted is
// the private owner ack: it fires for hidden orders, reports an iceberg's true
// size rather than its slice, and fires before matching — so a feed built on it
// broadcasts the full incoming size of an order that may never rest. Publishing
// from the displayed-book channel instead means hidden and reserve quantity
// never reach the wire at all.
//
// Live order IDs are tracked internally so 'D' only follows a prior 'A', and
// 'E' only fires for sides we publicly announced. That same rule is what keeps
// hidden orders off the feed end-to-end: they are never 'A'-announced, so their
// executions and deletes find no tracked id and are dropped.
//
// Threading (P3-1): synchronous by default — engine calls serialize the frame
// and invoke the sink inline on the matching thread. enableAsyncPublishing()
// decouples it: engine-facing calls only push a small POD RawEvent onto a
// lock-free MPSC ring (MpscQueue) and a dedicated worker thread drains it, runs
// the live-order state machine, serializes, and invokes the sink. MPSC because
// the matching thread (A/E/D) and control thread (S/R/H/Q) are independent
// producers; the single consumer keeps the live-order map lock-free.
// Lifecycle: quiesce all event sources BEFORE stopAsyncPublishing()/destruction
// so no producer races ring teardown; the worker drains buffered events on stop.

#include "EventListener.h"
#include "ItchProtocol.h"
#include "MpscQueue.h"
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
    uint64_t cancelsEmitted()           const { return cancelsEmitted_.load(std::memory_order_relaxed); }
    uint64_t deletesEmitted()           const { return deletesEmitted_.load(std::memory_order_relaxed); }
    uint64_t tradingActionsEmitted()    const { return tradingActionsEmitted_.load(std::memory_order_relaxed); }
    uint64_t directoriesEmitted()       const { return directoriesEmitted_.load(std::memory_order_relaxed); }
    uint64_t crossTradesEmitted()       const { return crossTradesEmitted_.load(std::memory_order_relaxed); }
    uint64_t messagesEmitted()          const { return messagesEmitted_.load(std::memory_order_relaxed); }
    // NOTE: liveOrderCount() reflects worker-thread-owned state in async
    // mode; read it only after the ring has drained (tests quiesce first).
    size_t   liveOrderCount()           const { return liveOrders_.size(); }

    // ── EventListener overrides ─────────────────────────────────────────

    void onBookVisible(const BookVisibleUpdate& u) override {
        if (!send_) return;
        RawEvent e;
        e.kind    = (u.action == BookVisibleUpdate::Action::Rest)
                        ? RawEvent::Kind::Add
                        : RawEvent::Kind::Cancel;
        e.orderId = u.orderId;
        e.side    = u.side;
        e.shares  = u.quantity;
        e.price   = u.price;
        e.ts      = now();
        dispatch(e);
    }

    void onOrderUpdate(const OrderUpdate& u) override {
        if (!send_) return;
        switch (u.status) {
        case OrderStatus::Accepted:
            // Not a public event — see the header comment. The 'A' comes from
            // onBookVisible(Rest), which knows the displayed size.
            break;
        case OrderStatus::Cancelled:
        case OrderStatus::Filled: {
            RawEvent e;
            e.kind     = RawEvent::Kind::Delete;
            e.orderId  = u.orderId;
            // A Filled delete is preceded/followed by an Executed that carries
            // the maker side; a Cancel has no trade. The serializer needs this
            // to know whether an 'E' is still pending for this order.
            e.fromFill = (u.status == OrderStatus::Filled);
            e.ts       = now();
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
        Quantity shares;            // current leaves (decremented on fills)
        Price    price;
        // A fully-filled resting order's Delete (from notifyOrderUpdate(Filled))
        // can be dispatched BEFORE its Executed: OrderBook batches onTrade to
        // AFTER the match loop (P1-3) while the Filled notify stays inline in the
        // loop, so the publisher sees D-then-E for the maker side. We must not
        // tear the order down before its 'E' fires. When that happens we stash
        // the delete here (no emit, no erase) and let the pending Executed emit
        // 'E' first, then flush this 'D' — restoring correct E→D wire order.
        bool     pendingDelete{false};
        uint64_t pendingDeleteTs{0};  // original delete event timestamp
    };

    // POD event carried through the ring. One flat struct (no union) —
    // trivially copyable, ~56 bytes, which the MPSC queue copies by
    // value. Per-kind fields are documented inline.
    struct RawEvent {
        enum class Kind : uint8_t {
            Add, Executed, Cancel, Delete,
            SystemEvent, StockDirectory, TradingAction, CrossTrade
        };
        Kind     kind{Kind::Add};
        Side     side{Side::Buy};    // Add
        bool     fromFill{false};    // Delete: true iff driven by Filled (vs Cancel)
        char     c0{' '};            // SystemEvent code / StockDir marketCat /
                                     // TradingAction wireState / CrossTrade type
        char     c1{' '};            // StockDir financialStatus
        char     reason4[4]{' ', ' ', ' ', ' '};  // TradingAction reason
        uint32_t u32{0};             // StockDir roundLotSize
        OrderId  orderId{0};         // Add / Executed / Cancel / Delete order ref
        Quantity shares{0};          // Add displayed qty / Executed trade qty /
                                     // Cancel shares removed / CrossTrade shares
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
            // An id we have already announced means an iceberg slice refresh.
            // The replenished slice joins the back of the queue, which on the
            // wire is a delete of the exhausted slice then a fresh add — the
            // standard encoding for an order that lost its time priority.
            if (liveOrders_.count(e.orderId)) emitOrderDelete(e.orderId, e.ts);
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
            // If a Filled delete for this order was stashed ahead of this 'E'
            // (batched-dispatch reorder), the order is now fully executed —
            // flush the deferred 'D' so the wire reads E→D, then erase.
            if (it->second.pendingDelete && it->second.shares == 0) {
                emitOrderDelete(e.orderId, it->second.pendingDeleteTs);
                liveOrders_.erase(it);
            }
            break;
        }
        case RawEvent::Kind::Cancel: {
            // Displayed size shrank with no trade behind it. Untracked ids are
            // dropped for the same reason as Executed — we never announced it.
            auto it = liveOrders_.find(e.orderId);
            if (it == liveOrders_.end()) break;
            Quantity removed = (e.shares > it->second.shares) ? it->second.shares
                                                              : e.shares;
            if (removed == 0) break;
            it->second.shares -= removed;
            uint8_t buf[ITCH_SIZE_ORDER_CANCEL];
            size_t n = encodeOrderCancel(buf, locateOf(), nextTracking(), e.ts,
                                         static_cast<uint64_t>(e.orderId), removed);
            emit(buf, n);
            cancelsEmitted_.fetch_add(1, std::memory_order_relaxed);
            messagesEmitted_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        case RawEvent::Kind::Delete: {
            auto it = liveOrders_.find(e.orderId);
            if (it == liveOrders_.end()) break;
            // A Filled delete whose Executed hasn't arrived yet (leaves still
            // > 0) must wait: emitting 'D' now would drop the maker's 'E'
            // (the state machine only emits 'E' for tracked orders). Stash it;
            // the pending Executed above will flush it. Cancels and already-
            // executed fills (leaves == 0) delete immediately.
            if (e.fromFill && it->second.shares > 0) {
                it->second.pendingDelete   = true;
                it->second.pendingDeleteTs = e.ts;
                break;
            }
            emitOrderDelete(e.orderId, e.ts);
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

    // Encode + send an Order Delete and bump counters. Does NOT touch
    // liveOrders_ — the caller owns the erase (it may hold an iterator).
    void emitOrderDelete(OrderId orderId, uint64_t ts) {
        uint8_t buf[ITCH_SIZE_ORDER_DELETE];
        size_t n = encodeOrderDelete(buf, locateOf(), nextTracking(), ts,
                                     static_cast<uint64_t>(orderId));
        emit(buf, n);
        deletesEmitted_.fetch_add(1, std::memory_order_relaxed);
        messagesEmitted_.fetch_add(1, std::memory_order_relaxed);
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
    std::atomic<uint64_t>                        cancelsEmitted_{0};
    std::atomic<uint64_t>                        deletesEmitted_{0};
    std::atomic<uint64_t>                        tradingActionsEmitted_{0};
    std::atomic<uint64_t>                        directoriesEmitted_{0};
    std::atomic<uint64_t>                        crossTradesEmitted_{0};
    std::atomic<uint64_t>                        messagesEmitted_{0};
};

}  // namespace OrderMatcher
