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
        std::lock_guard<std::mutex> lock(bookMutex_);
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

        AddOrderResult result = book->addOrder(req.orderId, req.participantId, req.side,
                                               req.price, req.qty, req.orderType,
                                               req.stopPrice, req.displayQty, req.tif,
                                               req.expiryTime, req.stopLimitPrice,
                                               req.pegType, req.pegOffset, req.trailAmount,
                                               req.minQty, req.hidden);
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
        break;
    }
    case OrderRequest::Type::Cancel: {
        auto* book = getOrderBook(req.symbolId);
        if (!book || !book->getOrder(req.orderId)) {
            return;
        }
        book->cancelOrder(req.orderId);
        if (journal_) {
            {
                std::lock_guard<std::mutex> lock(journalMutex_);
                journal_->logCancelOrder(req.orderId);
            }
            maybeTriggerAutoCheckpoint();
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
            if (auto* book = getOrderBook(symbolId)) {
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
    if (booksFrozen_.load(std::memory_order_acquire) || books_.contains(symbolId)) {
        return;
    }

    books_.insert(symbolId, std::make_unique<OrderBook>(symbolId, algo));
    symbolIds_.push_back(symbolId);
    rebuildThreadSymbolIndex();
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
    if (!running_.load(std::memory_order_acquire)) {
        return rejectedAsync(RejectReason::EngineStopped);
    }

    uint64_t sequenceId = nextSubmitSequence_.fetch_add(1, std::memory_order_relaxed);

    if (rateLimiter_.isEnabled() && !rateLimiter_.allow(participantId)) {
        rateLimitedCount_.fetch_add(1, std::memory_order_relaxed);
        return rejectedAsync(RejectReason::RateLimitExceeded);
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

    uint64_t total = 0;
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

void MatchingEngine::expireOrders(uint64_t currentTime) {
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

void MatchingEngine::checkpointInternal(bool alreadyDrained) {
    if (!journal_) {
        return;
    }

    if (async_ && !alreadyDrained) {
        waitForDrain();
    }

    std::lock_guard<std::mutex> lock(journalMutex_);
    journal_->rewriteAtomically([&](Journal& snapshotJournal) {
        for (SymbolId symbolId : symbolIds_) {
            const OrderBook* book = getOrderBook(symbolId);
            if (!book) {
                continue;
            }

            book->forEachOrder([&](const Order& order) {
                snapshotJournal.logSnapshot(order.id, order.participantId, symbolId,
                                            order.side, order.price, order.remainingQty,
                                            order.type, order.timeInForce, order.expiryTime,
                                            order.stopPrice, order.stopLimitPrice,
                                            order.displayQty, order.pegType,
                                            order.pegOffset, order.trailAmount,
                                            order.minQty, order.isHidden);
            });
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
