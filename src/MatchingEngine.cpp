#include "MatchingEngine.h"
#include "LatencyTracker.h"
#include "Metrics.h"
#include "StructuredLog.h"
#include <iostream>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#if defined(__linux__)
#include <pthread.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_policy.h>
#endif

namespace OrderMatcher {

namespace {

inline void cpuRelax() {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
    asm volatile("yield");
#else
    std::this_thread::yield();
#endif
}

void pinCurrentThreadToCore(size_t threadIndex) {
#if defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(static_cast<int>(threadIndex), &cpuset);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#elif defined(__APPLE__)
    thread_affinity_policy_data_t policy{
        static_cast<integer_t>(threadIndex + 1)
    };
    (void)thread_policy_set(mach_thread_self(), THREAD_AFFINITY_POLICY,
                            reinterpret_cast<thread_policy_t>(&policy), 1);
#else
    (void)threadIndex;
#endif
}

} // namespace

MatchingEngine::MatchingEngine() {
    ensureDefaultSymbol();
}

MatchingEngine::~MatchingEngine() {
    stopExpiryTimer();
    if (async_) {
        stopAsync();
    } else {
        stop();
    }
}

bool MatchingEngine::enqueueSafe(size_t threadIndex, const OrderRequest& req) {
    if (bpThresholdFraction_ > 0.0) {
        size_t maxDepth = static_cast<size_t>(
            bpThresholdFraction_ * requestQueues_[threadIndex]->capacity());
        if (requestQueues_[threadIndex]->approxSize() >= maxDepth) {
            bpRejectCount_.fetch_add(1, std::memory_order_relaxed);
            if (backpressureCb_) {
                backpressureCb_(req);
            }
            return false;
        }
    }

    for (uint32_t i = 0; i < maxPushRetries_; ++i) {
        if (requestQueues_[threadIndex]->push(req)) {
            threadStats_[threadIndex].submitted.fetch_add(1, std::memory_order_relaxed);
            submittedTotal_.fetch_add(1, std::memory_order_release);
            queueWakeups_[threadIndex].fetch_add(1, std::memory_order_release);
            queueWakeups_[threadIndex].notify_one();
            return true;
        }
        cpuRelax();
    }

    droppedCount_.fetch_add(1, std::memory_order_relaxed);
    if (backpressureCb_) {
        backpressureCb_(req);
    }
    return false;
}

namespace {

// Counters cached at file scope — first access locks the registry to
// allocate; subsequent calls hit the atomic directly. The cache means
// repeated submits on the hot path don't re-enter the registry mutex.
Counter& orderAcceptedCounter() {
    static auto& c = MetricsRegistry::instance().counter(
        "orders_accepted_total",
        "Cumulative count of orders accepted by the engine");
    return c;
}
Counter& orderRejectedCounter() {
    static auto& c = MetricsRegistry::instance().counter(
        "orders_rejected_total",
        "Cumulative count of orders rejected by the engine (any reason)");
    return c;
}

SubmitResult orderBookResultToSubmitResult(const AddOrderResult& result, uint64_t sequenceId) {
    if (std::holds_alternative<OrderId>(result)) {
        orderAcceptedCounter().increment();
        return SubmitResult::accepted(sequenceId);
    }
    orderRejectedCounter().increment();
    return SubmitResult::rejected(std::get<RejectReason>(result));
}

// Helper for the async path: bumps the rejected counter and returns
// the SubmitResult. Use at every async-side reject return.
SubmitResult rejectedAsync(RejectReason r) {
    orderRejectedCounter().increment();
    return SubmitResult::rejected(r);
}

// Symmetric helper for the async-accepted path.
SubmitResult acceptedAsync(uint64_t sequenceId) {
    orderAcceptedCounter().increment();
    return SubmitResult::accepted(sequenceId);
}

} // namespace

size_t MatchingEngine::getThreadIndex(SymbolId symbolId) const {
    return std::hash<SymbolId>{}(symbolId) % numThreads_;
}

uint64_t MatchingEngine::getSubmittedCount() const {
    return submittedTotal_.load(std::memory_order_acquire);
}

uint64_t MatchingEngine::getProcessedCount() const {
    return processedTotal_.load(std::memory_order_acquire);
}

void MatchingEngine::ensureDefaultSymbol() {
    // Mutates books_/symbolIds_; guard with bookMutex_ for the same discipline
    // as addSymbol. Called from the constructor (no contention), but uniform
    // locking keeps every books_/symbolIds_ mutation guarded.
    std::lock_guard<std::mutex> lock(bookMutex_);
    if (books_.contains(0)) {
        return;
    }

    books_.insert(0, std::make_unique<OrderBook>(0));
    symbolIds_.push_back(0);
}

void MatchingEngine::start() {
    booksFrozen_.store(true, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    obSink().log(obEvent("engine_start")
                     .kv("mode", "sync")
                     .kv("symbols", (unsigned long long)symbolIds_.size()));
}

void MatchingEngine::stop() {
    running_.store(false, std::memory_order_release);
    booksFrozen_.store(false, std::memory_order_release);
    obSink().log(obEvent("engine_stop").kv("mode", "sync"));
}

void MatchingEngine::rebuildThreadSymbolIndex() {
    size_t threadCount = numThreads_ == 0 ? 1 : numThreads_;
    symbolsByThread_.assign(threadCount, {});
    for (SymbolId symbolId : symbolIds_) {
        symbolsByThread_[std::hash<SymbolId>{}(symbolId) % threadCount].push_back(symbolId);
    }
}

void MatchingEngine::startAsync(size_t numThreads, size_t queueSize) {
    if (async_) {
        return;
    }

    numThreads_ = numThreads > 0 ? numThreads : 1;
    rebuildThreadSymbolIndex();

    threadStats_ = std::make_unique<ThreadStats[]>(numThreads_);
    e2eLatency_ = std::make_unique<LatencyTracker[]>(numThreads_);
    queueWakeups_ = std::make_unique<std::atomic<uint64_t>[]>(numThreads_);
    submittedTotal_.store(0, std::memory_order_release);
    processedTotal_.store(0, std::memory_order_release);

    requestQueues_.clear();
    workerThreads_.clear();
    requestQueues_.reserve(numThreads_);
    workerThreads_.reserve(numThreads_);

    running_.store(true, std::memory_order_release);
    booksFrozen_.store(true, std::memory_order_release);
    async_ = true;

    for (size_t i = 0; i < numThreads_; ++i) {
        queueWakeups_[i].store(0, std::memory_order_relaxed);
        requestQueues_.push_back(std::make_unique<MpscQueue<OrderRequest>>(queueSize));
        workerThreads_.emplace_back(&MatchingEngine::workerLoop, this, i);
    }
}

void MatchingEngine::startExpiryTimer(uint64_t intervalMs) {
    if (expiryRunning_) {
        return;
    }

    expiryIntervalMs_ = intervalMs;
    expiryRunning_ = true;
    expiryThread_ = std::thread([this]() {
        while (expiryRunning_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(expiryIntervalMs_));
            if (!expiryRunning_.load(std::memory_order_acquire)) {
                break;
            }

            expireOrdersFromClock();
        }
    });
}

void MatchingEngine::stopExpiryTimer() {
    if (!expiryRunning_) {
        return;
    }

    expiryRunning_ = false;
    if (expiryThread_.joinable()) {
        expiryThread_.join();
    }
}

uint64_t MatchingEngine::expiryNow() const {
    if (expiryClock_) {
        return expiryClock_();
    }
    return static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
}

void MatchingEngine::expireOrdersFromClock() {
    uint64_t now = expiryNow();

    if (async_) {
        OrderRequest req{};
        req.type = OrderRequest::Type::ExpireCheck;
        req.expiryTime = now;
        for (size_t i = 0; i < numThreads_; ++i) {
            enqueueSafe(i, req);
        }
    } else {
        // expireOrders() self-guards with bookMutex_ now (it iterates
        // symbolIds_); do NOT pre-lock — bookMutex_ is a non-recursive
        // std::mutex and re-locking would self-deadlock.
        expireOrders(now);
    }
}

void MatchingEngine::stopAsync() {
    if (!async_) {
        return;
    }

    stopExpiryTimer();

    OrderRequest shutdown{};
    shutdown.type = OrderRequest::Type::Shutdown;

    for (size_t i = 0; i < numThreads_; ++i) {
        while (!requestQueues_[i]->push(shutdown)) {
            cpuRelax();
        }
        threadStats_[i].submitted.fetch_add(1, std::memory_order_relaxed);
        submittedTotal_.fetch_add(1, std::memory_order_release);
        queueWakeups_[i].fetch_add(1, std::memory_order_release);
        queueWakeups_[i].notify_one();
    }

    for (auto& thread : workerThreads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    workerThreads_.clear();
    requestQueues_.clear();
    queueWakeups_.reset();

    running_.store(false, std::memory_order_release);
    booksFrozen_.store(false, std::memory_order_release);
    async_ = false;
}

void MatchingEngine::waitForDrain() {
    if (!async_) {
        return;
    }

    uint64_t target = submittedTotal_.load(std::memory_order_acquire);
    while (processedTotal_.load(std::memory_order_acquire) < target) {
        uint64_t observed = processedTotal_.load(std::memory_order_relaxed);
        processedTotal_.wait(observed, std::memory_order_relaxed);
    }

    if (checkpointPending_.exchange(false, std::memory_order_acq_rel)) {
        checkpointInternal(true);
    }
}

void MatchingEngine::workerLoop(size_t threadIndex) {
    pinCurrentThreadToCore(threadIndex);

    OrderRequest req{};
    size_t idleSpins = 0;
    uint64_t observedWake = queueWakeups_[threadIndex].load(std::memory_order_relaxed);

    while (true) {
        if (requestQueues_[threadIndex]->pop(req)) {
            idleSpins = 0;
            observedWake = queueWakeups_[threadIndex].load(std::memory_order_relaxed);

            if (req.type == OrderRequest::Type::Shutdown) {
                threadStats_[threadIndex].processed.fetch_add(1, std::memory_order_release);
                processedTotal_.fetch_add(1, std::memory_order_release);
                processedTotal_.notify_all();
                break;
            }

            processRequest(threadIndex, req);
            if (req.ingressNs != 0) {
                e2eLatency_[threadIndex].recordInterval(req.ingressNs, nowNs());
            }

            threadStats_[threadIndex].processed.fetch_add(1, std::memory_order_release);
            processedTotal_.fetch_add(1, std::memory_order_release);
            processedTotal_.notify_all();

            if (threadIndex == 0 &&
                checkpointPending_.load(std::memory_order_acquire) &&
                processedTotal_.load(std::memory_order_acquire) >=
                    submittedTotal_.load(std::memory_order_acquire)) {
                checkpointPending_.store(false, std::memory_order_release);
                checkpointInternal(true);
            }
            continue;
        }

        ++idleSpins;
        if (idleSpins < 128) {
            cpuRelax();
            continue;
        }
        if (idleSpins < 256) {
            std::this_thread::yield();
            continue;
        }

        queueWakeups_[threadIndex].wait(observedWake, std::memory_order_relaxed);
        observedWake = queueWakeups_[threadIndex].load(std::memory_order_relaxed);
        idleSpins = 0;
    }
}

void MatchingEngine::maybeTriggerAutoCheckpoint() {
    {
        std::lock_guard<std::mutex> lock(journalMutex_);
        if (!journal_) {
            return;
        }

        if (!journal_->needsCheckpoint(checkpointThresholdEntries_, checkpointThresholdBytes_)) {
            return;
        }
    }

    if (async_) {
        checkpointPending_.store(true, std::memory_order_release);
    } else {
        checkpointInternal(true);
    }
}

void MatchingEngine::processRequest(size_t threadIndex, const OrderRequest& req) {
    switch (req.type) {
    case OrderRequest::Type::NewOrder: {
        auto* book = getOrderBook(req.symbolId);
        if (!book) {
            return;
        }

        // Hierarchical pre-trade risk gate (no-op unless configured). Market
        // orders have no limit price, so they skip the fat-finger reference
        // check (pass 0); limit-priced orders use the book mid as an NBBO
        // proxy. Reading book state here is safe — one writer per book.
        if (!preTradeRiskCheck(req.participantId, req.side, req.price, req.qty,
                               req.orderType == OrderType::Market ? 0 : book->getMidPrice())) {
            book->emitReject(req.orderId, req.qty, RejectReason::RiskLimitBreached);
            break;
        }

        AddOrderResult result = book->addOrder(req.orderId, req.participantId, req.side,
                                               req.price, req.qty, req.orderType,
                                               req.stopPrice, req.displayQty, req.tif,
                                               req.expiryTime, req.stopLimitPrice,
                                               req.pegType, req.pegOffset, req.trailAmount,
                                               req.minQty, req.hidden);
        // P2-9: reserve the resting remainder as working exposure (see the
        // sync path in submitOrder for the double-count rationale).
        if (positionLimitsActive_.load(std::memory_order_relaxed) &&
            std::holds_alternative<OrderId>(result)) {
            const Order* o = book->getOrder(req.orderId);
            reservePosition(req.participantId, req.side, o ? o->remainingQty : 0);
        }
        if (journal_ && std::holds_alternative<OrderId>(result)) {
            {
                std::lock_guard<std::mutex> lock(journalMutex_);
                journal_->logAddOrder(req.orderId, req.participantId, req.symbolId, req.side,
                                      req.price, req.qty, req.orderType, req.tif,
                                      req.expiryTime, req.stopPrice, req.stopLimitPrice,
                                      req.displayQty, req.pegType, req.pegOffset,
                                      req.trailAmount, req.minQty, req.hidden);
            }
            maybeTriggerAutoCheckpoint();
        }
        if (catActive_.load(std::memory_order_relaxed) && std::holds_alternative<OrderId>(result)) {
            std::lock_guard<std::mutex> lock(catMutex_);
            catReporter_.recordNewOrder(nowNs(), req.orderId, req.participantId, req.symbolId,
                                        req.side, req.price, req.qty);
        }
        if (ocoActive_.load(std::memory_order_relaxed) ||
            observersActive_.load(std::memory_order_relaxed)) {
            driveOco(req.symbolId, book);
        }
        break;
    }
    case OrderRequest::Type::Cancel: {
        auto* book = getOrderBook(req.symbolId);
        if (!book || !book->getOrder(req.orderId)) {
            return;
        }
        // P2-9: release working exposure before the order leaves the book.
        if (positionLimitsActive_.load(std::memory_order_relaxed)) {
            releasePosition(book->getOrder(req.orderId));
        }
        book->cancelOrder(req.orderId);
        if (journal_) {
            {
                std::lock_guard<std::mutex> lock(journalMutex_);
                journal_->logCancelOrder(req.orderId);
            }
            maybeTriggerAutoCheckpoint();
        }
        if (catActive_.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lock(catMutex_);
            catReporter_.recordCancel(nowNs(), req.orderId);
        }
        if (ocoActive_.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lock(contingencyMutex_);
            contingency_.onCanceled(req.orderId);   // user-cancelled leg leaves its group
        }
        if (ocoActive_.load(std::memory_order_relaxed) ||
            observersActive_.load(std::memory_order_relaxed)) {
            driveOco(req.symbolId, book);
        }
        break;
    }
    case OrderRequest::Type::Modify: {
        auto* book = getOrderBook(req.symbolId);
        if (!book) {
            return;
        }
        if (book->modifyOrder(req.orderId, req.newQty) && journal_) {
            {
                std::lock_guard<std::mutex> lock(journalMutex_);
                journal_->logModifyOrder(req.orderId, req.newQty);
            }
            maybeTriggerAutoCheckpoint();
        }
        break;
    }
    case OrderRequest::Type::CancelReplace: {
        auto* book = getOrderBook(req.symbolId);
        if (!book) {
            return;
        }
        if (book->cancelReplace(req.orderId, req.newPrice, req.newQty) && journal_) {
            {
                std::lock_guard<std::mutex> lock(journalMutex_);
                journal_->logCancelReplace(req.orderId, req.newPrice, req.newQty);
            }
            maybeTriggerAutoCheckpoint();
        }
        break;
    }
    case OrderRequest::Type::KillSwitch: {
        if (threadIndex >= symbolsByThread_.size()) {
            return;
        }
        for (SymbolId symbolId : symbolsByThread_[threadIndex]) {
            auto* book = getOrderBook(symbolId);
            if (!book) continue;
            if (req.participantId == kKillAllParticipants) {
                // P2-8: engine kill switch — cancel EVERY resting order on this
                // worker's books. The worker owns these books, so the non-locking
                // forEachOrder walk is safe; ids are collected first, then
                // cancelled (cancelOrder takes bookLock_, so it must run outside
                // the walk).
                std::vector<OrderId> ids;
                book->forEachOrder([&](const Order& o) { ids.push_back(o.id); });
                for (OrderId id : ids) {
                    if (positionLimitsActive_.load(std::memory_order_relaxed)) {
                        releasePosition(book->getOrder(id));
                    }
                    book->cancelOrder(id);
                    if (journal_) {
                        std::lock_guard<std::mutex> lock(journalMutex_);
                        journal_->logCancelOrder(id);
                    }
                }
            } else {
                book->cancelAllForParticipant(req.participantId);
            }
        }
        break;
    }
    case OrderRequest::Type::ExpireCheck: {
        if (threadIndex >= symbolsByThread_.size()) {
            return;
        }
        std::function<void(OrderId)> onExpire;
        if (journal_) {
            onExpire = [this](OrderId id) {
                std::lock_guard<std::mutex> lock(journalMutex_);
                journal_->logCancelOrder(id);
            };
        }
        for (SymbolId symbolId : symbolsByThread_[threadIndex]) {
            if (auto* book = getOrderBook(symbolId)) {
                book->expireOrders(req.expiryTime, onExpire);
            }
        }
        break;
    }
    default:
        break;
    }
}

void MatchingEngine::addSymbol(SymbolId symbolId, MatchAlgorithm algo) {
    // Public entry point: take bookMutex_, then delegate the mutation. Every
    // books_/symbolIds_/ocoListeners_ mutation runs under bookMutex_ so it
    // cannot rehash the FlatHashMap (freeing the bucket array) underneath a
    // concurrent off-hot-path reader (snapshot/checkpoint/replication apply)
    // iterating symbolIds_ or calling getOrderBook().
    std::lock_guard<std::mutex> lock(bookMutex_);
    addSymbolLocked(symbolId, algo);
}

void MatchingEngine::addSymbolLocked(SymbolId symbolId, MatchAlgorithm algo) {
    // Caller MUST hold bookMutex_.
    if (booksFrozen_.load(std::memory_order_acquire) || books_.contains(symbolId)) {
        return;
    }

    books_.insert(symbolId, std::make_unique<OrderBook>(symbolId, algo));
    symbolIds_.push_back(symbolId);

    // Install the per-book OCO observer (separate from the user listener
    // slot, so gateway/OUCH market-data listeners are untouched). It only
    // buffers while OCO is active, so this is free until an OCO is registered.
    auto oco = std::make_unique<OcoBookListener>();
    oco->engine = this;
    oco->ocoActive = &ocoActive_;
    oco->tradesActive = &observersActive_;
    // Pre-reserve the observer + drain buffers so a burst of fills in one
    // request does not heap-allocate on the worker hot path. driveOco then
    // reuses the buffers (swap + clear), keeping steady-state allocation-free.
    constexpr size_t kOcoBufferReserve = 256;
    oco->executed.reserve(kOcoBufferReserve);
    oco->trades.reserve(kOcoBufferReserve);
    oco->firedScratch.reserve(kOcoBufferReserve);
    oco->tradesScratch.reserve(kOcoBufferReserve);
    if (auto* bk = getOrderBook(symbolId)) bk->setEngineListener(oco.get());
    ocoListeners_.insert(symbolId, std::move(oco));

    rebuildThreadSymbolIndex();
}

void MatchingEngine::OcoBookListener::onOrderUpdate(const OrderUpdate& u) {
    // Buffer executions only while OCO is active; drained by driveOco after
    // the triggering request completes. A partial fill counts — the first
    // execution of any leg wins.
    if (ocoActive && ocoActive->load(std::memory_order_relaxed) &&
        (u.status == OrderStatus::Filled || u.status == OrderStatus::PartiallyFilled)) {
        executed.push_back(u.orderId);
    }
}

void MatchingEngine::OcoBookListener::onTrade(const Trade& t) {
    // P2-9/10/11 per-fill accrual (last-trade price, taker position, OTR trade
    // counts). Runs inline on the matching thread in both sync and async modes,
    // gated by a single atomic load so the no-risk hot path is untouched.
    if (engine && engine->anyRiskActive_.load(std::memory_order_relaxed)) {
        engine->onRiskFill(t);
    }
    // Buffer executed trades for the trade-driven consumers (fees, risk
    // position accrual, audit trail); drained by driveOco after the request.
    if (tradesActive && tradesActive->load(std::memory_order_relaxed)) {
        trades.push_back(t);
    }
}

void MatchingEngine::registerOco(SymbolId /*symbolId*/, OrderId a, OrderId b) {
    std::lock_guard<std::mutex> lock(contingencyMutex_);
    contingency_.registerOco(a, b);
    ocoActive_.store(true, std::memory_order_relaxed);
}

void MatchingEngine::driveOco(SymbolId symbolId, OrderBook* book) {
    auto* lp = ocoListeners_.find(symbolId);
    if (!lp || !*lp) return;
    OcoBookListener* obs = lp->get();

    // (1) OCO: cancel the siblings of any leg that executed. Drain first so
    // the cancels below (which re-enter the observer with Cancelled updates,
    // ignored) cannot grow the buffer mid-iteration.
    if (!obs->executed.empty()) {
        // Swap into the persistent scratch (so a re-entrant push during the
        // cancels below cannot grow the buffer mid-iteration), then clear it
        // afterwards to retain its capacity for the next cycle.
        obs->firedScratch.swap(obs->executed);
        for (OrderId id : obs->firedScratch) {
            std::vector<OrderId> siblings;
            {
                std::lock_guard<std::mutex> lock(contingencyMutex_);
                siblings = contingency_.onExecuted(id);
            }
            for (OrderId sib : siblings) {
                if (!book->getOrder(sib)) continue;   // already gone
                book->cancelOrder(sib);
                if (journal_) {
                    std::lock_guard<std::mutex> jl(journalMutex_);
                    journal_->logCancelOrder(sib);    // replay reproduces the OCO cancel
                }
            }
        }
        obs->firedScratch.clear();   // retain capacity for the next drain cycle
    }

    // (2) Trade-driven consumers: maker-taker fees, hierarchical-risk position
    // accrual, and the CAT audit trail. Each fill moves both participants.
    if (!obs->trades.empty()) {
        obs->tradesScratch.swap(obs->trades);
        for (const Trade& t : obs->tradesScratch) {
            if (feesActive_.load(std::memory_order_relaxed)) {
                const ParticipantId maker = (t.aggressorSide == Side::Buy) ? t.sellerId : t.buyerId;
                const ParticipantId taker = (t.aggressorSide == Side::Buy) ? t.buyerId : t.sellerId;
                std::lock_guard<std::mutex> lock(feeMutex_);
                feeEngine_.applyFill(maker, taker, t.price, t.quantity);
            }
            if (riskActive_.load(std::memory_order_relaxed)) {
                std::lock_guard<std::mutex> lock(riskMutex_);
                riskManager_.onFill(t.buyerId, Side::Buy, t.price, t.quantity);
                riskManager_.onFill(t.sellerId, Side::Sell, t.price, t.quantity);
            }
            if (catActive_.load(std::memory_order_relaxed)) {
                std::lock_guard<std::mutex> lock(catMutex_);
                catReporter_.recordTrade(t.timestamp, t.tradeId, t.buyOrderId,
                                         t.sellOrderId, t.symbolId, t.price, t.quantity);
            }
        }
        obs->tradesScratch.clear();  // retain capacity for the next drain cycle
    }
}

bool MatchingEngine::preTradeRiskCheck(ParticipantId trader, Side side, Price price,
                                       Quantity qty, Price referencePrice) {
    if (!riskActive_.load(std::memory_order_relaxed)) return true;
    std::lock_guard<std::mutex> lock(riskMutex_);
    return riskManager_.check(trader, side, price, qty, referencePrice).allowed;
}

// ─── Hot-path pre-trade risk controls (P2-8 … P2-11) ────────────────────────

uint64_t MatchingEngine::riskNow() const {
    return riskClock_ ? riskClock_() : nowNs();
}

void MatchingEngine::logRiskReject(const char* control, SymbolId sym,
                                   ParticipantId pid, RejectReason reason) {
    // Guarded on the sink-active flag: LogEvent construction heap-allocates, so
    // the reject path stays allocation-free when no structured sink is attached.
    if (!obSinkActive()) return;
    obSink().log(obEvent("risk_reject")
                     .kv("control", control)
                     .kv("symbol_id", static_cast<long long>(sym))
                     .kv("participant_id", static_cast<long long>(pid))
                     .kv("reason", static_cast<long long>(static_cast<int>(reason))));
}

void MatchingEngine::setKillSwitch(bool engaged) {
    // ── ORDER OF OPERATIONS (P2-8) ──────────────────────────────────────────
    // 1. Publish the flag with release semantics FIRST. From this instant every
    //    thread entering submitOrder observes the switch (acquire-load) and
    //    rejects new orders with KillSwitchActive. Doing this before the cancel
    //    sweep closes the race where a fresh order slips in between "cancel
    //    resting" and "set flag".
    // 2. THEN cancel all resting orders.
    // Deactivation (engaged == false) only clears the flag; it does not resurrect
    // cancelled orders.
    killSwitchActive_.store(engaged, std::memory_order_release);
    if (!engaged) return;

    if (async_) {
        // Book mutations must run on the owning worker in async mode. Dispatch a
        // cancel-all request to every worker and wait for the sweep to drain.
        for (size_t i = 0; i < numThreads_; ++i) {
            OrderRequest req{};
            req.type = OrderRequest::Type::KillSwitch;
            req.participantId = kKillAllParticipants;
            enqueueSafe(i, req);
        }
        waitForDrain();
    } else {
        cancelAllRestingOrders();
    }
}

void MatchingEngine::cancelAllRestingOrders() {
    // Sync-mode kill-switch sweep. Lock order bookMutex_ -> bookLock_ ->
    // journalMutex_, matching expireOrders()/driveOco(). Ids are collected under
    // the book's shared lock (forEachOrderLocked), then cancelled outside it so
    // cancelOrder can take bookLock_ exclusively without re-entrancy.
    std::lock_guard<std::mutex> lock(bookMutex_);
    for (SymbolId sym : symbolIds_) {
        auto* book = getOrderBook(sym);
        if (!book) continue;
        std::vector<OrderId> ids;
        book->forEachOrderLocked([&](const Order& o) { ids.push_back(o.id); });
        for (OrderId id : ids) {
            if (positionLimitsActive_.load(std::memory_order_relaxed)) {
                releasePosition(book->getOrder(id));
            }
            book->cancelOrder(id);
            if (journal_) {
                std::lock_guard<std::mutex> jl(journalMutex_);
                journal_->logCancelOrder(id);
            }
        }
    }
}

void MatchingEngine::setPositionLimit(ParticipantId pid, int64_t maxAbsPosition) {
    if (pid >= MAX_PARTICIPANTS) return;
    positionLimit_[pid].store(maxAbsPosition, std::memory_order_relaxed);
    positionLimitsActive_.store(true, std::memory_order_relaxed);
    anyRiskActive_.store(true, std::memory_order_relaxed);
}

int64_t MatchingEngine::getPosition(ParticipantId pid) const {
    if (pid >= MAX_PARTICIPANTS) return 0;
    return positions_[pid].load(std::memory_order_relaxed);
}

void MatchingEngine::reservePosition(ParticipantId pid, Side side, Quantity restingQty) {
    if (pid >= MAX_PARTICIPANTS || restingQty == 0) return;
    int64_t signedRest = (side == Side::Buy) ? static_cast<int64_t>(restingQty)
                                             : -static_cast<int64_t>(restingQty);
    positions_[pid].fetch_add(signedRest, std::memory_order_relaxed);
}

void MatchingEngine::releasePosition(const Order* order) {
    if (!order || order->participantId >= MAX_PARTICIPANTS || order->remainingQty == 0) return;
    int64_t signedRest = (order->side == Side::Buy) ? static_cast<int64_t>(order->remainingQty)
                                                    : -static_cast<int64_t>(order->remainingQty);
    positions_[order->participantId].fetch_sub(signedRest, std::memory_order_relaxed);
}

void MatchingEngine::setFatFingerLimits(SymbolId sym, Quantity maxQty,
                                        double maxDeviationPct, int64_t maxNotional) {
    if (sym >= MAX_RISK_SYMBOLS) return;
    ffMaxQty_[sym].store(maxQty, std::memory_order_relaxed);
    ffMaxDeviationBps_[sym].store(
        static_cast<int64_t>(maxDeviationPct * 10000.0 + 0.5), std::memory_order_relaxed);
    ffMaxNotional_[sym].store(maxNotional, std::memory_order_relaxed);
    fatFingerActive_.store(true, std::memory_order_relaxed);
    anyRiskActive_.store(true, std::memory_order_relaxed);
}

void MatchingEngine::setReferencePrice(SymbolId sym, Price px) {
    if (sym >= MAX_RISK_SYMBOLS) return;
    refPrice_[sym].store(px, std::memory_order_relaxed);
}

void MatchingEngine::setOtrLimit(double maxRatio, uint64_t windowMs, uint64_t minOrders) {
    otrMaxRatio_ = maxRatio;
    otrWindowNs_ = windowMs * 1'000'000ull;
    otrMinOrders_ = minOrders;
    otrActive_.store(maxRatio > 0.0, std::memory_order_relaxed);
    if (maxRatio > 0.0) anyRiskActive_.store(true, std::memory_order_relaxed);
}

RejectReason MatchingEngine::checkRiskControls(SymbolId sym, ParticipantId pid, Side side,
                                               Price price, Quantity qty, OrderType type) {
    // ── P2-10 FAT-FINGER (per instrument) ───────────────────────────────────
    if (fatFingerActive_.load(std::memory_order_relaxed) && sym < MAX_RISK_SYMBOLS) {
        const uint64_t maxQty = ffMaxQty_[sym].load(std::memory_order_relaxed);
        if (maxQty != 0 && qty > maxQty) {
            fatFingerRejects_.fetch_add(1, std::memory_order_relaxed);
            logRiskReject("fat_finger_qty", sym, pid, RejectReason::FatFingerReject);
            return RejectReason::FatFingerReject;
        }
        const int64_t maxNotional = ffMaxNotional_[sym].load(std::memory_order_relaxed);
        if (maxNotional != 0 && price > 0) {
            // __int128 avoids overflow for price*qty at venue scale.
            const __int128 notional = static_cast<__int128>(price) * static_cast<__int128>(qty);
            if (notional > static_cast<__int128>(maxNotional)) {
                fatFingerRejects_.fetch_add(1, std::memory_order_relaxed);
                logRiskReject("fat_finger_notional", sym, pid, RejectReason::FatFingerReject);
                return RejectReason::FatFingerReject;
            }
        }
        // Market orders carry no limit price, so the ±deviation band does not apply.
        const int64_t devBps = ffMaxDeviationBps_[sym].load(std::memory_order_relaxed);
        const Price ref = refPrice_[sym].load(std::memory_order_relaxed);
        if (devBps != 0 && ref > 0 && price > 0 && type != OrderType::Market) {
            const int64_t diff = price > ref ? price - ref : ref - price;
            // |price-ref|/ref > devBps/10000  ⇔  diff*10000 > devBps*ref
            if (static_cast<__int128>(diff) * 10000 >
                static_cast<__int128>(devBps) * static_cast<__int128>(ref)) {
                fatFingerRejects_.fetch_add(1, std::memory_order_relaxed);
                logRiskReject("fat_finger_band", sym, pid, RejectReason::FatFingerReject);
                return RejectReason::FatFingerReject;
            }
        }
    }

    // ── P2-9 POSITION LIMIT (per participant, net signed exposure) ──────────
    if (positionLimitsActive_.load(std::memory_order_relaxed) && pid < MAX_PARTICIPANTS) {
        const int64_t limit = positionLimit_[pid].load(std::memory_order_relaxed);
        if (limit > 0) {
            const int64_t signedQty = (side == Side::Buy) ? static_cast<int64_t>(qty)
                                                          : -static_cast<int64_t>(qty);
            const int64_t projected = positions_[pid].load(std::memory_order_relaxed) + signedQty;
            if (projected > limit || projected < -limit) {
                positionRejects_.fetch_add(1, std::memory_order_relaxed);
                logRiskReject("position_limit", sym, pid, RejectReason::PositionLimitExceeded);
                return RejectReason::PositionLimitExceeded;
            }
        }
    }

    // ── P2-11 ORDER-TO-TRADE RATIO (per participant, rolling window) ────────
    if (otrActive_.load(std::memory_order_relaxed) && pid < MAX_PARTICIPANTS &&
        otrMaxRatio_ > 0.0) {
        const uint64_t now = riskNow();
        uint64_t start = otrWindowStart_[pid].load(std::memory_order_relaxed);
        if (now - start > otrWindowNs_) {
            // Roll the window; a single CAS winner resets the counters.
            if (otrWindowStart_[pid].compare_exchange_strong(start, now,
                                                             std::memory_order_relaxed)) {
                otrOrders_[pid].store(0, std::memory_order_relaxed);
                otrTrades_[pid].store(0, std::memory_order_relaxed);
            }
        }
        const uint64_t orders = otrOrders_[pid].fetch_add(1, std::memory_order_relaxed) + 1;
        const uint64_t trades = otrTrades_[pid].load(std::memory_order_relaxed);
        // Throttle once past the sample floor and orders/trades exceeds the cap.
        // Integer form of orders/trades > maxRatio (trades == 0 ⇒ any orders trip).
        if (orders >= otrMinOrders_ &&
            static_cast<double>(orders) > otrMaxRatio_ * static_cast<double>(trades)) {
            otrRejects_.fetch_add(1, std::memory_order_relaxed);
            logRiskReject("otr", sym, pid, RejectReason::OrderToTradeRatioExceeded);
            return RejectReason::OrderToTradeRatioExceeded;
        }
    }

    return RejectReason::None;
}

void MatchingEngine::onRiskFill(const Trade& t) {
    // Called inline from OcoBookListener::onTrade on every fill (both engine
    // modes), gated upstream by anyRiskActive_. All updates are atomic.

    // P2-10: track the last trade price as the fat-finger deviation reference.
    if (fatFingerActive_.load(std::memory_order_relaxed) && t.symbolId < MAX_RISK_SYMBOLS) {
        refPrice_[t.symbolId].store(t.price, std::memory_order_relaxed);
    }

    // P2-9: increment the TAKER's position by the filled qty. The maker's
    // exposure was reserved at its own submit (working) and merely converts to a
    // filled position here — no net change — so only the taker is accrued,
    // keeping reserved + filled == full order qty for both sides.
    if (positionLimitsActive_.load(std::memory_order_relaxed)) {
        const ParticipantId taker = (t.aggressorSide == Side::Buy) ? t.buyerId : t.sellerId;
        const int64_t signedQ = (t.aggressorSide == Side::Buy)
                                    ? static_cast<int64_t>(t.quantity)
                                    : -static_cast<int64_t>(t.quantity);
        if (taker < MAX_PARTICIPANTS) {
            positions_[taker].fetch_add(signedQ, std::memory_order_relaxed);
        }
    }

    // P2-11: a fill counts as a trade for both counterparties' OTR windows.
    if (otrActive_.load(std::memory_order_relaxed)) {
        if (t.buyerId < MAX_PARTICIPANTS) {
            otrTrades_[t.buyerId].fetch_add(1, std::memory_order_relaxed);
        }
        if (t.sellerId < MAX_PARTICIPANTS) {
            otrTrades_[t.sellerId].fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void MatchingEngine::mapParticipant(ParticipantId trader, uint64_t strategyId,
                                    uint64_t accountId, uint64_t firmId) {
    {
        std::lock_guard<std::mutex> lock(riskMutex_);
        riskManager_.mapParticipant(trader, strategyId, accountId, firmId);
    }
    riskActive_.store(true, std::memory_order_relaxed);
    observersActive_.store(true, std::memory_order_relaxed);
}

void MatchingEngine::setRiskTierLimits(RiskTier tier, uint64_t entityId, const TierLimits& limits) {
    {
        std::lock_guard<std::mutex> lock(riskMutex_);
        riskManager_.setLimits(tier, entityId, limits);
    }
    riskActive_.store(true, std::memory_order_relaxed);
    observersActive_.store(true, std::memory_order_relaxed);
}

int64_t MatchingEngine::riskNetPosition(RiskTier tier, uint64_t entityId) const {
    std::lock_guard<std::mutex> lock(riskMutex_);
    return riskManager_.netPosition(tier, entityId);
}

void MatchingEngine::setDefaultFeeSchedule(const FeeSchedule& s) {
    {
        std::lock_guard<std::mutex> lock(feeMutex_);
        feeEngine_.setDefaultSchedule(s);
    }
    feesActive_.store(true, std::memory_order_relaxed);
    observersActive_.store(true, std::memory_order_relaxed);
}

void MatchingEngine::setParticipantFeeSchedule(ParticipantId p, const FeeSchedule& s) {
    {
        std::lock_guard<std::mutex> lock(feeMutex_);
        feeEngine_.setParticipantSchedule(p, s);
    }
    feesActive_.store(true, std::memory_order_relaxed);
    observersActive_.store(true, std::memory_order_relaxed);
}

int64_t MatchingEngine::accruedFee(ParticipantId p) const {
    std::lock_guard<std::mutex> lock(feeMutex_);
    return feeEngine_.accruedFee(p);
}

void MatchingEngine::enableAuditTrail(bool on) {
    catActive_.store(on, std::memory_order_relaxed);
    if (on) observersActive_.store(true, std::memory_order_relaxed);
}

size_t MatchingEngine::auditRecordCount() const {
    std::lock_guard<std::mutex> lock(catMutex_);
    return catReporter_.recordCount();
}

std::string MatchingEngine::auditTrailJsonl() const {
    std::lock_guard<std::mutex> lock(catMutex_);
    return catReporter_.toJsonl();
}

OrderBook* MatchingEngine::getOrderBook(SymbolId symbolId) {
    auto* book = books_.find(symbolId);
    return book ? book->get() : nullptr;
}

const OrderBook* MatchingEngine::getOrderBook(SymbolId symbolId) const {
    const auto* book = books_.find(symbolId);
    return book ? book->get() : nullptr;
}

void MatchingEngine::processOrder(SymbolId symbolId, OrderId orderId,
                                  ParticipantId participantId, Side side, Price price,
                                  Quantity qty, OrderType type, Price stopPrice,
                                  Quantity displayQty, TimeInForce tif,
                                  uint64_t expiryTime, Price stopLimitPrice,
                                  PegType pegType, Price pegOffset, Price trailAmount,
                                  Quantity minQty, bool hidden) {
    (void)submitOrder(symbolId, orderId, participantId, side, price, qty, type, stopPrice,
                      displayQty, tif, expiryTime, stopLimitPrice, pegType, pegOffset,
                      trailAmount, minQty, hidden);
}

SubmitResult MatchingEngine::submitOrder(SymbolId symbolId, OrderId orderId,
                                         ParticipantId participantId, Side side, Price price,
                                         Quantity qty, OrderType type, Price stopPrice,
                                         Quantity displayQty, TimeInForce tif,
                                         uint64_t expiryTime, Price stopLimitPrice,
                                         PegType pegType, Price pegOffset, Price trailAmount,
                                         Quantity minQty, bool hidden) {
    // P2-8 KILL SWITCH — the very first thing checked. Settable from a
    // monitoring thread; acquire-load pairs with the release-store in
    // setKillSwitch() so a busy matching thread observes activation promptly.
    if (killSwitchActive_.load(std::memory_order_acquire)) [[unlikely]] {
        killSwitchRejects_.fetch_add(1, std::memory_order_relaxed);
        logRiskReject("kill_switch", symbolId, participantId, RejectReason::KillSwitchActive);
        return rejectedAsync(RejectReason::KillSwitchActive);
    }

    if (!running_.load(std::memory_order_acquire)) {
        return rejectedAsync(RejectReason::EngineStopped);
    }

    uint64_t sequenceId = nextSubmitSequence_.fetch_add(1, std::memory_order_relaxed);

    if (rateLimiter_.isEnabled() && !rateLimiter_.allow(participantId)) {
        rateLimitedCount_.fetch_add(1, std::memory_order_relaxed);
        return rejectedAsync(RejectReason::RateLimitExceeded);
    }

    // P2-9/10/11 pre-trade risk gate (fat-finger, position, OTR). Enforced at
    // the submitOrder entry point in BOTH sync and async modes, before the order
    // is enqueued or booked. Skipped entirely (one atomic load) when unconfigured.
    if (anyRiskActive_.load(std::memory_order_relaxed)) {
        RejectReason rr = checkRiskControls(symbolId, participantId, side, price, qty, type);
        if (rr != RejectReason::None) [[unlikely]] {
            return rejectedAsync(rr);
        }
    }

    if (async_) {
        if (!getOrderBook(symbolId)) {
            return rejectedAsync(RejectReason::SymbolNotFound);
        }

        OrderRequest req{};
        req.type = OrderRequest::Type::NewOrder;
        req.symbolId = symbolId;
        req.orderId = orderId;
        req.participantId = participantId;
        req.side = side;
        req.price = price;
        req.qty = qty;
        req.orderType = type;
        req.stopPrice = stopPrice;
        req.displayQty = displayQty;
        req.tif = tif;
        req.expiryTime = expiryTime;
        req.stopLimitPrice = stopLimitPrice;
        req.pegType = pegType;
        req.pegOffset = pegOffset;
        req.trailAmount = trailAmount;
        req.minQty = minQty;
        req.hidden = hidden;
        req.ingressNs = nowNs();
        if (!enqueueSafe(getThreadIndex(symbolId), req)) {
            return rejectedAsync(RejectReason::QueueBackpressure);
        }
        return acceptedAsync(sequenceId);
    }

    auto* book = getOrderBook(symbolId);
    if (!book) {
        return rejectedAsync(RejectReason::SymbolNotFound);
    }

    AddOrderResult result = book->addOrder(orderId, participantId, side, price, qty,
                                           type, stopPrice, displayQty, tif, expiryTime,
                                           stopLimitPrice, pegType, pegOffset, trailAmount,
                                           minQty, hidden);
    // P2-9: reserve the resting remainder as working exposure. Any portion that
    // filled immediately was already accrued to this (taker) participant by
    // onRiskFill during the match, so reserving only the remainder avoids double
    // counting — reserved + filled == full order qty.
    if (positionLimitsActive_.load(std::memory_order_relaxed) &&
        std::holds_alternative<OrderId>(result)) {
        const Order* o = book->getOrder(orderId);
        reservePosition(participantId, side, o ? o->remainingQty : 0);
    }
    if (journal_ && std::holds_alternative<OrderId>(result)) {
        {
            std::lock_guard<std::mutex> lock(journalMutex_);
            journal_->logAddOrder(orderId, participantId, symbolId, side, price, qty, type, tif,
                                  expiryTime, stopPrice, stopLimitPrice, displayQty, pegType,
                                  pegOffset, trailAmount, minQty, hidden);
        }
        maybeTriggerAutoCheckpoint();
    }
    return orderBookResultToSubmitResult(result, sequenceId);
}

void MatchingEngine::cancelOrder(SymbolId symbolId, OrderId orderId) {
    (void)submitCancel(symbolId, orderId);
}

SubmitResult MatchingEngine::submitCancel(SymbolId symbolId, OrderId orderId) {
    if (!running_.load(std::memory_order_acquire)) {
        return rejectedAsync(RejectReason::EngineStopped);
    }

    uint64_t sequenceId = nextSubmitSequence_.fetch_add(1, std::memory_order_relaxed);

    if (async_) {
        if (!getOrderBook(symbolId)) {
            return rejectedAsync(RejectReason::SymbolNotFound);
        }

        OrderRequest req{};
        req.type = OrderRequest::Type::Cancel;
        req.symbolId = symbolId;
        req.orderId = orderId;
        req.ingressNs = nowNs();
        if (!enqueueSafe(getThreadIndex(symbolId), req)) {
            return rejectedAsync(RejectReason::QueueBackpressure);
        }
        return acceptedAsync(sequenceId);
    }

    auto* book = getOrderBook(symbolId);
    if (!book || !book->getOrder(orderId)) {
        return rejectedAsync(book ? RejectReason::OrderNotFound
                                           : RejectReason::SymbolNotFound);
    }
    // P2-9: release the cancelled order's working exposure before it leaves the
    // book (read remaining qty while the order still exists).
    if (positionLimitsActive_.load(std::memory_order_relaxed)) {
        releasePosition(book->getOrder(orderId));
    }
    book->cancelOrder(orderId);
    if (journal_) {
        {
            std::lock_guard<std::mutex> lock(journalMutex_);
            journal_->logCancelOrder(orderId);
        }
        maybeTriggerAutoCheckpoint();
    }
    return acceptedAsync(sequenceId);
}

bool MatchingEngine::modifyOrder(SymbolId symbolId, OrderId orderId, Quantity newQty) {
    return submitModify(symbolId, orderId, newQty).isAccepted();
}

SubmitResult MatchingEngine::submitModify(SymbolId symbolId, OrderId orderId, Quantity newQty) {
    if (!running_.load(std::memory_order_acquire)) {
        return rejectedAsync(RejectReason::EngineStopped);
    }

    uint64_t sequenceId = nextSubmitSequence_.fetch_add(1, std::memory_order_relaxed);

    if (async_) {
        if (!getOrderBook(symbolId)) {
            return rejectedAsync(RejectReason::SymbolNotFound);
        }

        OrderRequest req{};
        req.type = OrderRequest::Type::Modify;
        req.symbolId = symbolId;
        req.orderId = orderId;
        req.newQty = newQty;
        req.ingressNs = nowNs();
        if (!enqueueSafe(getThreadIndex(symbolId), req)) {
            return rejectedAsync(RejectReason::QueueBackpressure);
        }
        return acceptedAsync(sequenceId);
    }

    auto* book = getOrderBook(symbolId);
    if (!book) {
        return rejectedAsync(RejectReason::SymbolNotFound);
    }
    bool modified = book->modifyOrder(orderId, newQty);
    if (modified && journal_) {
        {
            std::lock_guard<std::mutex> lock(journalMutex_);
            journal_->logModifyOrder(orderId, newQty);
        }
        maybeTriggerAutoCheckpoint();
    }
    return modified ? SubmitResult::accepted(sequenceId)
                    : SubmitResult::rejected(RejectReason::OrderNotFound);
}

bool MatchingEngine::cancelReplace(SymbolId symbolId, OrderId orderId, Price newPrice,
                                   Quantity newQty) {
    return submitCancelReplace(symbolId, orderId, newPrice, newQty).isAccepted();
}

SubmitResult MatchingEngine::submitCancelReplace(SymbolId symbolId, OrderId orderId,
                                                 Price newPrice, Quantity newQty) {
    if (!running_.load(std::memory_order_acquire)) {
        return rejectedAsync(RejectReason::EngineStopped);
    }

    uint64_t sequenceId = nextSubmitSequence_.fetch_add(1, std::memory_order_relaxed);

    if (async_) {
        if (!getOrderBook(symbolId)) {
            return rejectedAsync(RejectReason::SymbolNotFound);
        }

        OrderRequest req{};
        req.type = OrderRequest::Type::CancelReplace;
        req.symbolId = symbolId;
        req.orderId = orderId;
        req.newPrice = newPrice;
        req.newQty = newQty;
        req.ingressNs = nowNs();
        if (!enqueueSafe(getThreadIndex(symbolId), req)) {
            return rejectedAsync(RejectReason::QueueBackpressure);
        }
        return acceptedAsync(sequenceId);
    }

    auto* book = getOrderBook(symbolId);
    if (!book) {
        return rejectedAsync(RejectReason::SymbolNotFound);
    }

    bool replaced = book->cancelReplace(orderId, newPrice, newQty);
    if (replaced && journal_) {
        {
            std::lock_guard<std::mutex> lock(journalMutex_);
            journal_->logCancelReplace(orderId, newPrice, newQty);
        }
        maybeTriggerAutoCheckpoint();
    }
    return replaced ? SubmitResult::accepted(sequenceId)
                    : SubmitResult::rejected(RejectReason::OrderNotFound);
}

uint64_t MatchingEngine::killSwitch(ParticipantId participantId) {
    if (async_) {
        for (size_t i = 0; i < numThreads_; ++i) {
            OrderRequest req{};
            req.type = OrderRequest::Type::KillSwitch;
            req.participantId = participantId;
            enqueueSafe(i, req);
        }
        waitForDrain();
        return 0;
    }

    // Sync path: iterate symbolIds_ under bookMutex_ so a concurrent addSymbol
    // (e.g. a sync-mode replication apply) cannot reallocate the vector / books_
    // map mid-iteration. Lock order bookMutex_ -> bookLock_.
    uint64_t total = 0;
    std::lock_guard<std::mutex> lock(bookMutex_);
    for (SymbolId symbolId : symbolIds_) {
        if (auto* book = getOrderBook(symbolId)) {
            total += book->cancelAllForParticipant(participantId);
        }
    }
    return total;
}

void MatchingEngine::setRiskLimits(SymbolId symbolId, ParticipantId participantId,
                                   const RiskLimits& limits) {
    if (auto* book = getOrderBook(symbolId)) {
        book->setRiskLimits(participantId, limits);
    }
}

void MatchingEngine::setParticipantRole(ParticipantId p, ParticipantRole role) {
    std::lock_guard<std::mutex> lock(bookMutex_);
    for (auto& sym : symbolIds_) {
        if (auto* bk = getOrderBook(sym)) {
            bk->setParticipantRole(p, role);
        }
    }
}

MarketDataSnapshot MatchingEngine::getSnapshot(SymbolId symbolId, size_t depth) {
    if (auto* book = getOrderBook(symbolId)) {
        return book->getSnapshot(depth);
    }
    return {};
}

void MatchingEngine::uncross(SymbolId symbolId) {
    if (auto* book = getOrderBook(symbolId)) {
        book->uncross();
    }
}

size_t MatchingEngine::setTradingStateBatch(
        const std::vector<SymbolId>& symbols, TradingState state) {
    // Hold booksMutex_ for the whole batch. Per-book setTradingState is
    // a single atomic field assignment, so the lock just enforces that
    // any concurrent observer sees either the pre-batch or post-batch
    // configuration — never a mix.
    std::lock_guard<std::mutex> lock(bookMutex_);
    size_t transitioned = 0;
    for (SymbolId s : symbols) {
        if (auto* book = getOrderBook(s)) {
            book->setTradingState(state);
            ++transitioned;
        }
    }
    return transitioned;
}

size_t MatchingEngine::uncrossBatch(const std::vector<SymbolId>& symbols) {
    std::lock_guard<std::mutex> lock(bookMutex_);
    size_t crossed = 0;
    for (SymbolId s : symbols) {
        if (auto* book = getOrderBook(s)) {
            book->uncross();
            ++crossed;
        }
    }
    return crossed;
}

size_t MatchingEngine::resumeVolatilityAuctions() {
    std::lock_guard<std::mutex> lock(bookMutex_);
    size_t resumed = 0;
    for (SymbolId s : symbolIds_) {
        if (auto* book = getOrderBook(s)) {
            if (book->resumeVolatilityAuction()) ++resumed;
        }
    }
    return resumed;
}

void MatchingEngine::expireOrders(uint64_t currentTime) {
    // Sync-mode expiry path. Iterates symbolIds_, so hold bookMutex_ to stay
    // consistent against a concurrent addSymbol (expiry timer thread vs. a
    // symbol-management mutation). Lock order bookMutex_ -> book->bookLock_ ->
    // journalMutex_ (the onExpire callback logs under journalMutex_); sync-mode
    // only, where no async worker runs, so it cannot deadlock the worker path.
    std::lock_guard<std::mutex> lock(bookMutex_);

    // If the journal is enabled, log each expiration as a CancelOrder
    // BEFORE the cancel actually runs. Replay then reproduces these
    // expirations deterministically without needing a virtual clock —
    // the journal entries appear in their original sequence position.
    std::function<void(OrderId)> onExpire;
    if (journal_) {
        onExpire = [this](OrderId id) {
            std::lock_guard<std::mutex> lock(journalMutex_);
            journal_->logCancelOrder(id);
        };
    }
    for (SymbolId symbolId : symbolIds_) {
        if (auto* book = getOrderBook(symbolId)) {
            book->expireOrders(currentTime, onExpire);
        }
    }
}

void MatchingEngine::enableJournal(const std::string& path) {
    std::lock_guard<std::mutex> lock(journalMutex_);
    journal_ = std::make_unique<Journal>(path, Journal::SyncPolicy::GroupCommit, 64);
}

size_t MatchingEngine::replayJournal() {
    if (!journal_) {
        return 0;
    }

    for (SymbolId symbolId : symbolIds_) {
        if (auto* book = getOrderBook(symbolId)) {
            book->setReplayMode(true);
        }
    }

    std::vector<JournalEntry> entries;
    {
        std::lock_guard<std::mutex> lock(journalMutex_);
        entries = journal_->readAll(true, true);
    }
    size_t replayed = 0;
    uint64_t lastSequence = 0;

    for (const auto& entry : entries) {
        if (entry.sequenceNumber <= lastSequence) {
            continue;
        }
        lastSequence = entry.sequenceNumber;

        switch (entry.entryType) {
        case JournalEntry::Type::AddOrder: {
            auto* book = getOrderBook(entry.symbolId);
            if (!book) {
                addSymbol(entry.symbolId);
                book = getOrderBook(entry.symbolId);
                if (book) {
                    book->setReplayMode(true);
                }
            }
            if (book) {
                book->addOrder(entry.orderId, entry.participantId, entry.side,
                               entry.price, entry.quantity, entry.orderType,
                               entry.stopPrice, entry.displayQty, entry.timeInForce,
                               entry.expiryTime, entry.stopLimitPrice, entry.pegType,
                               entry.pegOffset, entry.trailAmount, entry.minQty,
                               entry.hidden);
            }
            break;
        }
        case JournalEntry::Type::CancelOrder: {
            for (SymbolId symbolId : symbolIds_) {
                auto* book = getOrderBook(symbolId);
                if (book && book->getOrder(entry.orderId)) {
                    book->cancelOrder(entry.orderId);
                    break;
                }
            }
            break;
        }
        case JournalEntry::Type::ModifyOrder: {
            for (SymbolId symbolId : symbolIds_) {
                auto* book = getOrderBook(symbolId);
                if (book && book->getOrder(entry.orderId)) {
                    book->modifyOrder(entry.orderId, entry.newQty);
                    break;
                }
            }
            break;
        }
        case JournalEntry::Type::CancelReplace: {
            for (SymbolId symbolId : symbolIds_) {
                auto* book = getOrderBook(symbolId);
                if (book && book->getOrder(entry.orderId)) {
                    book->cancelReplace(entry.orderId, entry.newPrice, entry.newQty);
                    break;
                }
            }
            break;
        }
        case JournalEntry::Type::Snapshot: {
            auto* book = getOrderBook(entry.symbolId);
            if (!book) {
                addSymbol(entry.symbolId);
                book = getOrderBook(entry.symbolId);
                if (book) {
                    book->setReplayMode(true);
                }
            }
            if (book && !book->getOrder(entry.orderId)) {
                book->addOrder(entry.orderId, entry.participantId, entry.side,
                               entry.price, entry.quantity, entry.orderType,
                               entry.stopPrice, entry.displayQty, entry.timeInForce,
                               entry.expiryTime, entry.stopLimitPrice, entry.pegType,
                               entry.pegOffset, entry.trailAmount, entry.minQty,
                               entry.hidden);
            }
            break;
        }
        }

        ++replayed;
    }

    for (SymbolId symbolId : symbolIds_) {
        if (auto* book = getOrderBook(symbolId)) {
            book->setReplayMode(false);
        }
    }

    return replayed;
}

void MatchingEngine::setReplayModeAllBooks(bool replay) {
    // Can run on the promotion path concurrently with the replication apply
    // thread (which adds symbols), so iterate symbolIds_ under bookMutex_.
    std::lock_guard<std::mutex> lock(bookMutex_);
    for (SymbolId symbolId : symbolIds_) {
        if (auto* book = getOrderBook(symbolId)) {
            book->setReplayMode(replay);
        }
    }
}

void MatchingEngine::streamSnapshot(
        const std::function<void(const JournalEntry&)>& fn) const {
    if (!fn) return;
    // Runs on the replication coordinator thread while workers mutate books.
    // Hold bookMutex_ for a consistent symbolIds_/books_ view, and iterate each
    // book under its own lock via forEachOrderLocked. Lock order: bookMutex_ ->
    // book->bookLock_.
    std::lock_guard<std::mutex> lock(bookMutex_);
    for (SymbolId symbolId : symbolIds_) {
        const auto* book = getOrderBook(symbolId);
        if (!book) continue;
        book->forEachOrderLocked([&](const Order& o) {
            JournalEntry e{};
            e.entryType    = JournalEntry::Type::Snapshot;
            e.orderId      = o.id;
            e.participantId = o.participantId;
            e.symbolId     = symbolId;
            e.side         = o.side;
            e.price        = o.price;
            // Use remainingQty so the snapshot reflects the live
            // state (partial fills already applied), not the original
            // submission size. Re-applying a Snapshot on backup uses
            // this as the order's qty.
            e.quantity     = o.remainingQty;
            e.orderType    = o.type;
            e.timeInForce  = o.timeInForce;
            e.expiryTime   = o.expiryTime;
            e.stopPrice    = o.stopPrice;
            e.stopLimitPrice = o.stopLimitPrice;
            e.displayQty   = o.displayQty;
            e.pegType      = o.pegType;
            e.pegOffset    = o.pegOffset;
            e.trailAmount  = o.trailAmount;
            e.minQty       = o.minQty;
            e.hidden       = o.isHidden;
            fn(e);
        });
    }
}

bool MatchingEngine::applyReplicatedEntry(const JournalEntry& entry) {
    // Mirrors the per-entry dispatch in replayJournal(), but driven
    // by network-delivered entries instead of disk replay. Backups
    // are expected to be in replay mode (see setReplayModeAllBooks)
    // so order updates and market data are suppressed locally — the
    // primary is the canonical source. When the backup has its own
    // journal enabled (the production posture), the entry is ALSO
    // persisted to the backup's local journal so the backup retains
    // durability of received entries across its own restarts — and
    // so the chaos suite can observe replication progress via
    // /journal/head on the backup.
    // The replication receive thread mutates books_/symbolIds_ via ensureBook
    // (addSymbol) and iterates symbolIds_ in the Cancel/Modify branches. Hold
    // bookMutex_ across the whole apply so neither the structural mutation nor
    // the iterations race a concurrent symbol add / off-hot-path reader. Lock
    // order bookMutex_ -> book->bookLock_ (taken inside addOrder/cancelOrder),
    // matching streamSnapshot/checkpoint; no path takes a book lock before
    // bookMutex_, so there is no inversion. ensureBook uses addSymbolLocked
    // because we already hold bookMutex_.
    std::lock_guard<std::mutex> booksLock(bookMutex_);

    auto ensureBook = [this](SymbolId sym) -> OrderBook* {
        auto* book = getOrderBook(sym);
        if (!book) {
            addSymbolLocked(sym);
            book = getOrderBook(sym);
            if (book) book->setReplayMode(true);
        }
        return book;
    };

    bool applied = false;
    switch (entry.entryType) {
    case JournalEntry::Type::AddOrder: {
        auto* book = ensureBook(entry.symbolId);
        if (!book) return false;
        book->addOrder(entry.orderId, entry.participantId, entry.side,
                       entry.price, entry.quantity, entry.orderType,
                       entry.stopPrice, entry.displayQty, entry.timeInForce,
                       entry.expiryTime, entry.stopLimitPrice, entry.pegType,
                       entry.pegOffset, entry.trailAmount, entry.minQty,
                       entry.hidden);
        applied = true;
        break;
    }
    case JournalEntry::Type::CancelOrder: {
        for (SymbolId sym : symbolIds_) {
            auto* book = getOrderBook(sym);
            if (book && book->getOrder(entry.orderId)) {
                book->cancelOrder(entry.orderId);
                applied = true;
                break;
            }
        }
        break;
    }
    case JournalEntry::Type::ModifyOrder: {
        for (SymbolId sym : symbolIds_) {
            auto* book = getOrderBook(sym);
            if (book && book->getOrder(entry.orderId)) {
                book->modifyOrder(entry.orderId, entry.newQty);
                applied = true;
                break;
            }
        }
        break;
    }
    case JournalEntry::Type::CancelReplace: {
        for (SymbolId sym : symbolIds_) {
            auto* book = getOrderBook(sym);
            if (book && book->getOrder(entry.orderId)) {
                book->cancelReplace(entry.orderId, entry.newPrice, entry.newQty);
                applied = true;
                break;
            }
        }
        break;
    }
    case JournalEntry::Type::Snapshot: {
        auto* book = ensureBook(entry.symbolId);
        if (!book) return false;
        if (!book->getOrder(entry.orderId)) {
            book->addOrder(entry.orderId, entry.participantId, entry.side,
                           entry.price, entry.quantity, entry.orderType,
                           entry.stopPrice, entry.displayQty, entry.timeInForce,
                           entry.expiryTime, entry.stopLimitPrice, entry.pegType,
                           entry.pegOffset, entry.trailAmount, entry.minQty,
                           entry.hidden);
        }
        applied = true;
        break;
    }
    }

    // Persist to backup's local journal if one is configured. The
    // backup's Journal::onCommit is NOT hooked (only the primary's
    // is, to ship to peers), so this write does not re-replicate
    // back to the primary — no echo loop.
    if (applied && journal_) {
        switch (entry.entryType) {
        case JournalEntry::Type::AddOrder:
            journal_->logAddOrder(entry.orderId, entry.participantId,
                                  entry.symbolId, entry.side, entry.price,
                                  entry.quantity, entry.orderType,
                                  entry.timeInForce, entry.expiryTime,
                                  entry.stopPrice, entry.stopLimitPrice,
                                  entry.displayQty, entry.pegType,
                                  entry.pegOffset, entry.trailAmount,
                                  entry.minQty, entry.hidden);
            break;
        case JournalEntry::Type::CancelOrder:
            journal_->logCancelOrder(entry.orderId);
            break;
        case JournalEntry::Type::ModifyOrder:
            journal_->logModifyOrder(entry.orderId, entry.newQty);
            break;
        case JournalEntry::Type::CancelReplace:
            journal_->logCancelReplace(entry.orderId, entry.newPrice,
                                       entry.newQty);
            break;
        case JournalEntry::Type::Snapshot:
            journal_->logSnapshot(entry.orderId, entry.participantId,
                                  entry.symbolId, entry.side, entry.price,
                                  entry.quantity, entry.orderType,
                                  entry.timeInForce, entry.expiryTime,
                                  entry.stopPrice, entry.stopLimitPrice,
                                  entry.displayQty, entry.pegType,
                                  entry.pegOffset, entry.trailAmount,
                                  entry.minQty, entry.hidden);
            break;
        }
    }

    return applied;
}

void MatchingEngine::checkpointInternal(bool alreadyDrained) {
    if (!journal_) {
        return;
    }

    if (async_ && !alreadyDrained) {
        waitForDrain();
    }

    // Two phases, deliberately NOT nested, to keep the lock order acyclic.
    // The expiry path takes book->bookLock_ then journalMutex_ (onExpire logs
    // under journalMutex_ while the book lock is held). Holding journalMutex_
    // here and then reaching for a book lock (which forEachOrderLocked does)
    // would invert that order and could deadlock a concurrent worker
    // ExpireCheck. So: phase 1 copies a point-in-time snapshot of every resting
    // order under each book's own lock (under bookMutex_ for a consistent
    // symbol set, never holding journalMutex_); phase 2 writes the gathered
    // copy under journalMutex_ alone (no book lock held). Order copies are
    // read-only value snapshots — the intrusive next/prev are never deref'd.
    std::vector<std::pair<SymbolId, Order>> resting;
    {
        std::lock_guard<std::mutex> booksLock(bookMutex_);
        for (SymbolId symbolId : symbolIds_) {
            const OrderBook* book = getOrderBook(symbolId);
            if (!book) {
                continue;
            }
            book->forEachOrderLocked([&](const Order& order) {
                resting.emplace_back(symbolId, order);
            });
        }
    }

    std::lock_guard<std::mutex> lock(journalMutex_);
    if (!journal_) {
        return;
    }
    journal_->rewriteAtomically([&](Journal& snapshotJournal) {
        for (const auto& [symbolId, order] : resting) {
            snapshotJournal.logSnapshot(order.id, order.participantId, symbolId,
                                        order.side, order.price, order.remainingQty,
                                        order.type, order.timeInForce, order.expiryTime,
                                        order.stopPrice, order.stopLimitPrice,
                                        order.displayQty, order.pegType,
                                        order.pegOffset, order.trailAmount,
                                        order.minQty, order.isHidden);
        }
    });
}

void MatchingEngine::checkpoint() {
    checkpointInternal(async_ ? false : true);
}

void MatchingEngine::processFIXMessage(const std::string& rawFix) {
    FixMessage msg;
    if (!msg.parse(rawFix.c_str(), rawFix.size())) {
        return;
    }

    auto params = fixToOrderParams(msg);
    if (!params.valid) {
        return;
    }

    switch (params.action) {
    case FixOrderParams::Action::NewOrder:
        processOrder(params.symbolId, params.orderId, params.participantId, params.side,
                     params.price, params.qty, params.orderType, 0, 0, params.tif);
        break;
    case FixOrderParams::Action::Cancel:
        cancelOrder(params.symbolId, params.orderId);
        break;
    case FixOrderParams::Action::CancelReplace:
        cancelReplace(params.symbolId, params.orderId, params.newPrice, params.newQty);
        break;
    default:
        break;
    }
}

void MatchingEngine::processOrder(OrderId orderId, ParticipantId participantId, Side side,
                                  Price price, Quantity qty, OrderType type,
                                  Price stopPrice, Quantity displayQty) {
    processOrder(static_cast<SymbolId>(0), orderId, participantId, side, price, qty, type,
                 stopPrice, displayQty);
}

void MatchingEngine::cancelOrder(OrderId orderId) {
    cancelOrder(static_cast<SymbolId>(0), orderId);
}

double MatchingEngine::getVWAP() const {
    auto* book = getOrderBook(0);
    return book ? book->getVWAP() : 0.0;
}

LatencyTracker MatchingEngine::getAggregateE2ELatency() const {
    LatencyTracker aggregate;
    if (!e2eLatency_) {
        return aggregate;
    }

    for (size_t i = 0; i < numThreads_; ++i) {
        aggregate.mergeFrom(e2eLatency_[i]);
    }
    return aggregate;
}

} // namespace OrderMatcher
