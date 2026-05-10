#pragma once

#include "OrderBook.h"
#include "Journal.h"
#include "MpscQueue.h"
#include "RingBuffer.h"
#include "FIXParser.h"
#include "FlatHashMap.h"
#include "LatencyTracker.h"
#include "RateLimiter.h"
#include "Utils.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace OrderMatcher {

// Message struct for async ring buffer processing
#pragma pack(push, 1)
struct OrderRequest {
    enum class Type : uint8_t {
        NewOrder = 1,
        Cancel = 2,
        Modify = 3,
        CancelReplace = 4,
        KillSwitch = 5,
        Shutdown = 6,
        ExpireCheck = 7
    };

    Type type;
    SymbolId symbolId;
    OrderId orderId;
    ParticipantId participantId;
    Side side;
    Price price;
    Quantity qty;
    OrderType orderType;
    Price stopPrice;
    Quantity displayQty;
    TimeInForce tif;
    uint64_t expiryTime;
    Price stopLimitPrice;
    PegType pegType;
    Price pegOffset;
    Price trailAmount;
    Quantity minQty;
    bool hidden;

    // For modify/cancel-replace
    Price newPrice;
    Quantity newQty;

    // End-to-end latency: stamped at enqueue time (nowNs())
    uint64_t ingressNs = 0;
};
#pragma pack(pop)

struct SubmitResult {
    enum class Status : uint8_t {
        Accepted,
        Rejected
    };

    Status status{Status::Rejected};
    RejectReason rejectReason{RejectReason::None};
    uint64_t sequenceId{0};

    static SubmitResult accepted(uint64_t sequenceId) {
        return {Status::Accepted, RejectReason::None, sequenceId};
    }

    static SubmitResult rejected(RejectReason reason) {
        return {Status::Rejected, reason, 0};
    }

    bool isAccepted() const { return status == Status::Accepted; }
};

class MatchingEngine {
public:
    using ClockFn = std::function<uint64_t()>;

    MatchingEngine();
    ~MatchingEngine();

    // Synchronous mode (direct calls, no threading)
    void start();
    void stop();

    // Async mode: spawns a pool of worker threads. Orders are routed by hashing the SymbolId.
    void startAsync(size_t numThreads = 1, size_t queueSize = 8192);
    void stopAsync();

    // Wait until all queued requests have been processed
    void waitForDrain();

    bool isAsync() const { return async_; }

    // Automated expiry timer (Gap 3)
    void startExpiryTimer(uint64_t intervalMs = 1000);
    void stopExpiryTimer();
    void setExpiryClock(ClockFn clock) { expiryClock_ = std::move(clock); }
    void clearExpiryClock() { expiryClock_ = {}; }
    uint64_t expiryNow() const;
    void expireOrdersFromClock();

    // Symbol management
    void addSymbol(SymbolId symbolId, MatchAlgorithm algo = MatchAlgorithm::PriceTime);
    OrderBook* getOrderBook(SymbolId symbolId);
    const OrderBook* getOrderBook(SymbolId symbolId) const;

    // Multi-symbol gateway (sync mode: direct call; async mode: enqueue)
    void processOrder(SymbolId symbolId, OrderId orderId, ParticipantId participantId,
                      Side side, Price price, Quantity qty, OrderType type,
                      Price stopPrice = 0, Quantity displayQty = 0,
                      TimeInForce tif = TimeInForce::GTC, uint64_t expiryTime = 0,
                      Price stopLimitPrice = 0,
                      PegType pegType = PegType::None, Price pegOffset = 0,
                      Price trailAmount = 0, Quantity minQty = 0, bool hidden = false);
    SubmitResult submitOrder(SymbolId symbolId, OrderId orderId, ParticipantId participantId,
                             Side side, Price price, Quantity qty, OrderType type,
                             Price stopPrice = 0, Quantity displayQty = 0,
                             TimeInForce tif = TimeInForce::GTC, uint64_t expiryTime = 0,
                             Price stopLimitPrice = 0,
                             PegType pegType = PegType::None, Price pegOffset = 0,
                             Price trailAmount = 0, Quantity minQty = 0, bool hidden = false);

    void cancelOrder(SymbolId symbolId, OrderId orderId);
    bool modifyOrder(SymbolId symbolId, OrderId orderId, Quantity newQty);
    bool cancelReplace(SymbolId symbolId, OrderId orderId, Price newPrice, Quantity newQty);
    SubmitResult submitCancel(SymbolId symbolId, OrderId orderId);
    SubmitResult submitModify(SymbolId symbolId, OrderId orderId, Quantity newQty);
    SubmitResult submitCancelReplace(SymbolId symbolId, OrderId orderId, Price newPrice, Quantity newQty);

    // Kill switch: cancel all orders for a participant across ALL symbols
    uint64_t killSwitch(ParticipantId participantId);

    // Risk management
    void setRiskLimits(SymbolId symbolId, ParticipantId participantId, const RiskLimits& limits);

    // Market data
    MarketDataSnapshot getSnapshot(SymbolId symbolId, size_t depth = 10);

    // Auction
    void uncross(SymbolId symbolId);

    // Coordinated state transition: switch every listed symbol's trading
    // state atomically (under booksMutex_). Real venues open and close
    // groups of symbols together — the equity universe at 9:30, an
    // options class at 9:30:30, etc. Without batched transitions, a
    // client iterating symbols and calling setTradingState individually
    // could have one symbol still matching while a sibling has already
    // halted, producing observably-inconsistent market states.
    //
    // Symbols not registered are skipped silently. Returns the count of
    // symbols that were transitioned.
    size_t setTradingStateBatch(const std::vector<SymbolId>& symbols,
                                TradingState state);

    // Convenience: uncross a group of symbols atomically. Same rationale
    // as setTradingStateBatch — opening/closing auctions across a group
    // should produce a single set of trades, not interleaved per-symbol
    // partial uncrosses.
    size_t uncrossBatch(const std::vector<SymbolId>& symbols);

    // Time management
    void expireOrders(uint64_t currentTime);

    // Journal/Persistence
    void enableJournal(const std::string& path);

    // Replay journal to rebuild order book state (crash recovery)
    size_t replayJournal();

    // Write full snapshot of all active orders to journal (checkpoint)
    void checkpoint();

    // Async stats (aggregates across all threads)
    uint64_t getSubmittedCount() const;
    uint64_t getProcessedCount() const;

    // Backpressure handling (Gap 7)
    using BackpressureCallback = std::function<void(const OrderRequest&)>;
    void setBackpressureCallback(BackpressureCallback cb) { backpressureCb_ = std::move(cb); }
    void setMaxPushRetries(uint32_t retries) { maxPushRetries_ = retries; }
    uint64_t getDroppedCount() const { return droppedCount_.load(std::memory_order_relaxed); }

    // End-to-end latency tracking (ingress → processing complete)
    const LatencyTracker& getE2ELatency(size_t threadIndex) const { return e2eLatency_[threadIndex]; }
    // Aggregate across all threads
    LatencyTracker getAggregateE2ELatency() const;

    // Per-participant rate limiting (bounds queue depth → bounds tail latency)
    void setRateLimit(uint64_t ratePerSec, uint64_t burstSize) {
        rateLimiter_.setDefaultRate(ratePerSec, burstSize);
    }
    void setParticipantRateLimit(ParticipantId id, uint64_t ratePerSec, uint64_t burstSize) {
        rateLimiter_.setParticipantRate(id, ratePerSec, burstSize);
    }
    RateLimiter& getRateLimiter() { return rateLimiter_; }
    uint64_t getRateLimitedCount() const { return rateLimitedCount_.load(std::memory_order_relaxed); }

    // Queue-depth backpressure: reject new orders when any target queue exceeds
    // this fraction of capacity (0.0 = disabled, 0.8 = reject at 80% full).
    // This puts a hard ceiling on queuing delay: maxDepth * avgProcessingTime.
    void setBackpressureThreshold(double fraction) { bpThresholdFraction_ = fraction; }
    double getBackpressureThreshold() const { return bpThresholdFraction_; }
    uint64_t getBackpressureRejectCount() const { return bpRejectCount_.load(std::memory_order_relaxed); }

    // ─── FIX protocol gateway ─────────────────────────────────────────────────
    // Process a raw FIX message string and route to appropriate handler
    void processFIXMessage(const std::string& rawFix);

    // ─── Legacy single-symbol interface (backward compat, uses symbol 0) ───
    void processOrder(OrderId orderId, ParticipantId participantId, Side side,
                      Price price, Quantity qty, OrderType type,
                      Price stopPrice = 0, Quantity displayQty = 0);
    void cancelOrder(OrderId orderId);
    void uncross() { uncross(0); }
    double getVWAP() const;

private:
    void ensureDefaultSymbol();
    void workerLoop(size_t threadIndex);
    void processRequest(size_t threadIndex, const OrderRequest& req);
    void maybeTriggerAutoCheckpoint();
    void checkpointInternal(bool alreadyDrained);
    void rebuildThreadSymbolIndex();

    FlatHashMap<SymbolId, std::unique_ptr<OrderBook>> books_{64};
    std::vector<SymbolId> symbolIds_;
    std::vector<std::vector<SymbolId>> symbolsByThread_;
    std::atomic<bool> running_{false};
    std::unique_ptr<Journal> journal_;
    std::atomic<bool> booksFrozen_{false};

    // Thread safety for sync-mode expiry timer
    mutable std::mutex bookMutex_;
    mutable std::mutex journalMutex_;

    // Async mode (Thread Pool)
    bool async_{false};
    size_t numThreads_{1};
    std::vector<std::unique_ptr<MpscQueue<OrderRequest>>> requestQueues_;
    std::vector<std::thread> workerThreads_;
    std::unique_ptr<std::atomic<uint64_t>[]> queueWakeups_;
    
    struct alignas(64) ThreadStats {
        std::atomic<uint64_t> submitted{0};
        std::atomic<uint64_t> processed{0};
    };
    std::unique_ptr<ThreadStats[]> threadStats_;
    std::atomic<uint64_t> submittedTotal_{0};
    std::atomic<uint64_t> processedTotal_{0};
    std::atomic<uint64_t> nextSubmitSequence_{1};

    // Backpressure (Gap 7)
    BackpressureCallback backpressureCb_;
    uint32_t maxPushRetries_{1000};
    std::atomic<uint64_t> droppedCount_{0};

    // Per-thread end-to-end latency trackers
    std::unique_ptr<LatencyTracker[]> e2eLatency_;

    // Per-participant rate limiter (applied at ingress before enqueue)
    RateLimiter rateLimiter_;
    std::atomic<uint64_t> rateLimitedCount_{0};

    // Queue-depth backpressure
    double bpThresholdFraction_{0.0};  // 0 = disabled
    std::atomic<uint64_t> bpRejectCount_{0};

    // Automated expiry timer (Gap 3)
    std::thread expiryThread_;
    std::atomic<bool> expiryRunning_{false};
    uint64_t expiryIntervalMs_{1000};
    ClockFn expiryClock_;
    std::atomic<bool> checkpointPending_{false};
    size_t checkpointThresholdEntries_{250000};
    size_t checkpointThresholdBytes_{64 * 1024 * 1024};

    // Internal helper: push to specific thread's queue with backpressure
    bool enqueueSafe(size_t threadIndex, const OrderRequest& req);
    size_t getThreadIndex(SymbolId symbolId) const;
};

} // namespace OrderMatcher
