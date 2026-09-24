#include "MatchingEngine.h"
#include "LatencyTracker.h"
#include "Utils.h"  // OrderMatcher::Utils::rdtsc / calibrateTsc / tscTicksToNs
#include "Metrics.h"
#include "StructuredLog.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <system_error>

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

// Sequence ids for SubmitResult. They must be UNIQUE and must INCREASE for a
// given caller; nothing consumes a dense global order — the gateway echoes the
// id straight back to the client (GatewayProtocol.h:234) and AdminServer prints
// it. This used to be one engine-wide fetch_add on the submit path, contended
// by every producer on every order.
//
// So hand them out in per-thread blocks: one contended RMW per kSeqBlock
// orders instead of one per order. Still unique, still monotonic per thread,
// still never 0 (0 means "rejected"). The counter is process-wide rather than
// per-engine, which only means a second engine in the same process starts
// partway up the number line — no consumer cares where it starts.
constexpr uint64_t kSeqBlock = 1024;
std::atomic<uint64_t> g_seqBlockBase{0};
thread_local uint64_t t_seqNext = 0;
thread_local uint64_t t_seqEnd  = 0;

inline uint64_t nextSequenceId() {
    if (t_seqNext == t_seqEnd) [[unlikely]] {
        const uint64_t base = g_seqBlockBase.fetch_add(kSeqBlock, std::memory_order_relaxed);
        t_seqNext = base + 1;
        t_seqEnd  = base + kSeqBlock + 1;
    }
    return t_seqNext++;
}

// waitForDrain's escalation ladder: spin, then yield, then sleep. See
// waitForDrain for why it polls at all.
constexpr uint64_t kDrainSpins  = 2048;
constexpr uint64_t kDrainYields = 2048;
constexpr auto     kDrainPollSleep = std::chrono::microseconds(50);

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

// C5: never-drop control-plane enqueue. See the header for why this shares the
// order ring rather than using a side channel.
// Bounded sibling of enqueueControl, for the one caller that must not spin
// forever. Does the submitted_/wakeup bookkeeping ONLY on success: counting a
// message that was never queued would make waitForDrain() wait for something
// that is never going to be processed.
bool MatchingEngine::tryEnqueueControl(size_t threadIndex, const OrderRequest& req,
                                       uint64_t maxSpins) {
    for (uint64_t spins = 0; spins < maxSpins; ++spins) {
        if (requestQueues_[threadIndex]->push(req)) {
            threadStats_[threadIndex].submitted.fetch_add(1, std::memory_order_relaxed);
            queueWakeups_[threadIndex].fetch_add(1, std::memory_order_release);
            queueWakeups_[threadIndex].notify_one();
            return true;
        }
        cpuRelax();
    }
    return false;
}

void MatchingEngine::enqueueControl(size_t threadIndex, const OrderRequest& req) {
    uint64_t spins = 0;
    while (!requestQueues_[threadIndex]->push(req)) {
        ++spins;
        cpuRelax();
    }
    if (spins > 0) {
        controlSpins_.fetch_add(1, std::memory_order_relaxed);
        // One line per contended control enqueue: rare by construction, and the
        // operator wants to know the ring was full when a safety control fired.
        if (obSinkActive()) {
            obSink().log(obEvent("control_enqueue_contended", LogSeverity::Warn)
                .kv("thread", (long long)threadIndex)
                .kv("request_type", (long long)static_cast<int>(req.type))
                .kv("spins", (long long)spins));
        }
    }
    // Same bookkeeping enqueueSafe does on success. The submitted counter in
    // particular is what makes the subsequent waitForDrain() actually wait for
    // this message: a dropped enqueue never bumped it, so waitForDrain returned
    // immediately and the caller believed a sweep had run that never did.
    threadStats_[threadIndex].submitted.fetch_add(1, std::memory_order_relaxed);
    queueWakeups_[threadIndex].fetch_add(1, std::memory_order_release);
    queueWakeups_[threadIndex].notify_one();
}

size_t MatchingEngine::countResting(ParticipantId pid) {
    // Same lock discipline as cancelAllRestingOrders(): bookMutex_ guards
    // symbolIds_/books_ against a concurrent addSymbol, and forEachOrderLocked
    // takes each book's own bookLock_ — exclusively, since it is a plain
    // std::mutex — so the walk is safe while a worker mutates a different
    // book, and blocks a worker trying to mutate this one.
    std::lock_guard<std::mutex> lock(bookMutex_);
    size_t n = 0;
    for (SymbolId sym : symbolIds_) {
        auto* book = getOrderBook(sym);
        if (!book) continue;
        book->forEachOrderLocked([&](const Order& o) {
            if (pid == kKillAllParticipants || o.participantId == pid) ++n;
        });
    }
    return n;
}

size_t MatchingEngine::sweepAndVerify(ParticipantId pid) {
    // Bounded reconciliation. With the apply-time kill re-check in
    // processRequest, one pass is provably sufficient for the engine-wide
    // switch; the retry exists so a participant kill racing an in-flight submit
    // still converges, and so the guarantee is VERIFIED rather than asserted.
    constexpr int kMaxSweeps = 3;
    size_t survivors = 0;
    for (int attempt = 0; attempt < kMaxSweeps; ++attempt) {
        OrderRequest req{};
        req.type = OrderRequest::Type::KillSwitch;
        req.participantId = pid;
        for (size_t i = 0; i < numThreads_; ++i) enqueueControl(i, req);
        waitForDrain();

        survivors = countResting(pid);
        if (survivors == 0) return 0;
    }
    // Never observed in test; logged rather than swallowed so an operator sees
    // a kill switch that did not fully converge instead of assuming it did.
    if (obSinkActive()) {
        obSink().log(obEvent("kill_switch_incomplete", LogSeverity::Error)
            .kv("participant", (long long)pid)
            .kv("survivors", (long long)survivors)
            .kv("sweeps", (long long)kMaxSweeps));
    }
    return survivors;
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

// Both counts are summed from threadStats_ at read time instead of being
// maintained as engine-wide atomics: see the note by threadStats_ in the
// header. Cold callers only (admin stats, drain, checkpoint trigger), so an
// O(numThreads) sum is free and the hot path loses a contended RMW per order.
// Sync mode never allocates threadStats_, and reported 0 before this change
// too, because nothing on the sync path ever incremented the old totals.
uint64_t MatchingEngine::getSubmittedCount() const {
    const ThreadStats* stats = threadStats_.get();
    if (!stats) return 0;
    uint64_t total = 0;
    for (size_t i = 0; i < numThreads_; ++i) {
        total += stats[i].submitted.load(std::memory_order_acquire);
    }
    return total;
}

uint64_t MatchingEngine::getProcessedCount() const {
    const ThreadStats* stats = threadStats_.get();
    if (!stats) return 0;
    uint64_t total = 0;
    for (size_t i = 0; i < numThreads_; ++i) {
        total += stats[i].processed.load(std::memory_order_acquire);
    }
    return total;
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
    shuttingDown_.store(false, std::memory_order_release);
    workersShouldStop_.store(false, std::memory_order_release);
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

    // Calibrate the TSC once here, while still single-threaded, so the ~5 ms
    // sample never lands on a worker's hot path. Used by the per-order e2e
    // latency measurement (Utils::rdtsc / tscTicksToNs).
    Utils::calibrateTsc();

    numThreads_ = numThreads > 0 ? numThreads : 1;
    rebuildThreadSymbolIndex();

    // Fresh ThreadStats zero the submitted/processed counters, which is what
    // resetting the old engine-wide totals used to do here.
    threadStats_ = std::make_unique<ThreadStats[]>(numThreads_);
    e2eLatency_ = std::make_unique<LatencyTracker[]>(numThreads_);
    queueWakeups_ = std::make_unique<std::atomic<uint64_t>[]>(numThreads_);

    requestQueues_.clear();
    workerThreads_.clear();
    requestQueues_.reserve(numThreads_);
    workerThreads_.reserve(numThreads_);

    shuttingDown_.store(false, std::memory_order_release);
    workersShouldStop_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    booksFrozen_.store(true, std::memory_order_release);
    async_ = true;

    for (size_t i = 0; i < numThreads_; ++i) {
        queueWakeups_[i].store(0, std::memory_order_relaxed);
        requestQueues_.push_back(std::make_unique<MpscQueue<OrderRequest>>(queueSize));
        workerThreads_.emplace_back(&MatchingEngine::workerLoop, this, i);
    }

    // Sync start() has always logged engine_start; the async path — the one
    // production actually runs — logged nothing at all, so an operator who
    // enabled logging saw silence at the single most important moment.
    obSink().log(obEvent("engine_start")
                     .kv("mode", "async")
                     .kv("symbols", (unsigned long long)symbolIds_.size())
                     .kv("threads", (unsigned long long)numThreads_)
                     .kv("queue_size", (unsigned long long)queueSize));

    // capacityMonitor_ was a constructed member with no callbacks and no
    // thread: it could not observe anything and never ran. Both happen here,
    // in this order — the callbacks read requestQueues_, which is rebuilt
    // just above and torn down in stopAsync(), so the monitor thread must
    // exist only between those two points.
    installCapacityCallbacks();
    capacityMonitor_.start();
}

// Resource queries only — each one reads a counter the engine already keeps,
// and none of them decides anything. What to DO about a breach (a webhook, a
// pager, an incident file) needs an endpoint and a path that only the
// deployment knows; the breach itself reaching obSink() does not.
void MatchingEngine::installCapacityCallbacks() {
    // Worst queue, not the average: one saturated worker is a stalled symbol
    // even when the other rings are empty, and averaging hides exactly that.
    // approxSize()/capacity() are atomic loads on the same queues the
    // backpressure check in enqueueSafe() already reads.
    capacityMonitor_.setQueueDepthCallback([this]() -> double {
        double worst = 0.0;
        for (const auto& queue : requestQueues_) {
            const size_t cap = queue->capacity();
            if (cap == 0) continue;
            worst = std::max(worst, static_cast<double>(queue->approxSize()) /
                                        static_cast<double>(cap));
        }
        return worst;
    });

    // Journal disk. The path is resolved once, here, and captured by value:
    // journal_ lives under journalMutex_ and is also held across checkpoint
    // rewrites, so taking that lock once a second from a monitoring thread
    // would put a background poller in the way of the commit path.
    //
    // Consequence of resolving it once: enableJournal() after startAsync()
    // gets no disk monitoring. main.cpp enables the journal first, which is
    // also the only order that lets the journal be replayed into the books.
    std::string journalDir;
    {
        std::lock_guard<std::mutex> lock(journalMutex_);
        if (journal_) {
            journalDir = std::filesystem::path(journal_->path()).parent_path().string();
            if (journalDir.empty()) journalDir = ".";
        }
    }
    if (!journalDir.empty()) {
        // One report, not one per second: a filesystem that cannot be
        // statted stays that way, and 1 Hz of identical warnings is how an
        // operator learns to filter out the channel this monitor reports on.
        auto reportedFailure = std::make_shared<std::atomic<bool>>(false);
        capacityMonitor_.setDiskUsageCallback([journalDir, reportedFailure]() -> double {
            std::error_code ec;
            const std::filesystem::space_info space =
                std::filesystem::space(journalDir, ec);
            if (ec || space.capacity == 0) {
                // Returning 0.0 reads as "disk empty" — never let that pass
                // silently, because it is indistinguishable from healthy.
                if (!reportedFailure->exchange(true)) {
                    obSink().log(obEvent("capacity_probe_failed", LogSeverity::Warn)
                                     .kv("resource", "journal_disk")
                                     .kv("path",     journalDir)
                                     .kv("error",    ec ? ec.message() : "zero capacity"));
                }
                return 0.0;
            }
            // `available` rather than `free`: the reserved-for-root blocks are
            // not space this process can journal into.
            return static_cast<double>(space.capacity - space.available) /
                   static_cast<double>(space.capacity);
        });
    }

    // Deliberately NOT installed, both blocked on a decision this code cannot
    // make:
    //   * memory — CapacityThresholds.memoryUsagePct is a fraction of a
    //     limit, and no limit exists anywhere in the config or the code. RSS
    //     over total host RAM is not it (the engine shares the host); the
    //     real ceiling is the container/cgroup limit the deployment sets.
    //   * replication lag — the ReplicationCoordinator is owned by main.cpp
    //     and constructed after startAsync(), so there is nothing to read
    //     here, and installing the callback later would race the running
    //     monitor thread. Moving coordinator setup ahead of startAsync() is
    //     the fix, and it is a change to startup ordering, not to this.
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
        // C5: control plane — an expiry sweep that is silently dropped leaves
        // expired GTD/DAY orders live and tradeable.
        for (size_t i = 0; i < numThreads_; ++i) {
            enqueueControl(i, req);
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

    // First, before anything is torn down. The monitor's callbacks hold
    // `this` and read requestQueues_, which this function clears — so a
    // monitor thread that outlived the queues would be reading freed memory
    // during shutdown, the one moment nobody is watching. stop() joins, and
    // the join is bounded by CapacityMonitor::kSleepSliceMs rather than by
    // the check interval, so it does not add to the shutdown watchdog's
    // reporting window.
    capacityMonitor_.stop();

    stopExpiryTimer();

    OrderRequest shutdown{};
    shutdown.type = OrderRequest::Type::Shutdown;

    // Shutdown goes in-band so it queues behind accepted work and nothing is
    // dropped. But enqueueControl spins until the push succeeds, and a worker
    // whose ring is full at this moment drains it only if it is still running
    // — so a wedged or slow worker turned "stop the engine" into an infinite
    // spin on the caller's thread, with no timeout and no diagnostic. That is
    // the shutdown hang the audit flagged.
    //
    // Bounded attempt, then a flag the worker checks once its queue is empty.
    // Both paths still drain every accepted order first; the flag only removes
    // the requirement that there be ring space for the message itself.
    for (size_t i = 0; i < numThreads_; ++i) {
        if (!tryEnqueueControl(i, shutdown)) {
            obSink().log(obEvent("shutdown_enqueue_full", LogSeverity::Warn)
                .kv("thread", (long long)i));
        }
    }
    workersShouldStop_.store(true, std::memory_order_release);
    for (size_t i = 0; i < numThreads_; ++i) {
        queueWakeups_[i].fetch_add(1, std::memory_order_release);
        queueWakeups_[i].notify_one();
    }

    // Report while we wait, then join — which is now known to return promptly,
    // because every worker has already left its loop.
    awaitWorkerExit();

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

    // Poll, rather than have every worker notify a condition variable on every
    // message it completes. The waiters are all cold — shutdown, checkpoint, a
    // research harness between orders — and the old form paid a notify_all()
    // per processed order to serve a waiter that is usually not there.
    //
    // ponytail: spin covers the common case (a few in-flight orders, tens of
    // microseconds); the sleep keeps a genuinely long drain, e.g. a final
    // checkpoint fsync, off the CPU. Ceiling: a waiter can be up to
    // kDrainPollSleep late. If a caller ever needs tighter drain latency, add a
    // waiter count the workers test before notifying — do NOT put the
    // unconditional notify back.
    const uint64_t target = getSubmittedCount();
    for (uint64_t spins = 0; getProcessedCount() < target; ++spins) {
        if (spins < kDrainSpins) {
            cpuRelax();
        } else if (spins < kDrainSpins + kDrainYields) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(kDrainPollSleep);
        }
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
                break;
            }

            processRequest(threadIndex, req);
            if (req.ingressTsc != 0) {
                // Stamp end with the same cheap counter, then convert the tick
                // delta to ns ONCE here (metrics write, off the matching path).
                const uint64_t endTsc = Utils::latencyClockTicks();
                if (endTsc > req.ingressTsc) {
                    e2eLatency_[threadIndex].record(
                        Utils::latencyTicksToNs(endTsc - req.ingressTsc));
                }
            }

            threadStats_[threadIndex].processed.fetch_add(1, std::memory_order_release);

            // The two sums walk every thread's stats, so the pending flag is
            // tested FIRST and short-circuits them away on every ordinary
            // order. This is the only hot-path reader of the aggregates.
            if (threadIndex == 0 &&
                checkpointPending_.load(std::memory_order_acquire) &&
                getProcessedCount() >= getSubmittedCount()) {
                checkpointPending_.store(false, std::memory_order_release);
                checkpointInternal(true);
            }
            continue;
        }

        // Backstop exit. A FAILED pop is not the same as an empty queue: this
        // is an MPSC ring, so pop() also returns false while a producer has
        // claimed a slot and not yet published it. Leaving on that alone drops
        // work that was already counted as submitted, and waitForDrain() then
        // waits forever for a message nobody will ever process — which is
        // exactly the hang this backstop was added to prevent, reintroduced
        // one layer down.
        //
        // So confirm against this thread's own counters: only leave once it
        // has processed everything ever submitted to it. An in-flight producer
        // leaves processed < submitted, and we keep spinning until it lands.
        if (workersShouldStop_.load(std::memory_order_acquire) &&
            threadStats_[threadIndex].processed.load(std::memory_order_acquire) >=
            threadStats_[threadIndex].submitted.load(std::memory_order_acquire)) {
            break;
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

    // Last act, covering both breaks above. Until this existed, stopAsync()
    // could not tell a worker that was finishing from one that would never
    // finish — join() looks identical either way.
    threadStats_[threadIndex].exited.store(true, std::memory_order_release);
}

// Wait for every worker to leave workerLoop(), saying what is still running.
//
// join() has no timeout, so a genuinely wedged worker turned shutdown into a
// silent hang: the process sat there until the orchestrator's grace period ran
// out and SIGKILLed it, with nothing in the logs to say why. The fix is not to
// stop waiting — a wedged worker still owns queues and book state this
// function is about to destroy, so detaching it would trade a visible hang for
// a use-after-free — but to make the wait explain itself.
//
// Slow is not the same as wedged, and this deliberately does not guess which
// it is looking at: a final checkpoint fsync can legitimately take seconds.
// It reports the counters and lets the operator judge.
void MatchingEngine::awaitWorkerExit() {
    const auto start = std::chrono::steady_clock::now();
    size_t reports = 0;

    for (;;) {
        size_t running = 0;
        for (size_t i = 0; i < numThreads_; ++i) {
            if (!threadStats_[i].exited.load(std::memory_order_acquire)) ++running;
        }
        if (running == 0) break;

        const auto waited = std::chrono::steady_clock::now() - start;
        const auto due = shutdownReportInterval_ * (reports + 1);
        if (waited >= due) {
            ++reports;
            const auto waitedMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(waited).count();
            for (size_t i = 0; i < numThreads_; ++i) {
                if (threadStats_[i].exited.load(std::memory_order_acquire)) continue;
                const uint64_t sub = threadStats_[i].submitted.load(std::memory_order_acquire);
                const uint64_t don = threadStats_[i].processed.load(std::memory_order_acquire);
                obSink().log(obEvent("shutdown_worker_still_running", LogSeverity::Warn)
                                 .kv("thread",    (long long)i)
                                 .kv("waited_ms", (long long)waitedMs)
                                 .kv("submitted", (unsigned long long)sub)
                                 .kv("processed", (unsigned long long)don)
                                 .kv("outstanding", (unsigned long long)(sub - don)));
            }
        }

        // Polling, not atomic::wait(): this needs a deadline to report on, and
        // wait() has none. The interval is shutdown-only, so the cost is noise.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
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

        // C5: re-check the kill switch AT APPLY TIME, not just at submit.
        // submitOrder tests the flag and then enqueues; a producer can pass
        // that test, be preempted, have setKillSwitch run its entire sweep, and
        // only then enqueue — so the order rests AFTER the sweep and survives
        // it. The worker is what mutates the book, so the worker is where the
        // check is authoritative. This is what makes "no resting order survives
        // an engaged kill switch" true rather than merely likely.
        if (killSwitchActive_.load(std::memory_order_acquire)) [[unlikely]] {
            killSwitchRejects_.fetch_add(1, std::memory_order_relaxed);
            book->emitReject(req.orderId, req.qty, RejectReason::KillSwitchActive);
            break;
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
        // H1: read the exposure inside cancelOrderReleasing's own critical
        // section. The previous form dereferenced an UNLOCKED getOrder()
        // pointer into orderPool_, with no guarantee the order was still
        // alive — a concurrent fill or a shutdown sweep frees that slot.
        // req.participantId is the requester here, not the order's owner: for
        // a Cancel it is whoever asked, and kAnyParticipant when the caller is
        // internal. A denied cancel must not journal or release exposure, so
        // bail before either.
        const auto exposure =
            book->cancelOrderReleasing(req.orderId, req.participantId);
        if (exposure.denied) [[unlikely]] {
            obSink().log(obEvent("cancel_denied_not_owner", LogSeverity::Warn)
                             .kv("order_id",  (unsigned long long)req.orderId)
                             .kv("symbol_id", (long long)req.symbolId)
                             .kv("requester", (unsigned long long)req.participantId));
            return;
        }
        if (positionLimitsActive_.load(std::memory_order_relaxed)) {
            releasePosition(exposure);
        }
        if (journal_) {
            {
                std::lock_guard<std::mutex> lock(journalMutex_);
                journal_->logCancelOrder(req.orderId, req.symbolId);
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
        if (book->modifyOrder(req.orderId, req.newQty, req.participantId) && journal_) {
            {
                std::lock_guard<std::mutex> lock(journalMutex_);
                journal_->logModifyOrder(req.orderId, req.symbolId, req.newQty);
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
        if (book->cancelReplace(req.orderId, req.newPrice, req.newQty,
                                req.participantId) && journal_) {
            {
                std::lock_guard<std::mutex> lock(journalMutex_);
                journal_->logCancelReplace(req.orderId, req.symbolId, req.newPrice, req.newQty);
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
                    // H1: read the exposure inside cancelOrderReleasing's own critical
                    // section. The previous form dereferenced an UNLOCKED getOrder()
                    // pointer into orderPool_, with no guarantee the order was still
                    // alive — a concurrent fill or a shutdown sweep frees that slot.
                    if (positionLimitsActive_.load(std::memory_order_relaxed)) {
                        releasePosition(book->cancelOrderReleasing(id));
                    } else {
                        book->cancelOrder(id);
                    }
                    if (journal_) {
                        std::lock_guard<std::mutex> lock(journalMutex_);
                        journal_->logCancelOrder(id, book->getSymbolId());
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
        // Built inside the loop: the callback receives only an OrderId, so
        // the symbol has to be captured per book rather than hoisted.
        for (SymbolId symbolId : symbolsByThread_[threadIndex]) {
            std::function<void(OrderId)> onExpire;
            if (journal_) {
                onExpire = [this, symbolId](OrderId id) {
                    std::lock_guard<std::mutex> lock(journalMutex_);
                    journal_->logCancelOrder(id, symbolId);
                };
            }
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
    if (ocoActive && ocoActive->load(std::memory_order_relaxed) && isExecution(u.status)) {
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
                    journal_->logCancelOrder(sib, book->getSymbolId());  // replay reproduces the OCO cancel
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
        // cancel-all request to every worker, drain, then VERIFY by recount that
        // nothing survived. Previously this used enqueueSafe and ignored its
        // return, so a dropped message left every resting order live while new
        // orders were rejected by the flag above.
        sweepAndVerify(kKillAllParticipants);
    } else {
        cancelAllRestingOrders();
    }
}

void MatchingEngine::cancelAllRestingOrders() {
    // Sync-mode kill-switch sweep. Lock order bookMutex_ -> bookLock_ ->
    // journalMutex_, matching expireOrders()/driveOco(). Ids are collected under
    // the book's own lock (forEachOrderLocked), then cancelled outside it so
    // cancelOrder can re-take bookLock_ without re-entrancy — it is a plain,
    // non-recursive std::mutex, so collecting and cancelling in one pass would
    // self-deadlock.
    std::lock_guard<std::mutex> lock(bookMutex_);
    for (SymbolId sym : symbolIds_) {
        auto* book = getOrderBook(sym);
        if (!book) continue;
        std::vector<OrderId> ids;
        book->forEachOrderLocked([&](const Order& o) { ids.push_back(o.id); });
        for (OrderId id : ids) {
            // H1: read the exposure inside cancelOrderReleasing's own critical
            // section. The previous form dereferenced an UNLOCKED getOrder()
            // pointer into orderPool_, with no guarantee the order was still
            // alive — a concurrent fill or a shutdown sweep frees that slot.
            if (positionLimitsActive_.load(std::memory_order_relaxed)) {
                releasePosition(book->cancelOrderReleasing(id));
            } else {
                book->cancelOrder(id);
            }
            if (journal_) {
                std::lock_guard<std::mutex> jl(journalMutex_);
                journal_->logCancelOrder(id, book->getSymbolId());
            }
        }
    }
}

size_t MatchingEngine::cancelDayOrders() {
    // Session-end DAY sweep. Same lock discipline as cancelAllRestingOrders()
    // (bookMutex_ -> book->bookLock_ -> journalMutex_): collect ids under each
    // book's lock, then cancel outside it so cancelOrder can take bookLock_
    // exclusively without re-entrancy on the non-recursive mutex.
    std::lock_guard<std::mutex> lock(bookMutex_);
    size_t cancelled = 0;
    for (SymbolId sym : symbolIds_) {
        auto* book = getOrderBook(sym);
        if (!book) continue;
        std::vector<OrderId> dayIds;
        book->forEachOrderLocked([&](const Order& o) {
            if (o.timeInForce == TimeInForce::DAY) dayIds.push_back(o.id);
        });
        for (OrderId id : dayIds) {
            // H1: read the exposure inside cancelOrderReleasing's own critical
            // section. The previous form dereferenced an UNLOCKED getOrder()
            // pointer into orderPool_, with no guarantee the order was still
            // alive — a concurrent fill or a shutdown sweep frees that slot.
            if (positionLimitsActive_.load(std::memory_order_relaxed)) {
                releasePosition(book->cancelOrderReleasing(id));
            } else {
                book->cancelOrder(id);
            }
            if (journal_) {
                std::lock_guard<std::mutex> jl(journalMutex_);
                journal_->logCancelOrder(id, book->getSymbolId());
            }
            ++cancelled;
            // "log at session end" — durable, per-order audit line.
            obSink().log(obEvent("day_order_cancelled_at_shutdown")
                             .kv("symbol", (long long)sym)
                             .kv("order", (unsigned long long)id));
        }
    }
    return cancelled;
}

MatchingEngine::ShutdownReport MatchingEngine::gracefulShutdown() {
    ShutdownReport report{};

    // 1. Stop admitting new orders. Already-enqueued requests still drain.
    shuttingDown_.store(true, std::memory_order_release);

    // 2. Drain in-flight work so IOC remainders are cancelled during matching
    //    and any partial fill completes before we snapshot (see header).
    if (async_) {
        waitForDrain();
    }

    // 3. Session end: cancel every DAY order (journaled + logged). GTD/GTC stay.
    report.dayOrdersCancelled = cancelDayOrders();

    // 4. Tally the survivors by time-in-force so the caller can see exactly what
    //    will be restored, then persist them via checkpoint. Restart's
    //    replayJournal() rebuilds the GTD/GTC book from this snapshot.
    {
        std::lock_guard<std::mutex> booksLock(bookMutex_);
        for (SymbolId sym : symbolIds_) {
            const OrderBook* book = getOrderBook(sym);
            if (!book) continue;
            book->forEachOrderLocked([&](const Order& o) {
                if (o.timeInForce == TimeInForce::GTD) ++report.gtdOrdersPersisted;
                else ++report.otherOrdersPersisted;
            });
        }
    }
    {
        std::lock_guard<std::mutex> jl(journalMutex_);
        report.journalEnabled = (journal_ != nullptr);
    }
    if (report.journalEnabled) {
        // Already drained (step 2) and cancelDayOrders() is synchronous, so no
        // fresh queue entries exist — snapshot the current state directly.
        checkpointInternal(true);
    }
    report.ordersPersisted = report.gtdOrdersPersisted + report.otherOrdersPersisted;

    obSink().log(obEvent("engine_graceful_shutdown")
                     .kv("day_cancelled", (unsigned long long)report.dayOrdersCancelled)
                     .kv("gtd_persisted", (unsigned long long)report.gtdOrdersPersisted)
                     .kv("other_persisted", (unsigned long long)report.otherOrdersPersisted)
                     .kv("journal", (long long)(report.journalEnabled ? 1 : 0)));
    return report;
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

void MatchingEngine::releasePosition(const OrderBook::OrderExposure& e) {
    if (!e.found || e.participantId >= MAX_PARTICIPANTS || e.remainingQty == 0) return;
    int64_t signedRest = (e.side == Side::Buy) ? static_cast<int64_t>(e.remainingQty)
                                               : -static_cast<int64_t>(e.remainingQty);
    positions_[e.participantId].fetch_sub(signedRest, std::memory_order_relaxed);
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
            if (orderNotional(price, qty) > static_cast<__int128>(maxNotional)) {
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

    // P3-6: once a graceful shutdown has begun, refuse NEW orders. Requests
    // already enqueued keep draining; only fresh submissions are turned away.
    if (shuttingDown_.load(std::memory_order_acquire)) [[unlikely]] {
        return rejectedAsync(RejectReason::EngineStopped);
    }

    uint64_t sequenceId = nextSequenceId();

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
        req.ingressTsc = Utils::latencyClockTicks();  // x86: TSC; else steady_clock ns
        if (!enqueueSafe(getThreadIndex(symbolId), req)) {
            return rejectedAsync(RejectReason::QueueBackpressure);
        }
        return acceptedAsync(sequenceId);
    }

    auto* book = getOrderBook(symbolId);
    if (!book) {
        return rejectedAsync(RejectReason::SymbolNotFound);
    }

    // Capture everything this order emits so none of it reaches the client
    // before the journal entry behind it is durable. No-op when disabled.
    durabilityGate_.beginOrder();
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
        uint64_t appendOrdinal = 0;
        {
            std::lock_guard<std::mutex> lock(journalMutex_);
            journal_->logAddOrder(orderId, participantId, symbolId, side, price, qty, type, tif,
                                  expiryTime, stopPrice, stopLimitPrice, displayQty, pegType,
                                  pegOffset, trailAmount, minQty, hidden);
            appendOrdinal = journal_->entriesAppended();
        }
        // Hold this order's events until that entry is durable. Note the
        // commit may already have happened inside logAddOrder (Immediate
        // policy, or a full batch), in which case releaseThrough has already
        // run for this ordinal and commitOrder releases immediately.
        durabilityGate_.commitOrder(appendOrdinal);
        if (durabilityGate_.enabled()) {
            const uint64_t durable = durableEntries_.load(std::memory_order_acquire);
            if (durable >= appendOrdinal) {
                durabilityGate_.releaseThrough(durable);
            }
        }
        maybeTriggerAutoCheckpoint();
    } else {
        // No journal entry — a reject, or journalling is off. There is nothing
        // for these events to wait on, and leaving the gate armed would spill
        // them into the next order's group and hold them indefinitely.
        durabilityGate_.abandonOrder();
    }
    return orderBookResultToSubmitResult(result, sequenceId);
}

void MatchingEngine::cancelOrder(SymbolId symbolId, OrderId orderId,
                                 ParticipantId requester) {
    (void)submitCancel(symbolId, orderId, requester);
}

SubmitResult MatchingEngine::submitCancel(SymbolId symbolId, OrderId orderId,
                                          ParticipantId requester) {
    if (!running_.load(std::memory_order_acquire)) {
        return rejectedAsync(RejectReason::EngineStopped);
    }

    // H1: once graceful shutdown has begun the sweeps are returning orders to
    // the pool. A cancel admitted after that raced the teardown and
    // dereferenced freed pool memory. Same gate submitOrder already uses.
    if (shuttingDown_.load(std::memory_order_acquire)) [[unlikely]] {
        return rejectedAsync(RejectReason::EngineStopped);
    }

    uint64_t sequenceId = nextSequenceId();

    if (async_) {
        if (!getOrderBook(symbolId)) {
            return rejectedAsync(RejectReason::SymbolNotFound);
        }

        OrderRequest req{};
        req.type = OrderRequest::Type::Cancel;
        req.symbolId = symbolId;
        req.orderId = orderId;
        // The worker does the ownership check; it has to travel with the
        // request, because by the time the worker runs the caller is gone.
        req.participantId = requester;
        req.ingressTsc = Utils::latencyClockTicks();  // x86: TSC; else steady_clock ns
        if (!enqueueSafe(getThreadIndex(symbolId), req)) {
            return rejectedAsync(RejectReason::QueueBackpressure);
        }
        return acceptedAsync(sequenceId);
    }

    auto* book = getOrderBook(symbolId);
    if (!book) {
        return rejectedAsync(RejectReason::SymbolNotFound);
    }
    // THE EXISTENCE CHECK IS cancelOrderReleasing's, NOT A PRE-CHECK HERE.
    //
    // This used to call book->getOrder(orderId) first, purely to decide between
    // OrderNotFound and SymbolNotFound. OrderBook::getOrder does NOT take
    // bookLock_, so that read raced any concurrent erase of orderLookup_ —
    // ThreadSanitizer on Linux caught it in ShutdownCancelRaceTest, where
    // gracefulShutdown's cancelDayOrders sweep erases while a client cancel
    // reads.
    //
    // It is the same getOrder()-then-cancel() pattern that cancelOrderReleasing
    // was introduced to replace (see its declaration): H1 moved the exposure
    // READ inside the lock but left this existence LOOKUP outside it. The
    // OrderExposure already reports `found` from inside the critical section,
    // so the pre-check was both racy and redundant — and dropping it removes a
    // lookup from the cancel path rather than adding one.
    // P2-9: release the cancelled order's working exposure before it leaves the
    // book (read remaining qty while the order still exists).
    // H1: read the exposure inside cancelOrderReleasing's own critical
    // section. The previous form dereferenced an UNLOCKED getOrder()
    // pointer into orderPool_, with no guarantee the order was still
    // alive — a concurrent fill or a shutdown sweep frees that slot.
    // One lock acquisition either way: cancelOrderReleasing() does the same
    // work as cancelOrder() plus reading three fields, which is cheaper than
    // taking bookLock_ a second time to ask about ownership separately.
    const auto exposure = book->cancelOrderReleasing(orderId, requester);
    if (exposure.denied) [[unlikely]] {
        return rejectedAsync(RejectReason::NotOrderOwner);
    }
    if (!exposure.found) [[unlikely]] {
        return rejectedAsync(RejectReason::OrderNotFound);
    }
    if (positionLimitsActive_.load(std::memory_order_relaxed)) {
        releasePosition(exposure);
    }
    if (journal_) {
        {
            std::lock_guard<std::mutex> lock(journalMutex_);
            journal_->logCancelOrder(orderId, symbolId);
        }
        maybeTriggerAutoCheckpoint();
    }
    return acceptedAsync(sequenceId);
}

// C4: see the declaration in MatchingEngine.h for what this trades away.
bool MatchingEngine::enableDurableClientAcks(bool on) {
    if (!on) {
        // Never strand events that are already held: they describe writes that
        // did happen, and a client that never hears about them is worse off
        // than one told slightly early.
        durabilityGate_.releaseAllForShutdown();
        durabilityGate_.setEnabled(false);
        if (journal_) journal_->setOnDurable(nullptr);
        books_.forEach([this](SymbolId, const std::unique_ptr<OrderBook>& book) {
            if (book && book->eventListener() == &durabilityGate_)
                book->setEventListener(durabilityGate_.downstream());
        });
        return true;
    }

    // The async path acks at enqueue, before matching has even run, so gating
    // the event dispatch would leave that ack exactly as undurable as it is
    // now while advertising a guarantee. Refuse rather than half-provide.
    if (async_) return false;
    if (!journal_) return false;

    // Interpose on every book's client-facing listener. The gate forwards to
    // whatever the application registered, so nothing is stolen — but a
    // setEventListener() call AFTER this point replaces the gate and silently
    // turns the guarantee off, which is why this is enabled once at startup.
    books_.forEach([this](SymbolId, const std::unique_ptr<OrderBook>& book) {
        if (!book) return;
        if (book->eventListener() != &durabilityGate_) {
            durabilityGate_.setDownstream(book->eventListener());
            book->setEventListener(&durabilityGate_);
        }
    });

    durableEntries_.store(journal_->entriesAppended(), std::memory_order_relaxed);
    // The counter is atomic because Journal's io_uring path reaps completions
    // on its own thread and fires onDurable_ from there — independently of
    // async_, since the ring is selected by SyncPolicy, not by engine mode. The
    // previous non-atomic `durableEntries_ += n` was therefore a genuine race
    // against processOrder's read, reachable today.
    //
    // The drain stays HERE rather than moving to the order-processing thread.
    // I tried moving it and DurableAckTest caught the reason within seconds:
    // durability becomes true at moments when no order is being processed — an
    // explicit flush(), a later order filling the batch, a checkpoint — and a
    // drain that only runs inside processOrder leaves those events held until
    // some unrelated order happens to arrive. On an idle book, that is forever.
    //
    // Which is the real shape of the async problem: a deferred drain needs a
    // thread that notices durability with nothing else going on. That, plus
    // one gate per concurrently-processing thread (the capture model holds a
    // single in-flight order), is what enabling async actually requires.
    journal_->setOnDurable([this](size_t n) {
        durableEntries_.fetch_add(n, std::memory_order_release);
        durabilityGate_.releaseThrough(
            durableEntries_.load(std::memory_order_acquire));
    });
    durabilityGate_.setEnabled(true);
    return true;
}

bool MatchingEngine::modifyOrder(SymbolId symbolId, OrderId orderId, Quantity newQty,
                                 ParticipantId requester) {
    return submitModify(symbolId, orderId, newQty, requester).isAccepted();
}

SubmitResult MatchingEngine::submitModify(SymbolId symbolId, OrderId orderId,
                                          Quantity newQty, ParticipantId requester) {
    if (!running_.load(std::memory_order_acquire)) {
        return rejectedAsync(RejectReason::EngineStopped);
    }

    // OrderBook::modifyOrder refuses this too, and that guard is the one that
    // actually protects the book. Repeating it here is about the ANSWER the
    // client gets: on the async path the request would otherwise be queued, be
    // accepted to the client's face, be refused by the worker, and never be
    // mentioned again. An accept that silently means no is worse than a reject.
    if (newQty == 0) [[unlikely]] {
        return rejectedAsync(RejectReason::InvalidQuantity);
    }

    uint64_t sequenceId = nextSequenceId();

    if (async_) {
        if (!getOrderBook(symbolId)) {
            return rejectedAsync(RejectReason::SymbolNotFound);
        }

        OrderRequest req{};
        req.type = OrderRequest::Type::Modify;
        req.symbolId = symbolId;
        req.orderId = orderId;
        req.newQty = newQty;
        req.participantId = requester;
        req.ingressTsc = Utils::latencyClockTicks();  // x86: TSC; else steady_clock ns
        if (!enqueueSafe(getThreadIndex(symbolId), req)) {
            return rejectedAsync(RejectReason::QueueBackpressure);
        }
        return acceptedAsync(sequenceId);
    }

    auto* book = getOrderBook(symbolId);
    if (!book) {
        return rejectedAsync(RejectReason::SymbolNotFound);
    }
    RejectReason modifyReason = RejectReason::None;
    bool modified = book->modifyOrder(orderId, newQty, modifyReason, requester);
    if (modified && journal_) {
        {
            std::lock_guard<std::mutex> lock(journalMutex_);
            journal_->logModifyOrder(orderId, symbolId, newQty);
        }
        maybeTriggerAutoCheckpoint();
    }
    return modified ? SubmitResult::accepted(sequenceId)
                    : SubmitResult::rejected(modifyReason);
}

bool MatchingEngine::cancelReplace(SymbolId symbolId, OrderId orderId, Price newPrice,
                                   Quantity newQty, ParticipantId requester) {
    return submitCancelReplace(symbolId, orderId, newPrice, newQty, requester).isAccepted();
}

SubmitResult MatchingEngine::submitCancelReplace(SymbolId symbolId, OrderId orderId,
                                                 Price newPrice, Quantity newQty,
                                                 ParticipantId requester) {
    if (!running_.load(std::memory_order_acquire)) {
        return rejectedAsync(RejectReason::EngineStopped);
    }

    uint64_t sequenceId = nextSequenceId();

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
        req.participantId = requester;
        req.ingressTsc = Utils::latencyClockTicks();  // x86: TSC; else steady_clock ns
        if (!enqueueSafe(getThreadIndex(symbolId), req)) {
            return rejectedAsync(RejectReason::QueueBackpressure);
        }
        return acceptedAsync(sequenceId);
    }

    auto* book = getOrderBook(symbolId);
    if (!book) {
        return rejectedAsync(RejectReason::SymbolNotFound);
    }

    RejectReason replaceReason = RejectReason::None;
    bool replaced = book->cancelReplace(orderId, newPrice, newQty, replaceReason,
                                        requester);
    if (replaced && journal_) {
        {
            std::lock_guard<std::mutex> lock(journalMutex_);
            journal_->logCancelReplace(orderId, symbolId, newPrice, newQty);
        }
        maybeTriggerAutoCheckpoint();
    }
    return replaced ? SubmitResult::accepted(sequenceId)
                    : SubmitResult::rejected(replaceReason);
}

uint64_t MatchingEngine::killSwitch(ParticipantId participantId) {
    if (async_) {
        // C5: undroppable, and verified complete by recount before returning.
        sweepAndVerify(participantId);
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
    for (SymbolId symbolId : symbolIds_) {
        std::function<void(OrderId)> onExpire;
        if (journal_) {
            onExpire = [this, symbolId](OrderId id) {
                std::lock_guard<std::mutex> lock(journalMutex_);
                journal_->logCancelOrder(id, symbolId);
            };
        }
        if (auto* book = getOrderBook(symbolId)) {
            book->expireOrders(currentTime, onExpire);
        }
    }
}

bool MatchingEngine::enableJournal(const std::string& path) {
    std::lock_guard<std::mutex> lock(journalMutex_);
    journal_ = std::make_unique<Journal>(path, Journal::SyncPolicy::GroupCommit, 64);
    return !journal_->recoveryFailed();
}

// Which book holds `orderId` for a Cancel/Modify/CancelReplace record.
//
// `recorded` is the entry's symbolId. Journals written after those records
// began carrying one name the book directly, which is both correct and O(1).
// Older journals left the field 0, so fall back to the scan this used to do
// unconditionally — no worse than before for them, and exact for everything
// written since.
//
// The scan is why duplicate order ids across symbols silently lost orders on
// recovery: it takes whichever book it reaches first, and nothing enforces
// global id uniqueness (OrderBook's duplicate check is per-book).
OrderBook* MatchingEngine::bookHoldingOrder(SymbolId recorded, OrderId orderId) {
    if (auto* book = getOrderBook(recorded);
        book && book->getOrder(orderId)) {
        return book;
    }
    for (SymbolId symbolId : symbolIds_) {
        if (auto* book = getOrderBook(symbolId);
            book && book->getOrder(orderId)) {
            return book;
        }
    }
    return nullptr;
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
            if (auto* book = bookHoldingOrder(entry.symbolId, entry.orderId)) {
                book->cancelOrder(entry.orderId);
            }
            break;
        }
        case JournalEntry::Type::ModifyOrder: {
            if (auto* book = bookHoldingOrder(entry.symbolId, entry.orderId)) {
                book->modifyOrder(entry.orderId, entry.newQty);
            }
            break;
        }
        case JournalEntry::Type::CancelReplace: {
            if (auto* book = bookHoldingOrder(entry.symbolId, entry.orderId)) {
                book->cancelReplace(entry.orderId, entry.newPrice, entry.newQty);
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
        if (auto* book = bookHoldingOrder(entry.symbolId, entry.orderId)) {
            book->cancelOrder(entry.orderId);
            applied = true;
        }
        break;
    }
    case JournalEntry::Type::ModifyOrder: {
        if (auto* book = bookHoldingOrder(entry.symbolId, entry.orderId)) {
            book->modifyOrder(entry.orderId, entry.newQty);
            applied = true;
        }
        break;
    }
    case JournalEntry::Type::CancelReplace: {
        if (auto* book = bookHoldingOrder(entry.symbolId, entry.orderId)) {
            book->cancelReplace(entry.orderId, entry.newPrice, entry.newQty);
            applied = true;
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
            journal_->logCancelOrder(entry.orderId, entry.symbolId);
            break;
        case JournalEntry::Type::ModifyOrder:
            journal_->logModifyOrder(entry.orderId, entry.symbolId, entry.newQty);
            break;
        case JournalEntry::Type::CancelReplace:
            journal_->logCancelReplace(entry.orderId, entry.symbolId,
                                       entry.newPrice, entry.newQty);
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

    // A checkpoint REPLACES the journal, so every record it discards must be
    // represented in the snapshot it replaces them with. The whole correctness
    // argument is about that one property.
    //
    // The previous version read the append counter AFTER gathering the book,
    // which made it a detector wired to a no-op:
    //
    //   1. An order journaled between the gather and the counter read was
    //      already counted by the time the counter was read, so the equality
    //      held and the rename discarded it WITH NO WARNING. It had been
    //      fsynced and the client had been acked.
    //   2. When the counter DID differ, the fallback called rewriteAtomically
    //      with the same lambda over the same stale `resting` vector — so it
    //      renamed anyway, from pre-gather state. Both branches destroyed the
    //      record; one of them logged about it.
    //
    // Reading the counter FIRST is what makes the check sound, and it works
    // because of an ordering that already exists: processOrder mutates the
    // book before it journals. So any append counted here had already landed
    // in the book before the gather ran, and is therefore in the snapshot.
    // Anything appended after the counter read changes it, and is detected.
    //
    // On a mismatch the snapshot is simply thrown away and re-gathered. It is
    // never renamed from stale state — losing an acked order is worse than not
    // checkpointing, and a checkpoint that does not happen is visible in the
    // journal's size while a lost order is visible nowhere.
    constexpr int kMaxAttempts = 3;

    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        uint64_t appendsBefore = 0;
        {
            std::lock_guard<std::mutex> lock(journalMutex_);
            if (!journal_) {
                return;
            }
            appendsBefore = journal_->entriesAppended();
        }

        // Phase 1: a point-in-time copy of every resting order, under each
        // book's own lock and never holding journalMutex_. The lock order is
        // bookMutex_ -> bookLock_ -> journalMutex_ because the expiry path
        // takes it that way; inverting it here would deadlock a concurrent
        // ExpireCheck. Order copies are read-only value snapshots — the
        // intrusive next/prev are never dereferenced.
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

        const auto writeSnapshot = [&](Journal& snapshotJournal) {
            for (const auto& [symbolId, order] : resting) {
                snapshotJournal.logSnapshot(order.id, order.participantId, symbolId,
                                            order.side, order.price, order.remainingQty,
                                            order.type, order.timeInForce, order.expiryTime,
                                            order.stopPrice, order.stopLimitPrice,
                                            order.displayQty, order.pegType,
                                            order.pegOffset, order.trailAmount,
                                            order.minQty, order.isHidden);
            }
        };

        // Phase 2: build the replacement OUTSIDE journalMutex_. Writing every
        // resting order plus a durability barrier is the expensive part, and
        // it touches nothing the live journal owns.
        if (!journal_->prepareRewrite(writeSnapshot)) {
            obSink().log(obEvent("checkpoint_prepare_failed", LogSeverity::Error)
                             .kv("orders", (unsigned long long)resting.size()));
            return;
        }

        // Phase 3: swap, but only if nothing was appended since the counter
        // read above.
        {
            std::lock_guard<std::mutex> lock(journalMutex_);
            if (!journal_) {
                return;
            }
            if (journal_->entriesAppended() == appendsBefore) {
                if (!journal_->commitRewrite()) {
                    // Previously discarded. A failed rename leaves the old
                    // journal in place, which is safe — but it means the
                    // checkpoint silently did not happen, and the journal goes
                    // on growing with nobody aware of it.
                    obSink().log(obEvent("checkpoint_commit_failed", LogSeverity::Error)
                                     .kv("orders", (unsigned long long)resting.size()));
                }
                return;
            }
        }

        obSink().log(obEvent("checkpoint_raced_appends", LogSeverity::Warn)
                         .kv("attempt", (long long)attempt)
                         .kv("appends_before", (unsigned long long)appendsBefore)
                         .kv("orders", (unsigned long long)resting.size()));
    }

    // Out of attempts. Under sustained load a quiet moment may simply not
    // occur; the journal keeps growing, which is recoverable and observable.
    // Renaming stale state instead would not be either.
    obSink().log(obEvent("checkpoint_abandoned", LogSeverity::Error)
                     .kv("attempts", (long long)kMaxAttempts));
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
