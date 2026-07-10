#pragma once

#include "OrderBook.h"
#include "Journal.h"
#include "ContingencyManager.h"
#include "FeeEngine.h"
#include "CatReporter.h"
#include "HierarchicalRiskManager.h"
#include "MpscQueue.h"
#include "RingBuffer.h"
#include "FIXParser.h"
#include "FlatHashMap.h"
#include "LatencyTracker.h"
#include "RateLimiter.h"
#include "GraduatedKillSwitch.h"
#include "IncidentLogger.h"
#include "CapacityMonitor.h"
#include "Utils.h"
#include "BatchRiskValidator.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <limits>
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

    // ─── P3-6 Graceful shutdown with state persistence ───────────────────────
    // Summary of what a graceful shutdown did, returned so the caller can log /
    // assert the outcome. All counts are post-drain, point-in-time.
    struct ShutdownReport {
        size_t dayOrdersCancelled = 0;    // DAY orders cancelled at session end
        size_t gtdOrdersPersisted = 0;    // GTD survivors written to the checkpoint
        size_t otherOrdersPersisted = 0;  // GTC / other survivors written
        size_t ordersPersisted = 0;       // gtd + other (total snapshotted)
        bool   journalEnabled = false;    // false ⇒ nothing durable to restore from
    };

    // Deterministic, fail-safe shutdown. Order of operations (see the .cpp for
    // the rationale of each step):
    //   1. Stop admitting NEW orders (shuttingDown_ flag). In-flight requests
    //      already enqueued still drain — nothing already accepted is dropped.
    //   2. Drain the worker queues so every in-flight order finishes matching:
    //        · an in-flight IOC cancels its unmatched remainder during the match;
    //        · an in-flight partial fill completes (the batched match loop runs
    //          to completion) and the resting remainder is materialised before
    //          the snapshot, so it is either persisted (GTD/GTC) or cancelled
    //          (DAY) — never left half-processed.
    //   3. Cancel every DAY order (session end) — journaled + logged — so a
    //      restart does NOT restore them.
    //   4. Checkpoint the remaining resting orders (GTD + GTC) so a restart's
    //      replayJournal() restores them byte-for-byte.
    // Never throws, never crashes; safe to call once at process shutdown. The
    // engine is NOT torn down here — the caller still calls stop()/stopAsync().
    ShutdownReport gracefulShutdown();
    bool isShuttingDown() const { return shuttingDown_.load(std::memory_order_acquire); }

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

    // Multi-symbol gateway.
    //
    // Two flavors with the same effect on the engine. New code should
    // prefer `submitOrder` — it returns a `SubmitResult` with the
    // reject reason on failure. `processOrder` is the legacy
    // fire-and-forget form; it discards the SubmitResult and is kept
    // only for backwards compatibility with tests and tools that don't
    // care about the rejection path.
    //
    // Sync mode (start()): the order is dispatched directly on the
    // caller's thread.
    // Async mode (startAsync()): the order is enqueued onto the
    // worker's MpscQueue and processed off-thread.
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

    // Set the market-maker role for a participant across all symbols.
    // Role determines guaranteed fill allocation in pro-rata matching.
    void setParticipantRole(ParticipantId p, ParticipantRole role);

    // ─── Contingent orders (OCO) ─────────────────────────────────────
    // Link two same-symbol orders One-Cancels-Other: the first to execute
    // (any fill) cancels the other. The cancel runs synchronously right
    // after the triggering order is processed — thread-per-symbol makes
    // that race-free — and is journaled so replay reproduces it.
    void registerOco(SymbolId symbolId, OrderId a, OrderId b);

    // ─── Risk, fees & compliance (opt-in) ────────────────────────────
    // Multi-tier pre-trade risk: map a trader into its strategy/account/firm
    // hierarchy and set per-tier limits. Once any mapping/limit is set the
    // pre-trade check runs in the order path and fills accrue to every tier.
    void mapParticipant(ParticipantId trader, uint64_t strategyId,
                        uint64_t accountId, uint64_t firmId);
    void setRiskTierLimits(RiskTier tier, uint64_t entityId, const TierLimits& limits);
    int64_t riskNetPosition(RiskTier tier, uint64_t entityId) const;
    // Maker-taker fees: configure schedules; query accrued net fees.
    void setDefaultFeeSchedule(const FeeSchedule& s);
    void setParticipantFeeSchedule(ParticipantId p, const FeeSchedule& s);
    int64_t accruedFee(ParticipantId p) const;
    // CAT-style audit trail: enable capture; inspect / export records.
    void enableAuditTrail(bool on = true);
    size_t auditRecordCount() const;
    std::string auditTrailJsonl() const;

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

    // Reopen every book currently in a volatility auction: run its
    // reopening cross and return it to continuous trading. Returns the
    // count of books reopened. The timing policy (how long the volatility
    // pause lasts) lives with the caller — a scheduler/timer decides when
    // to invoke this; the engine just performs the reopening crosses.
    size_t resumeVolatilityAuctions();

    // Time management
    void expireOrders(uint64_t currentTime);

    // Journal/Persistence
    void enableJournal(const std::string& path);

    // Access the underlying Journal (null if enableJournal not called).
    // Used by the replication coordinator wiring to attach an onCommit
    // hook that ships each committed batch to backups.
    Journal* getJournal() { return journal_.get(); }

    // Toggle replay mode across every registered book. Backups run
    // permanently in replay mode so that applying primary-shipped
    // journal entries does not re-emit market data or order updates
    // to local clients (the primary is the canonical source).
    void setReplayModeAllBooks(bool replay);

    // Apply a single deserialized JournalEntry to the engine. Used by
    // the replication backup path: the coordinator receives an entry
    // over the wire and dispatches it through this method so the
    // backup's books stay byte-identical to the primary's. Returns
    // false if the entry references a symbol unknown to this engine
    // (caller can decide whether to log/halt).
    bool applyReplicatedEntry(const struct JournalEntry& entry);

    // Stream a snapshot of all currently-resting orders. For each
    // order, builds a JournalEntry of type Snapshot and invokes the
    // callback. Used by the replication primary path to bring a
    // newly-connected backup up to date before resuming live
    // replication — closes the "joined-after-orders-existed" gap
    // surfaced by the rolling-restart chaos scenario.
    //
    // Concurrency: takes each book's bookLock_ in shared mode while
    // iterating. New orders accepted during iteration may or may not
    // appear in the stream; the Snapshot handler on the receiver
    // side is idempotent (skips already-known orderIds), so the
    // overlap is safe — duplicates collapse, no entries are lost.
    void streamSnapshot(const std::function<void(const struct JournalEntry&)>& fn) const;

    // Replay journal to rebuild order book state (crash recovery)
    size_t replayJournal();

    // Write full snapshot of all active orders to journal (checkpoint)
    void checkpoint();

    // Async stats (aggregates across all threads)
    uint64_t getSubmittedCount() const;
    uint64_t getProcessedCount() const;

    // Backpressure handling. The callback fires when the engine's
    // ingress queue is full and a request is being dropped. Note:
    // `std::function` may heap-allocate when the captured lambda
    // exceeds the small-buffer threshold — call this once at startup
    // (not per-connection) to keep the allocation off the hot path.
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

    // Pre-trade risk batching configuration (Phase 1, Week 2)
    void setRiskBatchSize(size_t size) {
        riskBatchSize_ = size > 16 ? 16 : (size == 0 ? 1 : size);
    }
    size_t getRiskBatchSize() const { return riskBatchSize_; }

    // ─── Hot-path pre-trade risk controls (P2-8 … P2-11) ─────────────────────
    // All checks below are O(1), lock-free (atomic ops only) and allocation-free
    // on the accept path. Each control is opt-in: it does nothing until configured
    // at startup, so the existing hot path is unchanged when unused. Per-id state
    // lives in fixed-size arrays indexed by participant/symbol id; ids at or above
    // the bounds bypass the array-indexed controls.
    static constexpr size_t MAX_PARTICIPANTS = 1024;  // positions_ / OTR array bound
    static constexpr size_t MAX_RISK_SYMBOLS = 256;   // fat-finger array bound

    // P2-8 Kill switch. setKillSwitch(true) is callable from a monitoring thread
    // while the matching thread is busy: it publishes the flag first (so new
    // orders are rejected immediately) THEN cancels all resting orders.
    void setKillSwitch(bool engaged);
    bool isKillSwitchActive() const {
        return killSwitchActive_.load(std::memory_order_acquire);
    }

    // P2-9 Position limits. Limit is the max absolute net position (signed
    // exposure = filled ± working orders). 0 = unlimited.
    void setPositionLimit(ParticipantId pid, int64_t maxAbsPosition);
    int64_t getPosition(ParticipantId pid) const;

    // P2-10 Fat-finger, per instrument. maxQty / maxNotional 0 = unset;
    // maxDeviationPct 0 = unset (e.g. 0.10 for ±10% around the reference price).
    void setFatFingerLimits(SymbolId sym, Quantity maxQty,
                            double maxDeviationPct, int64_t maxNotional);
    // Seed / override the reference price used by the ±deviation band. It is
    // otherwise tracked automatically from the last trade on that symbol.
    void setReferencePrice(SymbolId sym, Price px);

    // P2-11 Order-to-trade ratio throttle (per participant, shared limit/window).
    // maxRatio 0 = disabled. A participant whose orders/trades over the rolling
    // window exceeds maxRatio (after at least minOrders in the window) is throttled
    // until the window rolls forward.
    void setOtrLimit(double maxRatio, uint64_t windowMs, uint64_t minOrders);
    // Inject a clock for the OTR rolling window (defaults to nowNs()). Enables
    // deterministic tests of the engage/release transition.
    void setRiskClock(ClockFn clock) { riskClock_ = std::move(clock); }

    // Risk-control rejection counters (observability / tests).
    uint64_t getKillSwitchRejectCount() const {
        return killSwitchRejects_.load(std::memory_order_relaxed);
    }
    uint64_t getPositionRejectCount() const {
        return positionRejects_.load(std::memory_order_relaxed);
    }
    uint64_t getFatFingerRejectCount() const {
        return fatFingerRejects_.load(std::memory_order_relaxed);
    }
    uint64_t getOtrRejectCount() const {
        return otrRejects_.load(std::memory_order_relaxed);
    }

    // ─── FIX protocol gateway ─────────────────────────────────────────
    // Process a raw FIX message string and route to appropriate handler
    void processFIXMessage(const std::string& rawFix);

    // ─── Phase 4: Graduated Kill Switch ──────────────────────────────
    GraduatedKillSwitch& getKillSwitch() { return killSwitch_; }
    const GraduatedKillSwitch& getKillSwitch() const { return killSwitch_; }

    // ─── Phase 4: Incident Logger ───────────────────────────────────
    IncidentLogger& getIncidentLogger() { return incidentLogger_; }
    bool enableIncidentLog(const std::string& path) { return incidentLogger_.open(path); }

    // ─── Phase 4: Capacity Monitor ──────────────────────────────────
    CapacityMonitor& getCapacityMonitor() { return capacityMonitor_; }

    // ─── Legacy single-symbol interface (backward compat, uses symbol 0) ───
    void processOrder(OrderId orderId, ParticipantId participantId, Side side,
                      Price price, Quantity qty, OrderType type,
                      Price stopPrice = 0, Quantity displayQty = 0);
    void cancelOrder(OrderId orderId);
    void uncross() { uncross(0); }
    double getVWAP() const;

enum class RiskValidationResult : uint8_t {
    NotValidated,
    Passed,
    Failed
};

// Private helper methods inside class MatchingEngine:
private:
    void ensureDefaultSymbol();
    // Raw symbol registration. ASSUMES bookMutex_ is already held by the
    // caller. addSymbol() is the public, self-locking wrapper; internal callers
    // that already hold bookMutex_ (e.g. applyReplicatedEntry's ensureBook) use
    // this form to avoid re-locking the non-recursive mutex.
    void addSymbolLocked(SymbolId symbolId, MatchAlgorithm algo = MatchAlgorithm::PriceTime);
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
    // P3-6: set by gracefulShutdown() to refuse NEW orders while draining.
    // Reset on start()/startAsync() so an engine can be restarted in-process.
    std::atomic<bool> shuttingDown_{false};

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
    size_t riskBatchSize_{8};

    // ─── Phase 4 Compliance ──────────────────────────────────────────
    GraduatedKillSwitch killSwitch_;
    IncidentLogger incidentLogger_;
    CapacityMonitor capacityMonitor_;

    // Internal helper: push to specific thread's queue with backpressure
    bool enqueueSafe(size_t threadIndex, const OrderRequest& req);
    size_t getThreadIndex(SymbolId symbolId) const;

    // ─── OCO contingent-order plumbing ───────────────────────────────
    // Per-book observer buffering ids of orders that executed, so the
    // engine can drive OCO cancellation after the triggering request
    // completes (off the book lock). Buffers only while OCO is active, so
    // the no-OCO hot path stays free.
    // Per-book engine-side observer. Buffers (a) ids of orders that executed
    // — drained to drive OCO sibling-cancellation — and (b) executed trades —
    // drained to drive fees, hierarchical-risk position accrual, and the CAT
    // audit trail. Each buffer is gated by its own atomic so the hot path
    // stays free when its feature is unused. A book is driven by exactly one
    // worker thread, so the buffers are single-threaded.
    struct OcoBookListener : EventListener {
        MatchingEngine* engine = nullptr;                 // for hot-path risk fill accrual
        const std::atomic<bool>* ocoActive = nullptr;     // gates the executed buffer
        const std::atomic<bool>* tradesActive = nullptr;  // gates the trades buffer
        std::vector<OrderId> executed;
        std::vector<Trade>   trades;
        // Persistent drain buffers reused across driveOco cycles. The drain
        // swaps these with executed/trades and clear()s them (keeping
        // capacity), so a buffer with reserved storage is always swapped back
        // into executed/trades — eliminating the per-cycle reallocation the
        // old swap-with-a-local pattern incurred. Touched only by the symbol's
        // owning worker thread (matching + drain run on the same thread).
        std::vector<OrderId> firedScratch;
        std::vector<Trade>   tradesScratch;
        void onOrderUpdate(const OrderUpdate& u) override;
        void onTrade(const Trade& t) override;
    };
    // Drain a book's observer after a request: OCO sibling cancels, then the
    // trade-driven consumers (fees / risk accrual / audit).
    void driveOco(SymbolId symbolId, OrderBook* book);
    // Pre-trade hierarchical risk gate; true = admit. No-op (true) when risk
    // is not configured. Runs on the worker thread, where book reads are safe.
    bool preTradeRiskCheck(ParticipantId trader, Side side, Price price,
                           Quantity qty, Price referencePrice);

    ContingencyManager contingency_;
    std::mutex contingencyMutex_;
    std::atomic<bool> ocoActive_{false};
    FlatHashMap<SymbolId, std::unique_ptr<OcoBookListener>> ocoListeners_{64};

    // Risk / fees / compliance components (opt-in; gated to keep the hot path
    // free until configured). Each has its own mutex — they are touched from
    // worker threads (drain) and reader threads (admin getters).
    FeeEngine feeEngine_;
    CatReporter catReporter_;
    HierarchicalRiskManager riskManager_;
    mutable std::mutex feeMutex_;
    mutable std::mutex catMutex_;
    mutable std::mutex riskMutex_;
    std::atomic<bool> feesActive_{false};
    std::atomic<bool> riskActive_{false};
    std::atomic<bool> catActive_{false};
    std::atomic<bool> observersActive_{false};  // any trade-driven consumer enabled

    // ─── Hot-path pre-trade risk controls state (P2-8 … P2-11) ───────────────
    // Feature gates. Loaded (relaxed) on the accept path; when all are false the
    // only added cost is a couple of atomic loads. anyRiskActive_ additionally
    // gates the per-fill onTrade accrual hook.
    std::atomic<bool> killSwitchActive_{false};      // P2-8
    std::atomic<bool> positionLimitsActive_{false};  // P2-9
    std::atomic<bool> fatFingerActive_{false};       // P2-10
    std::atomic<bool> otrActive_{false};             // P2-11
    std::atomic<bool> anyRiskActive_{false};         // gates onRiskFill()

    // P2-9 positions: signed net exposure (filled ± working orders), and the
    // per-participant absolute limit (0 = unlimited). Value-initialised to 0.
    std::atomic<int64_t> positions_[MAX_PARTICIPANTS]{};
    std::atomic<int64_t> positionLimit_[MAX_PARTICIPANTS]{};

    // P2-10 fat-finger per-symbol config + reference (last-trade) price.
    std::atomic<uint64_t> ffMaxQty_[MAX_RISK_SYMBOLS]{};          // 0 = unset
    std::atomic<int64_t>  ffMaxDeviationBps_[MAX_RISK_SYMBOLS]{}; // basis points; 0 = unset
    std::atomic<int64_t>  ffMaxNotional_[MAX_RISK_SYMBOLS]{};     // 0 = unset
    std::atomic<int64_t>  refPrice_[MAX_RISK_SYMBOLS]{};          // last trade / seed; 0 = none

    // P2-11 OTR rolling window, per participant.
    std::atomic<uint64_t> otrWindowStart_[MAX_PARTICIPANTS]{};
    std::atomic<uint64_t> otrOrders_[MAX_PARTICIPANTS]{};
    std::atomic<uint64_t> otrTrades_[MAX_PARTICIPANTS]{};
    double   otrMaxRatio_{0.0};                    // 0 = disabled
    uint64_t otrWindowNs_{1'000'000'000ull};       // 1s default
    uint64_t otrMinOrders_{20};
    ClockFn  riskClock_;                           // OTR window clock (default nowNs)

    // Rejection counters (observability).
    std::atomic<uint64_t> killSwitchRejects_{0};
    std::atomic<uint64_t> positionRejects_{0};
    std::atomic<uint64_t> fatFingerRejects_{0};
    std::atomic<uint64_t> otrRejects_{0};

    // Sentinel participant id meaning "all participants" for the async
    // KillSwitch request path (cancel every resting order, not one trader's).
    static constexpr ParticipantId kKillAllParticipants =
        std::numeric_limits<ParticipantId>::max();

    // Combined pre-trade risk gate (fat-finger, position, OTR). Returns
    // RejectReason::None to admit. Kill switch is checked separately (first).
    RejectReason checkRiskControls(SymbolId sym, ParticipantId pid, Side side,
                                   Price price, Quantity qty, OrderType type);
    // Reserve/release working-order exposure for the position limit.
    void reservePosition(ParticipantId pid, Side side, Quantity restingQty);
    void releasePosition(const Order* order);
    // Per-fill accrual: last-trade price, taker position, OTR trade counts.
    void onRiskFill(const Trade& t);
    // Cancel every resting order across all books (sync path of the kill switch).
    void cancelAllRestingOrders();
    // P3-6: cancel every DAY order across all books (session end), journaling +
    // logging each so a restart does not restore them. Returns the count.
    size_t cancelDayOrders();
    uint64_t riskNow() const;
    void logRiskReject(const char* control, SymbolId sym, ParticipantId pid,
                       RejectReason reason);
};

} // namespace OrderMatcher
