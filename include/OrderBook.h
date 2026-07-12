#pragma once

#include "Order.h"
#include "IntrusiveList.h"
#include "MemoryPool.h"
#include "FlatPriceMap.h"
#include "FlatHashMap.h"
#include "EventListener.h"
#include "RingBuffer.h"
#include "SelfTradeProtection.h"
#include "LULDManager.h"
#include "WashTradeDetector.h"
#include "BatchRiskValidator.h"
#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <variant>
#include <cstdint>
#include <limits>

namespace OrderMatcher {

class Gauge;  // Metrics.h — pointer only; avoids pulling Metrics into this header

struct Trade {
    uint64_t tradeId;
    OrderId buyOrderId;
    OrderId sellOrderId;
    ParticipantId buyerId;
    ParticipantId sellerId;
    Price price;
    Quantity quantity;
    uint64_t timestamp;
    uint64_t sequenceNumber;
    SymbolId symbolId = 0;
    // Side of the aggressing (taker) order; the resting order is the maker.
    // Set for continuous-match trades; auction-cross trades keep the default
    // (a call auction has no single aggressor — fees treat the buyer as taker).
    Side aggressorSide = Side::Buy;
};

// A single continuous-match fill, buffered on the stack inside OrderBook::match
// so the per-fill onTrade dispatch and trade-fill audit logging can be flushed
// in ONE batch after the matching loop instead of firing interleaved with book
// mutation on every fill. Wraps the fully-formed Trade — every downstream sink
// (listeners + structured audit log) is derived from it, so no other per-fill
// state needs buffering.
struct FillEvent {
    Trade trade;
};

// Market Data: single price level
struct PriceLevel {
    Price price;
    Quantity totalQuantity;
    uint32_t orderCount;
};

// Market Data: L2 Depth Snapshot
struct MarketDataSnapshot {
    SymbolId symbolId;
    // Fixed-size arrays instead of vectors — no allocation
    static constexpr size_t MAX_DEPTH = 20;
    PriceLevel bids[MAX_DEPTH];
    PriceLevel asks[MAX_DEPTH];
    size_t bidCount = 0;
    size_t askCount = 0;
    Price lastTradePrice;
    Quantity lastTradeQty;
    uint64_t timestamp;
};

// Market Data: incremental update
struct MarketDataUpdate {
    enum class Action : uint8_t { Add, Modify, Delete };
    Action action;
    Side side;
    PriceLevel level;
    uint64_t timestamp;
    uint64_t sequenceNumber;
};

// Auction indicative / imbalance snapshot (NASDAQ NOII analogue).
// Computed non-destructively over the resting book plus any parked
// auction market orders, so it can be published continuously during
// PreOpen / AuctionClose / VolatilityAuction without executing trades.
struct AuctionResult {
    bool     hasCross = false;           // a positive volume would cross
    Price    indicativePrice = 0;        // clearing price (0 if !hasCross)
    Quantity pairedVolume = 0;           // matched qty at the clearing price
    Quantity imbalanceQty = 0;           // unmatched qty (at the cross, else book-wide)
    Side     imbalanceSide = Side::Buy;  // surplus side (meaningful iff imbalanceQty > 0)
};

// Order status notification
struct OrderUpdate {
    OrderId orderId;
    OrderStatus status;
    Quantity filledQty;
    Quantity remainingQty;
    Price lastFillPrice;
    RejectReason rejectReason = RejectReason::None;
    uint64_t timestamp;
    uint64_t sequenceNumber;
};

// Pre-trade risk limits per participant
struct RiskLimits {
    Quantity maxOrderSize = 0;       // 0 = no limit
    Price maxOrderNotional = 0;      // 0 = no limit
    Quantity maxPositionSize = 0;    // 0 = no limit
};

// Result type for addOrder: holds the OrderId on success, or a RejectReason on failure
using AddOrderResult = std::variant<OrderId, RejectReason>;

struct ParticipantStats {
    uint64_t ordersSubmitted = 0;
    uint64_t tradesExecuted = 0;
    uint64_t rejectedOrders = 0;
    int64_t netPosition = 0;

    double getOTR() const {
        return static_cast<double>(ordersSubmitted) / std::max(static_cast<uint64_t>(1), tradesExecuted);
    }
};

// Pre-allocated fixed-size array for tracking special order types
// (stop, trailing stop, pegged). Avoids std::vector heap allocation.
template <typename T, size_t MaxSize = 65536>
class FixedVector {
public:
    FixedVector() : size_(0) {}

    bool push_back(const T& val) {
        if (size_ >= MaxSize) [[unlikely]] return false;
        data_[size_++] = val;
        return true;
    }

    void erase_swap(size_t idx) {
        if (idx < size_) {
            data_[idx] = data_[size_ - 1];
            size_--;
        }
    }

    // Erase by value (swap with last)
    bool erase_value(const T& val) {
        for (size_t i = 0; i < size_; i++) {
            if (data_[i] == val) {
                erase_swap(i);
                return true;
            }
        }
        return false;
    }

    T& operator[](size_t i) { return data_[i]; }
    const T& operator[](size_t i) const { return data_[i]; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    T* begin() { return data_; }
    T* end() { return data_ + size_; }
    const T* begin() const { return data_; }
    const T* end() const { return data_ + size_; }
    void clear() { size_ = 0; }

private:
    T data_[MaxSize];
    size_t size_;
};

class OrderBook {
public:
    // orderPoolCapacity: number of Order slots to pre-allocate. 0 = engine
    // default (INITIAL_CAPACITY). A smaller pool is used by pool-pressure tests
    // to exercise the degradation thresholds without allocating 200k orders.
    explicit OrderBook(SymbolId symbolId = 0, MatchAlgorithm algo = MatchAlgorithm::PriceTime,
                       size_t orderPoolCapacity = 0);

    // Core Actions
    AddOrderResult addOrder(OrderId orderId, ParticipantId participantId, Side side, Price price, Quantity qty, OrderType type,
                  Price stopPrice = 0, Quantity displayQty = 0,
                  TimeInForce tif = TimeInForce::GTC, uint64_t expiryTime = 0,
                  Price stopLimitPrice = 0,
                  PegType pegType = PegType::None, Price pegOffset = 0,
                  Price trailAmount = 0, Quantity minQty = 0, bool hidden = false,
                  bool riskChecksBypassed = false);
    void cancelOrder(OrderId orderId);
    bool modifyOrder(OrderId orderId, Quantity newQty);

    // Cancel/Replace: full amendment (price change loses time priority)
    bool cancelReplace(OrderId orderId, Price newPrice, Quantity newQty);

    // Kill switch: cancel all orders for a participant
    uint64_t cancelAllForParticipant(ParticipantId participantId);

    // Auction
    void uncross();

    // Non-destructive auction indicative + imbalance over the current
    // book (NASDAQ NOII analogue). Shares the price-discovery core with
    // uncross() so the published indicative cannot diverge from the price
    // the cross will actually use. Takes the book's shared lock.
    AuctionResult computeAuctionState() const;

    // Reopen a volatility auction: run the reopening cross at the
    // discovered clearing price and return the book to continuous
    // trading, re-anchoring the volatility reference to the reopening
    // print. No-op returning false unless the book is currently in
    // TradingState::VolatilityAuction. A manual Halt is unaffected.
    bool resumeVolatilityAuction();

    // Time-based expiry: call periodically to expire GTD/DAY orders
    // Cancel any DAY/GTD orders whose expiryTime has elapsed. The
    // optional onExpire callback is invoked once per order BEFORE the
    // cancel runs, with the order's id as the only argument. The engine
    // uses this to journal each expiration as a normal CancelOrder
    // entry, so replay reproduces expirations deterministically without
    // needing a virtual clock.
    void expireOrders(uint64_t currentTime,
                      const std::function<void(OrderId)>& onExpire = {});

    // Market Data
    MarketDataSnapshot getSnapshot(size_t depth = 10) const;

    // Risk Management
    void setRiskLimits(ParticipantId participantId, const RiskLimits& limits);

    // ─── LMM/DMM Market-Maker Role ───────────────────────────────────
    void setParticipantRole(ParticipantId p, ParticipantRole role) {
        participantRoles_.insert(p, role);
    }
    ParticipantRole getParticipantRole(ParticipantId p) const {
        const auto* r = participantRoles_.find(p);
        return r ? *r : ParticipantRole::Regular;
    }

    // ─── Self-Trade Prevention (Phase 4, Week 13) ────────────────────
    void setSTPMode(ParticipantId participant, STPMode mode) {
        stpModes_.insert(participant, mode);
    }
    STPMode getSTPMode(ParticipantId participant) const {
        auto* m = stpModes_.find(participant);
        return m ? *m : STPMode::None;
    }

    // ─── LULD Volatility Controls (Phase 4, Week 14) ─────────────────
    LULDManager& getLULDManager() { return luld_; }
    const LULDManager& getLULDManager() const { return luld_; }

    // ─── Wash Trade Detection (Phase 4, Week 13) ─────────────────────
    WashTradeDetector& getWashTradeDetector() { return washTradeDetector_; }
    const WashTradeDetector& getWashTradeDetector() const { return washTradeDetector_; }

    // Analytics
    double getVWAP() const { return vwap_; }
    uint64_t getTradeCount() const { return priceUpdates_; }

    // Trade history ring buffer access.
    //
    // CONCURRENCY (SPSC contract): tradeHistory_ is written (push) only by the
    // single worker thread that owns this book, under bookLock_. This accessor
    // returns a CONST reference precisely so a reader on another thread cannot
    // call push()/pop() (both non-const) and break the single-producer/single-
    // consumer invariant. The const observers that remain reachable — size(),
    // empty(), capacity() — touch only the release/acquire atomics head_/tail_,
    // never the buffer storage, so calling them concurrently with the producer's
    // push() is data-race-free. Do NOT add a non-const overload or a path to
    // pop() from a second consumer.
    const RingBuffer<Trade>& getTradeRingBuffer() const { return tradeHistory_; }
    void clearTradeHistory() { /* ring buffer wraps automatically */ }

    void resetStatus() {
        tradingState_ = TradingState::Continuous;
        referencePrice_ = 0;
        priceUpdates_ = 0;
    }

    // Trading state machine (regulator/engine controls; participants observe).
    // Set the trading state. Out-of-line so the structured-event sink
    // can record the transition without leaking the StructuredLog
    // include into this header's downstream consumers.
    void setTradingState(TradingState s);
    TradingState getTradingState() const { return tradingState_; }
    // Convenience: legacy callers checking the binary halt flag still work.
    // True iff the book is currently in TradingState::Halted.

    // Configurable circuit breaker
    void setCircuitBreakerThreshold(double threshold) { cbThreshold_ = threshold; }
    double getCircuitBreakerThreshold() const { return cbThreshold_; }
    void setReferencePrice(Price price) { referencePrice_ = price; }

    // Price-band (LULD-style) admission filter. Distinct from the
    // volatility circuit breaker: the breaker HALTS the market when a
    // single price moves too far; the band REJECTS individual orders
    // priced outside [ref*(1-pct), ref*(1+pct)] without affecting market
    // state. Both can be active simultaneously.
    //
    // Argument: pct is the fractional half-width (0.05 = ±5%). Pass 0 to
    // disable. Reference price is `referencePrice_`, the same value the
    // breaker uses; if no reference has been seen yet, the band is not
    // applied (orders are admitted unconditionally so the first trade can
    // establish a reference).
    void setPriceBandPct(double pct) { priceBandPct_ = pct < 0 ? 0 : pct; }
    double getPriceBandPct() const   { return priceBandPct_; }

    // Market-order sweep protection: cap how far a market order may walk
    // from the arrival touch price (fractional half-width, e.g. 0.10 = 10%).
    // 0 disables. The portion of a market order that would fill beyond the
    // collar is left unfilled (and cancelled), so a single sweep cannot
    // clear the book at runaway prices.
    void setMarketProtectionPct(double pct) { marketProtectionPct_ = pct < 0 ? 0 : pct; }
    double getMarketProtectionPct() const   { return marketProtectionPct_; }
    ParticipantStats getParticipantStats(ParticipantId id) const {
        auto* state = participantRisk_.find(OTRKey(id, symbolId_));
        if (state) {
            ParticipantStats stats;
            stats.ordersSubmitted = state->ordersSubmitted;
            stats.tradesExecuted = state->tradesExecuted;
            stats.rejectedOrders = state->rejectedOrders;
            stats.netPosition = state->currentExposure;
            return stats;
        }
        return ParticipantStats{};
    }

    const ParticipantRiskState* getParticipantRiskState(ParticipantId id) const {
        return participantRisk_.find(OTRKey(id, symbolId_));
    }

    // Depth limit: max price levels per side (0 = unlimited)
    void setMaxDepth(size_t maxLevels) { maxDepthPerSide_ = maxLevels; }
    size_t getMaxDepth() const { return maxDepthPerSide_; }

    // Get all active orders (for snapshots/inspection)
    // Note: returns up to maxOrders pointers into a caller-provided buffer
    size_t getAllOrders(const Order** out, size_t maxOrders) const;

    // Iterate all active orders (for checkpoint — avoids allocation).
    // CALLER MUST hold bookLock_ (or otherwise guarantee no concurrent writer)
    // — this overload does NOT lock. Safe from the owning worker thread; use
    // forEachOrderLocked() from any other thread.
    template<typename Fn>
    void forEachOrder(Fn&& fn) const {
        orderLookup_.forEach([&](OrderId, Order* order) {
            if (order) fn(*order);
        });
    }

    // Same as forEachOrder, but acquires bookLock_ for the whole iteration so
    // it is safe to call from a thread that does NOT own the book — snapshot
    // streaming, checkpoint, replication. Writers take the lock exclusively, so
    // orderLookup_ cannot be mutated/rehashed mid-walk. The callback must not
    // re-enter a method that takes bookLock_ (no re-entrancy on the non-
    // recursive mutex).
    template<typename Fn>
    void forEachOrderLocked(Fn&& fn) const {
        std::lock_guard<std::mutex> lock(bookLock_);
        orderLookup_.forEach([&](OrderId, Order* order) {
            if (order) fn(*order);
        });
    }

    // Getters
    const Order* getOrder(OrderId orderId) const;
    size_t getBidLevelsCount() const;
    size_t getAskLevelsCount() const;
    SymbolId getSymbolId() const { return symbolId_; }
    bool isHalted() const { return tradingState_ == TradingState::Halted; }

    // Event listener (replaces std::function callbacks)
    void setEventListener(EventListener* listener) {
        listener_ = listener ? listener : &nullListener();
        refreshTradeListenerFlag();
    }

    // True iff a real (non-null) trade listener is registered. The fill hot
    // path consults this to skip the per-fill onTrade vtable dispatch entirely
    // when no listener is wired (see match()/matchProRata()).
    bool hasTradeListener() const { return hasTradeListener_; }

    // Secondary, engine-owned listener for internal observation (e.g. OCO
    // contingent-order tracking). Kept separate from the user listener slot
    // above so installing one does not clobber a gateway/OUCH market-data
    // listener. Receives the same order-update events.
    void setEngineListener(EventListener* listener) {
        engineListener_ = listener ? listener : &nullListener();
        refreshTradeListenerFlag();
    }

    // Emit a rejection notification for an order the engine declined before
    // it ever reached the book (e.g. a hierarchical pre-trade risk breach).
    // Touches no book state; just notifies listeners so the reject is
    // observable on the same path as in-book rejects.
    void emitReject(OrderId orderId, Quantity qty, RejectReason reason) {
        notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, qty, 0, reason);
    }

    // Replay mode: suppresses all callbacks during journal replay
    void setReplayMode(bool replay) { replayMode_ = replay; }
    bool isReplayMode() const { return replayMode_; }

    Price getBestBid() const;
    Price getBestAsk() const;
    Price getMidPrice() const;

    double getOTR(ParticipantId p) const {
        auto* state = participantRisk_.find(OTRKey(p, symbolId_));
        return state ? state->getOTR() : 0.0;
    }

    void setMatchAlgorithm(MatchAlgorithm algo) { matchAlgorithm_ = algo; }

    // ─── P3-8 Order-pool utilization / controlled degradation ─────────
    // Fixed-size Order pool. When it fills, new orders are shed (rejected)
    // BEFORE true exhaustion so resting orders can still be cancelled/matched.
    size_t poolCapacity() const { return orderPool_.capacity(); }
    size_t poolInUse() const { return orderPool_.capacity() - orderPool_.available(); }
    double poolUtilization() const {
        const size_t cap = orderPool_.capacity();
        return cap ? static_cast<double>(cap - orderPool_.available()) /
                         static_cast<double>(cap)
                   : 0.0;
    }
    // Count of new orders shed due to pool pressure (RejectReason::PoolCapacityExceeded).
    uint64_t getPoolRejectCount() const { return poolRejects_; }

    // Current count of orders participant `pid` has resting in this book — the
    // O(1) value backing the self-trade pre-check. Exposed for tests/telemetry;
    // a parity test asserts it equals a ground-truth scan of bids_/asks_.
    uint32_t stpRestingCount(ParticipantId pid) const {
        return pid < kStpMaxParticipants ? stpResting_[pid] : 0;
    }

    // Test-only ground truth: count orders for `pid` ACTUALLY resting in
    // bids_/asks_ by scanning. The O(1) counter (stpRestingCount) must always
    // equal this for any tracked id — the StpOccupancyTest parity assertion.
    uint32_t debugScanRestingCount(ParticipantId pid) const {
        uint32_t n = 0;
        auto count = [&](Price, const OrderList& level) {
            for (const Order* o = level.front(); o; o = o->next)
                if (o->participantId == pid) ++n;
        };
        bids_.forEachLevel(count);
        asks_.forEachLevel(count);
        return n;
    }

    // Degradation thresholds (fractions of pool capacity).
    static constexpr double kPoolWarnPct   = 0.80;  // warn / observe
    static constexpr double kPoolRejectPct = 0.95;  // reject new orders

private:
    // Update the utilization gauge and, on an upward band crossing, warn (≥80%),
    // note the reject band (≥95%), or raise a critical alert (100%). Cheap:
    // one gauge store + a couple of integer compares. Called on the book's
    // owning worker thread (the addOrder path).
    void updatePoolUtilization(size_t inUse, size_t cap);

    // Internal cancel that does NOT take bookLock_. Public cancelOrder
    // wraps this with a unique_lock; callers that already hold the lock
    // (expireOrders, cancelAllForParticipant) call this directly to
    // avoid self-deadlock on the non-recursive mutex.
    void cancelOrderImpl(OrderId orderId);

    // Release parked MOC/LOC orders into the book when AuctionClose begins.
    void releaseOnCloseOrders();
    // Cancel any LOC orders remaining in the book after uncross completes.
    void cancelLocOrders();

    // Stack-buffer capacity for the batched per-fill work in match() (onTrade
    // dispatch, trade-fill audit logging, deferred lookup erases). A single
    // aggressive order that produces more fills than this is handled by
    // flush-and-continue inside the loop — the batch is drained and reused, so
    // no fill is ever truncated regardless of sweep depth.
    static constexpr int kMaxFillsPerOrder = 256;

    void match(Order* order);
    void matchProRata(Order* order);
    bool checkSMP(const Order& incoming, const Order& resting) const;
    bool checkLiquidity(Side side, Price price, Quantity qty, OrderType type) const;
    bool checkMinQty(Side side, Price price, Quantity minQty) const;

    void checkStopOrders(Price lastTradePrice);
    void updateTrailingStops(Price lastTradePrice);
    void updatePeggedOrders();
    // Cancel any market orders parked during an auction without
    // executing them (no candidate price, no counterparties, etc.).
    void cancelAuctionMarketOrders();

    // Shared auction price-discovery core. Assumes the caller already
    // holds bookLock_ (uncross() holds it unique; computeAuctionState()
    // holds it shared). Pure read — never mutates the book. Applies the
    // full venue tie-break cascade: max executable volume → min imbalance
    // → min distance to reference price → market-pressure side.
    AuctionResult discoverUncrossPrice() const;
    void updateAnalytics(Price price, Quantity qty, ParticipantId p1 = 0, ParticipantId p2 = 0);
    bool checkCircuitBreaker(Price price);
    bool checkRiskLimits(ParticipantId participantId, Price price, Quantity qty);

    void notifyOrderUpdate(OrderId orderId, OrderStatus status, Quantity filledQty, Quantity remainingQty,
                           Price lastFillPrice = 0, RejectReason reason = RejectReason::None);
    void notifyMarketData(MarketDataUpdate::Action action, Side side, Price price);

    void removeFromBook(Order* order);
    bool addToBook(Order* order);  // returns false if depth limit exceeded
    bool canAddToBook(const Order* order) const;
    void ensurePriceRange(Price price);

    // ── Self-trade-protection occupancy (P1-2) ───────────────────────────────
    // Per-participant count of orders currently RESTING in THIS book, so the
    // pre-match self-trade check is an O(1) array lookup instead of the former
    // O(depth) scan of every crossable level. Maintained via Order::inBook:
    // stpNoteAdded on book entry (addToBook), stpNoteRemoved on every book exit
    // (cancel / fill / STP removal / expiry / uncross). Non-idempotent add +
    // idempotent remove: a missed removal site can only OVERcount -> the STP path
    // runs when it need not (safe, slower), never UNDERcount -> a self-trade slips
    // through (unsafe). Ids >= kStpMaxParticipants are untracked; stpNoneResting
    // returns false for them so they take the full STP path (conservative).
    static constexpr size_t kStpMaxParticipants = 1024;

    void stpNoteAdded(Order* order) {
        order->inBook = true;
        const ParticipantId pid = order->participantId;
        if (pid < kStpMaxParticipants) ++stpResting_[pid];
    }
    void stpNoteRemoved(Order* order) {
        if (!order->inBook) return;  // parked / never rested / already removed
        order->inBook = false;
        const ParticipantId pid = order->participantId;
        if (pid < kStpMaxParticipants && stpResting_[pid] > 0) --stpResting_[pid];
    }
    // True iff this participant has NO order a fill could self-match against.
    // Out-of-range ids conservatively return false (run the full STP path).
    bool stpNoneResting(ParticipantId pid) const {
        return pid < kStpMaxParticipants && stpResting_[pid] == 0;
    }

    // Mutex protecting the book's mutable state. Writers (addOrder,
    // cancelOrder, modifyOrder, cancelReplace, expireOrders,
    // cancelAllForParticipant, uncross) and the snapshot/auction readers
    // (getSnapshot, computeAuctionState) all take an exclusive lock. Without
    // this, a snapshot read could observe a side-flipping cancelReplace
    // mid-flight and return a torn view (an order on both sides) — exactly
    // the scenario the Snapshot.tla spec produced as a counterexample.
    //
    // A plain std::mutex (not shared_mutex) because the engine's
    // single-writer-per-book worker-thread model has no writer-vs-writer
    // contention, and snapshot reads are rare and off the hot path — so
    // concurrent-reader throughput is not worth the 3-5x more expensive
    // uncontended write-lock of a shared_mutex. The exclusive read-lock only
    // serializes the (infrequent) reader against the writer, which the
    // shared_mutex already did anyway.
    mutable std::mutex bookLock_;

    // Bids: Descending Price (flat array, O(1) best-bid)
    FlatPriceMap bids_;
    // Asks: Ascending Price (flat array, O(1) best-ask)
    FlatPriceMap asks_;

    // STP occupancy counter (see stpNoteAdded/stpNoteRemoved). Zero-initialized;
    // uint32 x 1024 = 4 KiB per book. Indexed by participant id (< bound).
    uint32_t stpResting_[kStpMaxParticipants]{};

    // Pending orders by type — pre-allocated fixed vectors (no heap allocation).
    // Memory note: each `FixedVector<Order*, 16384>` is 128 KiB; the four
    // members below total ~512 KiB per OrderBook regardless of utilization.
    // Acceptable for a few dozen symbols; if scaling to a large universe,
    // consider chunked / lazy-allocated alternatives.
    FixedVector<Order*, 16384> stopOrders_;
    FixedVector<Order*, 16384> trailingStopOrders_;
    FixedVector<Order*, 16384> peggedOrders_;
    // Market orders submitted during PreOpen / AuctionOpen / AuctionClose.
    // Parked here instead of in the FlatPriceMap (they have no limit
    // price). At uncross time they are inserted at the discovered uncross
    // price and execute against limit liquidity; any remainder is
    // cancelled at the end of the uncross.
    FixedVector<Order*, 16384> auctionMarketOrders_;
    // MOC/LOC orders parked before AuctionClose. Released when the book
    // transitions to AuctionClose: MOC→auctionMarketOrders_, LOC→limit book.
    FixedVector<Order*, 16384> onCloseOrders_;
    // OrderIds of LOC orders released into the limit book during AuctionClose;
    // cancelled after uncross() completes.
    FixedVector<OrderId, 16384> locActiveIds_;

    // O(1) Lookup — open-addressing hash map (replaces std::unordered_map)
    FlatHashMap<OrderId, Order*> orderLookup_;
    ObjectPool<Order> orderPool_;

    // ─── P3-8 pool-pressure state (owning worker thread only) ─────────
    // Per-symbol utilization gauge in the global MetricsRegistry (set in ctor).
    Gauge* poolGauge_ = nullptr;
    // Last-observed pressure band (0 normal, 1 warn ≥80%, 2 reject ≥95%,
    // 3 exhausted 100%). Used to log/alert only on upward transitions.
    int poolPressureBand_ = 0;
    // New orders shed due to pool pressure.
    uint64_t poolRejects_ = 0;

    // Event listener (replaces std::function callbacks — zero-cost vtable dispatch)
    EventListener* listener_ = &nullListener();
    // Secondary engine-internal listener (OCO tracking); see setEngineListener.
    EventListener* engineListener_ = &nullListener();

    // Cached: true iff either listener slot holds a real (non-null) listener.
    // Lets the fill hot path skip the onTrade vtable dispatch when nothing is
    // wired. Refreshed by setEventListener/setEngineListener.
    bool hasTradeListener_ = false;
    void refreshTradeListenerFlag() {
        hasTradeListener_ = (listener_ != &nullListener()) ||
                            (engineListener_ != &nullListener());
    }

    // Replay mode flag — suppresses callbacks during journal recovery
    bool replayMode_{false};

    // Identity
    SymbolId symbolId_;
    MatchAlgorithm matchAlgorithm_;

    uint64_t nextTradeId_{1};
    Price lastTradePrice_{0};
    Quantity lastTradeQty_{0};
    // Atomic so a future lock-free reader path cannot race with the
    // writer's increment. Today every caller
    // already serializes under unique_lock(bookLock_), so this is
    // defensive; cost is zero on the writer-only fast path.
    std::atomic<uint64_t> nextSequenceNumber_{1};

    // Analytics
    double vwap_ = 0.0;
    Quantity totalQty_ = 0;
    double cumulativePrice_ = 0.0;
    size_t priceUpdates_ = 0;

    // Safety
    Price referencePrice_{0};
    TradingState tradingState_{TradingState::Continuous};
    size_t maxDepthPerSide_{0};  // 0 = unlimited
    double cbThreshold_{0.05};   // Circuit breaker threshold (default 5%)
    double priceBandPct_{0.0};   // LULD price-band half-width (0 = disabled)
    double marketProtectionPct_{0.0};  // market-order sweep collar (0 = disabled)

    // Per-participant tracking — open-addressing hash maps
    FlatHashMap<OTRKey, ParticipantRiskState, OTRKeyHash> participantRisk_;
    FlatHashMap<ParticipantId, FixedVector<OrderId, 4096>> participantOrders_;

    // Trade history — bounded ring buffer (streams out, never reallocates)
    RingBuffer<Trade> tradeHistory_{65536};

    // ─── Phase 4 Compliance ──────────────────────────────────────────
    FlatHashMap<ParticipantId, STPMode> stpModes_{1024};
    FlatHashMap<ParticipantId, ParticipantRole> participantRoles_{1024};
    LULDManager luld_;
    WashTradeDetector washTradeDetector_;
};

} // namespace OrderMatcher
