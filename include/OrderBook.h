#pragma once

#include "Order.h"
#include "IntrusiveList.h"
#include "MemoryPool.h"
#include "FlatPriceMap.h"
#include "FlatHashMap.h"
#include "EventListener.h"
#include "RingBuffer.h"
#include <functional>
#include <shared_mutex>
#include <variant>
#include <cstdint>
#include <limits>

namespace OrderMatcher {

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
    explicit OrderBook(SymbolId symbolId = 0, MatchAlgorithm algo = MatchAlgorithm::PriceTime);

    // Core Actions
    AddOrderResult addOrder(OrderId orderId, ParticipantId participantId, Side side, Price price, Quantity qty, OrderType type,
                  Price stopPrice = 0, Quantity displayQty = 0,
                  TimeInForce tif = TimeInForce::GTC, uint64_t expiryTime = 0,
                  Price stopLimitPrice = 0,
                  PegType pegType = PegType::None, Price pegOffset = 0,
                  Price trailAmount = 0, Quantity minQty = 0, bool hidden = false);
    void cancelOrder(OrderId orderId);
    bool modifyOrder(OrderId orderId, Quantity newQty);

    // Cancel/Replace: full amendment (price change loses time priority)
    bool cancelReplace(OrderId orderId, Price newPrice, Quantity newQty);

    // Kill switch: cancel all orders for a participant
    uint64_t cancelAllForParticipant(ParticipantId participantId);

    // Auction
    void uncross();

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

    // Analytics
    double getVWAP() const { return vwap_; }
    uint64_t getTradeCount() const { return priceUpdates_; }

    // Trade history ring buffer access
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
    ParticipantStats getParticipantStats(ParticipantId id) const {
        auto* stats = otrStats_.find(id);
        return stats ? *stats : ParticipantStats{};
    }

    // Depth limit: max price levels per side (0 = unlimited)
    void setMaxDepth(size_t maxLevels) { maxDepthPerSide_ = maxLevels; }
    size_t getMaxDepth() const { return maxDepthPerSide_; }

    // Get all active orders (for snapshots/inspection)
    // Note: returns up to maxOrders pointers into a caller-provided buffer
    size_t getAllOrders(const Order** out, size_t maxOrders) const;

    // Iterate all active orders (for checkpoint — avoids allocation)
    template<typename Fn>
    void forEachOrder(Fn&& fn) const {
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
    }

    // Replay mode: suppresses all callbacks during journal replay
    void setReplayMode(bool replay) { replayMode_ = replay; }
    bool isReplayMode() const { return replayMode_; }

    Price getBestBid() const;
    Price getBestAsk() const;
    Price getMidPrice() const;

    double getOTR(ParticipantId p) const {
        auto* stats = otrStats_.find(p);
        return stats ? stats->getOTR() : 0.0;
    }

    void setMatchAlgorithm(MatchAlgorithm algo) { matchAlgorithm_ = algo; }

private:
    // Internal cancel that does NOT take bookLock_. Public cancelOrder
    // wraps this with a unique_lock; callers that already hold the lock
    // (expireOrders, cancelAllForParticipant) call this directly to
    // avoid self-deadlock on a non-recursive shared_mutex.
    void cancelOrderImpl(OrderId orderId);

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

    // Reader/writer mutex protecting the book's mutable state. Writers
    // (addOrder, cancelOrder, modifyOrder, cancelReplace, expireOrders,
    // cancelAllForParticipant, uncross) take a unique_lock; the snapshot
    // reader takes a shared_lock. Without this, a snapshot read could
    // observe a side-flipping cancelReplace mid-flight and return a
    // torn view (an order on both sides) — exactly the scenario the
    // Snapshot.tla spec produced as a counterexample.
    //
    // Cost: one uncontended mutex acquire per write on the engine's
    // hot path. With single-writer-per-book (the engine's worker-
    // thread model), there's no writer-vs-writer contention; the
    // overhead only materializes when a snapshot is in flight.
    mutable std::shared_mutex bookLock_;

    // Bids: Descending Price (flat array, O(1) best-bid)
    FlatPriceMap bids_;
    // Asks: Ascending Price (flat array, O(1) best-ask)
    FlatPriceMap asks_;

    // Pending orders by type — pre-allocated fixed vectors (no heap allocation)
    FixedVector<Order*, 16384> stopOrders_;
    FixedVector<Order*, 16384> trailingStopOrders_;
    FixedVector<Order*, 16384> peggedOrders_;
    // Market orders submitted during PreOpen / AuctionOpen / AuctionClose.
    // Parked here instead of in the FlatPriceMap (they have no limit
    // price). At uncross time they are inserted at the discovered uncross
    // price and execute against limit liquidity; any remainder is
    // cancelled at the end of the uncross.
    FixedVector<Order*, 16384> auctionMarketOrders_;

    // O(1) Lookup — open-addressing hash map (replaces std::unordered_map)
    FlatHashMap<OrderId, Order*> orderLookup_;
    ObjectPool<Order> orderPool_;

    // Event listener (replaces std::function callbacks — zero-cost vtable dispatch)
    EventListener* listener_ = &nullListener();

    // Replay mode flag — suppresses callbacks during journal recovery
    bool replayMode_{false};

    // Identity
    SymbolId symbolId_;
    MatchAlgorithm matchAlgorithm_;

    uint64_t nextTradeId_{1};
    Price lastTradePrice_{0};
    Quantity lastTradeQty_{0};
    uint64_t nextSequenceNumber_{1};

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

    // Per-participant tracking — open-addressing hash maps
    FlatHashMap<ParticipantId, ParticipantStats> otrStats_;
    FlatHashMap<ParticipantId, RiskLimits> riskLimits_;
    FlatHashMap<ParticipantId, FixedVector<OrderId, 4096>> participantOrders_;

    // Trade history — bounded ring buffer (streams out, never reallocates)
    RingBuffer<Trade> tradeHistory_{65536};
};

} // namespace OrderMatcher
