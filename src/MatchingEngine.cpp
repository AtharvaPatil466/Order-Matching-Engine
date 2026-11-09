#include "MatchingEngine.h"
#include "LatencyTracker.h"
#include "Utils.h"  // OrderMatcher::Utils::rdtsc / calibrateTsc / tscTicksToNs
#include "Metrics.h"
#include "StructuredLog.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <system_error>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#if defined(__linux__)
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <pthread/qos.h>
#endif

namespace OrderMatcher {

namespace {

// PROTO-1: every enum an order carries is range-checked here, where every
// ingress meets — TcpGateway, admin, in-process callers and
// the async queue behind them. In the gateway alone (0.13) the rest handed a
// side of 2 to the book, which rested it in the asks, and an orderType past
// LOC rested as a limit. Bounded by the LAST enumerator of each type in
// Types.h: appending one there moves the bound here.
bool orderEnumsDefined(Side side, OrderType type, TimeInForce tif, PegType peg) {
    return static_cast<uint8_t>(side) <= static_cast<uint8_t>(Side::Sell) &&
           static_cast<uint8_t>(type) <= static_cast<uint8_t>(OrderType::LOC) &&
           static_cast<uint8_t>(tif)  <= static_cast<uint8_t>(TimeInForce::DAY) &&
           static_cast<uint8_t>(peg)  <= static_cast<uint8_t>(PegType::PrimaryPeg);
}

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
// id straight back to the client (GatewayResponse.sequenceId) and AdminServer prints
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
thread_local uint64_t t_seqOwner = 0;   // seqOwnerTag_ of the engine that leased the block

// D1: ids restarted at 1 in every process, so a restarted engine handed its
// clients ids it had already given them. They are leased from the journal now,
// kSeqLease at a time (one record and one flush per lease), and replay resumes
// the allocator above the last lease.
// A thread's block outlives the engine that leased it, so each block is tagged
// with its engine: a second engine on the same thread (every test binary, the
// Python bridge) takes a fresh block and leases it in its own journal, instead
// of handing out ids from the first engine's block that its journal never saw.
constexpr uint64_t kSeqLease = uint64_t{1} << 20;

void raiseSequenceBase(uint64_t floor) {
    uint64_t cur = g_seqBlockBase.load(std::memory_order_relaxed);
    while (cur < floor &&
           !g_seqBlockBase.compare_exchange_weak(cur, floor, std::memory_order_relaxed)) {}
}

// waitForDrain's escalation ladder: spin, then yield, then sleep. See
// waitForDrain for why it polls at all.
constexpr uint64_t kDrainSpins  = 2048;
constexpr uint64_t kDrainYields = 2048;
constexpr auto     kDrainPollSleep = std::chrono::microseconds(50);

// Pin the calling worker to `cpu` (-1: leave it on the mask it inherited).
// Linux pins; macOS has no CPU affinity, only an affinity tag per worker that
// asks the scheduler to spread them — a placement hint, kept as it always was.
void pinCurrentThreadToCore([[maybe_unused]] int cpu, [[maybe_unused]] size_t threadIndex) {
#if defined(__linux__)
    if (cpu < 0) return;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    if (const int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
        rc != 0) {
        obSink().log(obEvent("worker_pin_failed", LogSeverity::Warn)
                         .kv("thread", (long long)threadIndex)
                         .kv("cpu", (long long)cpu)
                         .kv("errno", (long long)rc));
    }
#elif defined(__APPLE__)
    thread_affinity_policy_data_t policy{
        static_cast<integer_t>(threadIndex + 1)
    };
    (void)thread_policy_set(mach_thread_self(), THREAD_AFFINITY_POLICY,
                            reinterpret_cast<thread_policy_t>(&policy), 1);
#endif
}

// The CPU worker i pins itself to, -1 for none. A configured list
// (setWorkerCores, the worker_cores key) is used in order once
// Utils::workerCoresError passes it. Otherwise worker i runs on the i-th CPU
// the process may use — CPU i when every CPU is allowed — and a worker past the
// last one keeps the mask it inherited. It used to be CPU i whatever the mask
// said, so `taskset -c 2-5` put the workers on 0-3 and a cpuset or a second
// engine shared their cores.
std::vector<int> workerCpusFor(size_t workers, const std::vector<int>& configured) {
    const std::vector<int> allowed = Utils::allowedCpus();
    if (!configured.empty()) {
        if (const std::string err = Utils::workerCoresError(configured, workers, allowed);
            !err.empty()) {
            throw std::invalid_argument("worker_cores: " + err);
        }
        return {configured.begin(), configured.begin() + static_cast<std::ptrdiff_t>(workers)};
    }
    std::vector<int> cpus(workers, -1);
    for (size_t i = 0; i < workers && i < allowed.size(); ++i) cpus[i] = allowed[i];
    return cpus;
}

// "2,3,-" for the engine_start log: worker i's CPU, "-" when unpinned.
std::string cpuList(const std::vector<int>& cpus) {
    std::string out;
    for (size_t i = 0; i < cpus.size(); ++i) {
        if (i) out += ',';
        out += cpus[i] < 0 ? std::string("-") : std::to_string(cpus[i]);
    }
    return out;
}

// Best effort: the checkpoint thread yields the CPU to the workers. Per-thread
// nice on Linux (nice is per task there); utility QoS on macOS.
void lowerCurrentThreadPriority() {
#if defined(__linux__)
    const int rc = setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 10);
    (void)rc;
#elif defined(__APPLE__)
    const int rc = pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    (void)rc;
#endif
}

} // namespace

MatchingEngine::MatchingEngine() {
    ensureDefaultSymbol();
    killSwitch_.setHaltFunction([this] { syncKillSwitchHalts(); });
}

// GW-9: make the books say what the graduated kill switch says. A book it
// halts goes to Halted — journaled, on its worker — and one it no longer halts,
// that it halted, goes back to the state it left. A book halted for any other
// reason is left alone either way. Control plane: waits for the workers.
void MatchingEngine::syncKillSwitchHalts() {
    std::vector<SymbolId> toHalt;
    std::vector<std::pair<SymbolId, TradingState>> toResume;
    {
        std::lock_guard<std::mutex> lock(bookMutex_);
        for (SymbolId s : symbolIds_) {
            const OrderBook* book = getOrderBook(s);
            if (!book) continue;
            const auto mine = std::find(killHalted_.begin(), killHalted_.end(), s);
            if (killSwitch_.isSymbolHalted(s)) {
                if (mine == killHalted_.end() &&
                    book->getTradingState() != TradingState::Halted) {
                    toHalt.push_back(s);
                    killHalted_.push_back(s);
                }
            } else if (mine != killHalted_.end()) {
                toResume.emplace_back(s, book->preHaltState());
                killHalted_.erase(mine);
            }
        }
    }
    if (!toHalt.empty()) (void)setTradingStateBatch(toHalt, TradingState::Halted);
    for (const auto& [s, state] : toResume) (void)setTradingStateBatch({s}, state);
}

MatchingEngine::~MatchingEngine() {
    stopExpiryTimer();
    if (async_) {
        stopAsync();
    } else {
        stop();
    }
}

bool MatchingEngine::admit(size_t threadIndex) {
    std::atomic<uint64_t>& gate = threadStats_[threadIndex].admission;
    if (gate.fetch_add(1, std::memory_order_acquire) & kQueueClosed) {
        gate.fetch_sub(1, std::memory_order_release);
        return false;
    }
    return true;
}

void MatchingEngine::leave(size_t threadIndex) {
    threadStats_[threadIndex].admission.fetch_sub(1, std::memory_order_release);
}

// ponytail: two RMWs per order on the ThreadStats line the producer already
// RMWs for `submitted`. If they ever show up in a profile, fold the closed bit
// into `submitted` itself (bump before the push, undo on failure) — that needs
// waitForDrain and the worker backstop to tolerate the undo.
bool MatchingEngine::enqueueSafe(size_t threadIndex, const OrderRequest& req) {
    if (!admit(threadIndex)) return false;
    const bool queued = pushWithBackpressure(threadIndex, req);
    leave(threadIndex);
    return queued;
}

bool MatchingEngine::pushWithBackpressure(size_t threadIndex, const OrderRequest& req) {
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
// forever: stopAsync's Shutdown, pushed after the admission gates are closed,
// so it deliberately does not pass through them.
// Does the submitted_/wakeup bookkeeping ONLY on success: counting a
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
    // The one way a control message is not delivered: the engine is stopping,
    // its workers drain and exit, and nothing is left for it to act on.
    if (!admit(threadIndex)) {
        obSink().log(obEvent("control_after_stop", LogSeverity::Warn)
            .kv("thread", (long long)threadIndex)
            .kv("request_type", (long long)static_cast<int>(req.type)));
        return;
    }
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
    leave(threadIndex);
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
    // Through addSymbolLocked, like every other book, so it gets the engine
    // listener: inserted bare, its fills never reached onTrade.
    std::lock_guard<std::mutex> lock(bookMutex_);
    addSymbolLocked(0);
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
    // First, so a bad worker_cores list throws before anything is started.
    const size_t workers = numThreads > 0 ? numThreads : 1;
    std::vector<int> workerCpus = workerCpusFor(workers, workerCores_);

    // Calibrate the TSC once here, while still single-threaded, so the ~5 ms
    // sample never lands on a worker's hot path. Used by the per-order e2e
    // latency measurement (Utils::rdtsc / tscTicksToNs).
    Utils::calibrateTsc();

    numThreads_ = workers;
    workerCpus_ = std::move(workerCpus);
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
    checkpointThreadStop_.store(false, std::memory_order_release);
    checkpointThread_ = std::thread(&MatchingEngine::checkpointLoop, this);

    // Sync start() has always logged engine_start; the async path — the one
    // production actually runs — logged nothing at all, so an operator who
    // enabled logging saw silence at the single most important moment.
    obSink().log(obEvent("engine_start")
                     .kv("mode", "async")
                     .kv("symbols", (unsigned long long)symbolIds_.size())
                     .kv("threads", (unsigned long long)numThreads_)
                     .kv("worker_cores", cpuList(workerCpus_))
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

    // Deliberately NOT installed, blocked on a decision this code cannot make:
    // memory — CapacityThresholds.memoryUsagePct is a fraction of a limit, and
    // no limit exists anywhere in the config or the code. RSS over total host
    // RAM is not it (the engine shares the host); the real ceiling is the
    // container/cgroup limit the deployment sets.
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
    // Wall-clock nanoseconds since the Unix epoch — the unit of a client's GTD
    // expiryTime. Not high_resolution_clock: that is system_clock under
    // libstdc++ but steady_clock (time since boot) under libc++, so the same
    // expiryTime meant a wall-clock instant on Linux and an uptime on macOS.
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
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

    // Before the workers: a checkpoint in progress takes its cut from them.
    // One that is running finishes; a pending one is dropped (gracefulShutdown
    // runs its own).
    checkpointThreadStop_.store(true, std::memory_order_release);
    checkpointPending_.store(true, std::memory_order_release);
    checkpointPending_.notify_one();
    if (checkpointThread_.joinable()) checkpointThread_.join();
    checkpointPending_.store(false, std::memory_order_release);

    // LF-5: no producer may be inside a push when the queues are destroyed
    // below, and none may land an order behind the Shutdown message, where it
    // is acked and never applied. Turn new submissions away, close every
    // queue's gate, and wait out the producers already through it. The workers
    // are still running, so a producer spinning on a full ring gets its slot.
    running_.store(false, std::memory_order_release);
    for (size_t i = 0; i < numThreads_; ++i) {
        threadStats_[i].admission.fetch_or(kQueueClosed, std::memory_order_acq_rel);
    }
    for (size_t i = 0; i < numThreads_; ++i) {
        while (threadStats_[i].admission.load(std::memory_order_acquire) != kQueueClosed) {
            std::this_thread::yield();
        }
    }

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
    //
    // Per worker, never summed. A fast worker that has already run orders
    // submitted after this snapshot pushes the TOTAL past the target while a
    // slow worker has not yet reached the message the caller is waiting on —
    // so setTradingStateBatch returned with books still unflipped, and the
    // caller then read state the worker was writing (THR-1).
    std::vector<uint64_t> targets(numThreads_);
    for (size_t i = 0; i < numThreads_; ++i) {
        targets[i] = threadStats_[i].submitted.load(std::memory_order_acquire);
    }
    uint64_t spins = 0;
    for (size_t i = 0; i < numThreads_; ++i) {
        while (threadStats_[i].processed.load(std::memory_order_acquire) < targets[i]) {
            if (spins < kDrainSpins) {
                cpuRelax();
            } else if (spins < kDrainSpins + kDrainYields) {
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(kDrainPollSleep);
            }
            ++spins;
        }
    }
}

void MatchingEngine::workerLoop(size_t threadIndex) {
    pinCurrentThreadToCore(workerCpus_[threadIndex], threadIndex);

    OrderRequest req{};
    size_t idleSpins = 0;
    uint64_t observedWake = queueWakeups_[threadIndex].load(std::memory_order_relaxed);

    while (true) {
        if (cutRequested_.load(std::memory_order_acquire)) [[unlikely]] {
            holdForCut();   // a checkpoint is copying the books
        }
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
            // D3 while idle: on a core another thread wants, each yield cedes
            // a whole timeslice — 128 of them took ~360 ms against one busy
            // thread — so committing only after them left acked orders in
            // memory for that long. Apply the commit delay between yields.
            if (journal_) commitJournalIfDue();
            std::this_thread::yield();
            continue;
        }

        // D3: about to park, so nothing more joins the journal's batch until
        // the next order arrives — which on a quiet book may be never. Commit
        // it now rather than leave acked orders in memory for kill -9.
        if (journal_) commitJournal();

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
        // THR-13: the checkpoint thread runs it. It used to be worker 0, at
        // the next moment the engine was drained, and that worker's symbols
        // stopped matching for as long as the snapshot took to write and sync.
        if (!checkpointPending_.exchange(true, std::memory_order_acq_rel))
            checkpointPending_.notify_one();
    } else {
        checkpointInternal(true);
    }
}

uint64_t MatchingEngine::nextSequenceId() {
    if (t_seqNext == t_seqEnd || t_seqOwner != seqOwnerTag_) [[unlikely]] {
        const uint64_t base = g_seqBlockBase.fetch_add(kSeqBlock, std::memory_order_relaxed);
        t_seqOwner = seqOwnerTag_;
        t_seqNext = base + 1;
        t_seqEnd  = base + kSeqBlock + 1;
        if (base + kSeqBlock > sequenceLease_.load(std::memory_order_acquire))
            leaseSequenceIds(base + kSeqBlock);
    }
    return t_seqNext++;
}

void MatchingEngine::leaseSequenceIds(uint64_t through) {
    if (!journal_) return;   // nothing durable to promise
    std::lock_guard<std::mutex> lock(journalMutex_);
    if (through <= sequenceLease_.load(std::memory_order_relaxed)) return;
    const uint64_t lease = through + kSeqLease;
    journal_->logCounters(0, 0, lease);
    journal_->flush();   // durable before any id it covers reaches a client
    sequenceLease_.store(lease, std::memory_order_release);
}

void MatchingEngine::restoreCounters(OrderBook* book, const JournalEntry& e) {
    if (book && e.orderId != 0) {
        book->restoreNextTradeId(e.orderId);
        book->restorePrices({e.price, e.newPrice, e.newQty});
    }
    if (e.quantity == 0) return;
    uint64_t cur = sequenceLease_.load(std::memory_order_relaxed);
    while (cur < e.quantity &&
           !sequenceLease_.compare_exchange_weak(cur, e.quantity, std::memory_order_acq_rel)) {}
    raiseSequenceBase(e.quantity);   // every id from here on is above the lease
    t_seqNext = t_seqEnd = 0;        // and this thread's block is not
}

void MatchingEngine::checkpointLoop() {
    lowerCurrentThreadPriority();
    for (;;) {
        checkpointPending_.wait(false, std::memory_order_acquire);
        if (checkpointThreadStop_.load(std::memory_order_acquire)) return;
        checkpointPending_.store(false, std::memory_order_release);
        // Requests keep arriving while one runs; skip those it satisfied.
        bool needed = false;
        {
            std::lock_guard<std::mutex> lock(journalMutex_);
            needed = journal_ && journal_->needsCheckpoint(checkpointThresholdEntries_,
                                                           checkpointThresholdBytes_);
        }
        if (needed) checkpointInternal(true);
    }
}

// The cut. Workers never take bookMutex_, so a checkpoint holding it can wait
// for them here; anything else holding bookMutex_ is waited out before this.
void MatchingEngine::pauseWorkersForCut() {
    std::unique_lock<std::mutex> lk(cutMutex_);
    cutRequested_.store(true, std::memory_order_release);
    for (size_t i = 0; i < numThreads_; ++i) {
        queueWakeups_[i].fetch_add(1, std::memory_order_release);   // unpark idlers
        queueWakeups_[i].notify_one();
    }
    // ponytail: polls so a worker that EXITED instead of arriving still counts.
    const auto allHere = [this] {
        size_t gone = 0;
        for (size_t i = 0; i < numThreads_; ++i)
            if (threadStats_[i].exited.load(std::memory_order_acquire)) ++gone;
        return workersAtCut_ + gone >= numThreads_;
    };
    while (!cutCv_.wait_for(lk, std::chrono::milliseconds(1), allHere)) {}
}

void MatchingEngine::resumeWorkersAfterCut() {
    {
        std::lock_guard<std::mutex> lk(cutMutex_);
        cutRequested_.store(false, std::memory_order_release);
    }
    cutCv_.notify_all();
}

void MatchingEngine::holdForCut() {
    std::unique_lock<std::mutex> lk(cutMutex_);
    ++workersAtCut_;
    cutCv_.notify_all();
    cutCv_.wait(lk, [this] { return !cutRequested_.load(std::memory_order_relaxed); });
    --workersAtCut_;
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
            releaseWorking(req.participantId, req.side, req.qty);
            book->emitReject(req.orderId, req.qty, RejectReason::KillSwitchActive);
            break;
        }
        // GW-9, the same way: a halt or a participant kill since the submit.
        if (killSwitch_.engaged() && killSwitch_.blocks(req.symbolId, req.participantId))
            [[unlikely]] {
            killSwitchRejects_.fetch_add(1, std::memory_order_relaxed);
            releaseWorking(req.participantId, req.side, req.qty);
            book->emitReject(req.orderId, req.qty, RejectReason::KillSwitchActive);
            break;
        }
        // D4, the same way: an order queued before the journal halted must
        // not match now. It could never be made durable.
        if (journalHalted()) [[unlikely]] {
            releaseWorking(req.participantId, req.side, req.qty);
            book->emitReject(req.orderId, req.qty, RejectReason::JournalHalted);
            break;
        }

        if (const RejectReason rr = checkBookValuedNotional(
                *book, req.symbolId, req.participantId, req.side, req.orderType, req.price,
                req.qty, req.stopPrice, req.stopLimitPrice, req.trailAmount);
            rr != RejectReason::None) [[unlikely]] {
            releaseWorking(req.participantId, req.side, req.qty);
            book->emitReject(req.orderId, req.qty, rr);
            break;
        }

        // Hierarchical pre-trade risk gate (no-op unless configured). A Market
        // or stop order is valued at the price it can reach (STP-12); Market
        // orders skip the band (0), the rest measure from the mid. This thread
        // owns the book, so no lock.
        if (!preTradeRiskCheck(req.participantId, req.side,
                               book->notionalPrice(req.side, req.orderType, req.price,
                                                   req.stopPrice, req.stopLimitPrice,
                                                   req.trailAmount),
                               req.qty,
                               req.orderType == OrderType::Market ? 0 : book->getMidPrice(),
                               req.qty)) {
            releaseWorking(req.participantId, req.side, req.qty);
            book->emitReject(req.orderId, req.qty, RejectReason::RiskLimitBreached);
            break;
        }

        const TradingState stateBefore = book->getTradingState();
        AddOrderResult result = book->addOrder(req.orderId, req.participantId, req.side,
                                               req.price, req.qty, req.orderType,
                                               req.stopPrice, req.displayQty, req.tif,
                                               req.expiryTime, req.stopLimitPrice,
                                               req.pegType, req.pegOffset, req.trailAmount,
                                               req.minQty, req.hidden);
        // Refused: no order lives, nothing traded. Admitted, the book answers
        // for the reservation from here.
        if (!std::holds_alternative<OrderId>(result)) {
            releaseWorking(req.participantId, req.side, req.qty);
            releaseHierarchical(req.participantId, req.side, req.qty);
        }
        if (journal_ && std::holds_alternative<OrderId>(result)) {
            {
                std::lock_guard<std::mutex> lock(journalMutex_);
                logBreakerTrip(req.symbolId, stateBefore, book->getTradingState());
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
        // req.participantId is the requester here, not the order's owner: for
        // a Cancel it is whoever asked, and kAnyParticipant when the caller is
        // internal. A denied cancel must not journal, so bail first. The
        // order's working quantity goes back inside the book (untrackOrder).
        const auto exposure =
            book->cancelOrderReleasing(req.orderId, req.participantId);
        if (exposure.denied) [[unlikely]] {
            obSink().log(obEvent("cancel_denied_not_owner", LogSeverity::Warn)
                             .kv("order_id",  (unsigned long long)req.orderId)
                             .kv("symbol_id", (long long)req.symbolId)
                             .kv("requester", (unsigned long long)req.participantId));
            return;
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
        // A repriced order matches as an aggressor: its OCO group and the
        // trade-driven consumers see those fills like any other (GTS-4).
        if (ocoActive_.load(std::memory_order_relaxed) ||
            observersActive_.load(std::memory_order_relaxed)) {
            driveOco(req.symbolId, book);
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
            // P2-8: kill switch — cancel every resting order on this worker's
            // books, or every one of a participant's. One path for both (D6):
            // the participant kill used cancelAllForParticipant, which neither
            // journals nor releases exposure, so a restart put the killed
            // orders back and the position limit kept counting them. The
            // worker owns these books, so the non-locking forEachOrder walk is
            // safe; ids are collected first, then cancelled (cancelOrder takes
            // bookLock_, so it must run outside the walk).
            const bool everyone = req.participantId == kKillAllParticipants;
            std::vector<OrderId> ids;
            book->forEachOrder([&](const Order& o) {
                if (everyone || o.participantId == req.participantId) ids.push_back(o.id);
            });
            for (OrderId id : ids) {
                book->cancelOrder(id);  // its working quantity goes back in the book
                if (journal_) {
                    std::lock_guard<std::mutex> lock(journalMutex_);
                    journal_->logCancelOrder(id, book->getSymbolId());
                }
            }
        }
        break;
    }
    case OrderRequest::Type::TradingState:
    case OrderRequest::Type::Uncross:
        (void)applySessionControl(req);
        break;
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
                // H3: the volatility-auction timer runs on the owning worker.
                if (book->volatilityAuctionDue(req.expiryTime)) {
                    OrderRequest reopen{};
                    reopen.type = OrderRequest::Type::TradingState;
                    reopen.symbolId = symbolId;
                    reopen.newQty = static_cast<Quantity>(TradingState::Continuous);
                    (void)applySessionControl(reopen);
                }
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
    // concurrent off-hot-path reader (snapshot/checkpoint)
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
    // slot, so market-data listeners are untouched). It only
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
    if (auto* bk = getOrderBook(symbolId)) {
        bk->setEngineListener(oco.get());
        // THR-10: a book cancels an expired maker its match meets, on the
        // sweep's clock, journaled as the sweep journals one.
        bk->setExpiryHooks([this] { return expiryNow(); },
                           [this, symbolId](OrderId id) {
                               std::lock_guard<std::mutex> lock(journalMutex_);
                               if (journal_) journal_->logCancelOrder(id, symbolId);
                           });
    }
    ocoListeners_.insert(symbolId, std::move(oco));

    rebuildThreadSymbolIndex();
}

void MatchingEngine::OcoBookListener::onOrderUpdate(const OrderUpdate& u) {
    // Buffer executions only while OCO is active; drained by driveOco after
    // the triggering request completes. A partial fill counts — the first
    // execution of any leg wins. Keyed on executed quantity, not status: an
    // order cut short after real fills ends Cancelled / CancelledBySTP with
    // filledQty > 0 (GTDIFF-4), and a zero-fill STP kill still cannot win (C1).
    if (ocoActive && ocoActive->load(std::memory_order_relaxed) && u.filledQty > 0) {
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
                // Out of each side's working quantity, into its position.
                riskManager_.onFill(t.buyerId, Side::Buy, t.price, t.quantity);
                riskManager_.release(t.buyerId, Side::Buy, t.quantity);
                riskManager_.onFill(t.sellerId, Side::Sell, t.price, t.quantity);
                riskManager_.release(t.sellerId, Side::Sell, t.quantity);
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
                                       Quantity qty, Price referencePrice, Quantity reserve) {
    if (!riskActive_.load(std::memory_order_relaxed)) return true;
    std::lock_guard<std::mutex> lock(riskMutex_);
    return riskManager_.checkAndReserve(trader, side, price, qty, referencePrice, reserve)
        .allowed;
}

void MatchingEngine::releaseHierarchical(ParticipantId pid, Side side, Quantity qty) {
    if (!riskActive_.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(riskMutex_);
    riskManager_.release(pid, side, qty);
}

// ponytail: the observer's buffers have one drainer per book; the sync path
// gets that from its caller (one event loop in the gateway). Concurrent sync
// callers on one book with OCO, fees, CAT or hierarchical risk on would need
// the swap in driveOco under the book lock and per-call scratch buffers.
uint64_t MatchingEngine::driveOcoSync(SymbolId symbolId, OrderBook* book,
                                      uint64_t appendOrdinal) {
    if (!ocoActive_.load(std::memory_order_relaxed) &&
        !observersActive_.load(std::memory_order_relaxed))
        return appendOrdinal;
    driveOco(symbolId, book);
    if (!journal_ || appendOrdinal == 0) return appendOrdinal;
    std::lock_guard<std::mutex> lock(journalMutex_);
    return journal_->entriesAppended();
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
        cancelAllRestingOrders(kKillAllParticipants);
    }
}

uint64_t MatchingEngine::cancelAllRestingOrders(ParticipantId pid) {
    // Sync-mode kill-switch sweep. Lock order bookMutex_ -> bookLock_ ->
    // journalMutex_, matching expireOrders()/driveOco(). Ids are collected under
    // the book's own lock (forEachOrderLocked), then cancelled outside it so
    // cancelOrder can re-take bookLock_ without re-entrancy — it is a plain,
    // non-recursive std::mutex, so collecting and cancelling in one pass would
    // self-deadlock.
    std::lock_guard<std::mutex> lock(bookMutex_);
    const bool everyone = pid == kKillAllParticipants;
    uint64_t cancelled = 0;
    uint64_t appendOrdinal = 0;
    durabilityGate_.beginOrder();
    for (SymbolId sym : symbolIds_) {
        auto* book = getOrderBook(sym);
        if (!book) continue;
        std::vector<OrderId> ids;
        book->forEachOrderLocked([&](const Order& o) {
            if (everyone || o.participantId == pid) ids.push_back(o.id);
        });
        cancelled += ids.size();
        for (OrderId id : ids) {
            book->cancelOrder(id);  // its working quantity goes back in the book
            if (journal_) {
                std::lock_guard<std::mutex> jl(journalMutex_);
                journal_->logCancelOrder(id, book->getSymbolId());
                appendOrdinal = journal_->entriesAppended();
            }
        }
    }
    holdUntilDurable(appendOrdinal);
    return cancelled;
}

size_t MatchingEngine::cancelDayOrders() {
    std::vector<SymbolId> all;
    {
        std::lock_guard<std::mutex> lock(bookMutex_);
        all = symbolIds_;
    }
    return cancelDayOrders(all);
}

size_t MatchingEngine::endTradingSession(const std::vector<SymbolId>& symbols) {
    // The explicit session-end command (roadmap 1.8-H8 / 1.4-D9): the close,
    // then DAY orders. Process stop is not session end (gracefulShutdown keeps
    // them by default); this is the only other path that retires them.
    setTradingStateBatch(symbols, TradingState::PostClose);
    std::vector<SymbolId> closed;
    for (SymbolId s : symbols) {
        const OrderBook* book = getOrderBook(s);
        if (book && book->getTradingState() == TradingState::PostClose) closed.push_back(s);
    }
    return cancelDayOrders(closed);
}

size_t MatchingEngine::cancelDayOrders(const std::vector<SymbolId>& symbols) {
    // Session-end DAY sweep. Same lock discipline as cancelAllRestingOrders()
    // (bookMutex_ -> book->bookLock_ -> journalMutex_): collect ids under each
    // book's lock, then cancel outside it so cancelOrder can take bookLock_
    // exclusively without re-entrancy on the non-recursive mutex.
    std::lock_guard<std::mutex> lock(bookMutex_);
    size_t cancelled = 0;
    uint64_t appendOrdinal = 0;
    durabilityGate_.beginOrder();
    for (SymbolId sym : symbols) {
        auto* book = getOrderBook(sym);
        if (!book) continue;
        std::vector<OrderId> dayIds;
        book->forEachOrderLocked([&](const Order& o) {
            if (o.timeInForce == TimeInForce::DAY) dayIds.push_back(o.id);
        });
        for (OrderId id : dayIds) {
            book->cancelOrder(id);  // its working quantity goes back in the book
            if (journal_) {
                std::lock_guard<std::mutex> jl(journalMutex_);
                journal_->logCancelOrder(id, book->getSymbolId());
                appendOrdinal = journal_->entriesAppended();
            }
            ++cancelled;
            // "log at session end" — durable, per-order audit line.
            obSink().log(obEvent("day_order_cancelled_at_shutdown")
                             .kv("symbol", (long long)sym)
                             .kv("order", (unsigned long long)id));
        }
    }
    holdUntilDurable(appendOrdinal);
    return cancelled;
}

MatchingEngine::ShutdownReport MatchingEngine::gracefulShutdown(bool cancelDayOrdersNow) {
    ShutdownReport report{};

    // 1. Stop admitting new orders. Already-enqueued requests still drain.
    //    Stop expiry too: until stopAsync() (after the checkpoint below) the
    //    timer would keep posting sweeps into books that are being persisted.
    shuttingDown_.store(true, std::memory_order_release);
    stopExpiryTimer();

    // 2. Drain in-flight work so IOC remainders are cancelled during matching
    //    and any partial fill completes before we snapshot (see header).
    if (async_) {
        waitForDrain();
    }

    // 3. Only at a real session end: cancel every DAY order (journaled +
    //    logged). A process stop is not one — see the header — so by default
    //    DAY orders are persisted like everything else and restored on restart.
    if (cancelDayOrdersNow) report.dayOrdersCancelled = cancelDayOrders();

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
        // Waits out a checkpoint the checkpoint thread may still be running.
        checkpointInternal(true, /*waitIfBusy=*/true);
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
    anyRiskActive_.store(true, std::memory_order_relaxed);
    // The first limit: orders already resting reserved nothing, so count them
    // or their removal would release what was never held.
    if (!positionLimitsActive_.exchange(true, std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(bookMutex_);
        rebuildPositionsLocked();
    }
}

int64_t MatchingEngine::getPosition(ParticipantId pid) const {
    if (pid >= MAX_PARTICIPANTS) return 0;
    return filled_[pid].load(std::memory_order_relaxed) +
           working_[pid][sideIndex(Side::Buy)].load(std::memory_order_relaxed) -
           working_[pid][sideIndex(Side::Sell)].load(std::memory_order_relaxed);
}

int64_t MatchingEngine::getWorkingQty(ParticipantId pid, Side side) const {
    if (pid >= MAX_PARTICIPANTS) return 0;
    return working_[pid][sideIndex(side)].load(std::memory_order_relaxed);
}

int64_t MatchingEngine::getFilledPosition(ParticipantId pid) const {
    if (pid >= MAX_PARTICIPANTS) return 0;
    return filled_[pid].load(std::memory_order_relaxed);
}

// The limit bounds what this side could reach: working on it plus the filled
// position in its direction. Checking and adding are one step, because a check
// followed by a later add let every concurrent admission pass against the same
// figure before any of them booked (THR-3); on the async path the add came a
// whole queue later.
//
// Read order against settleFill, which moves a fill to `filled_` first and takes
// it off `working_` second, with release: an acquire of working_ that sees the
// decrement also sees the filled position it became. A read that misses the
// decrement sees the fill at most twice, which refuses too much, never too
// little; and a decrement between the read and the CAS fails the CAS.
bool MatchingEngine::tryReserveWorking(ParticipantId pid, Side side, Quantity qty) {
    std::atomic<int64_t>& working = working_[pid][sideIndex(side)];
    const int64_t limit = positionLimit_[pid].load(std::memory_order_relaxed);
    int64_t current = working.load(std::memory_order_acquire);
    for (;;) {
        const __int128 filled = filled_[pid].load(std::memory_order_relaxed);
        const __int128 reach = current + (side == Side::Buy ? filled : -filled) + qty;
        if (limit > 0 && reach > limit) return false;
        if (working.compare_exchange_weak(current, current + static_cast<int64_t>(qty),
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire))
            return true;
    }
}

void MatchingEngine::releaseWorking(ParticipantId pid, Side side, Quantity qty) {
    if (!positionLimitsActive_.load(std::memory_order_relaxed) || pid >= MAX_PARTICIPANTS) return;
    working_[pid][sideIndex(side)].fetch_sub(static_cast<int64_t>(qty), std::memory_order_release);
}

void MatchingEngine::settleFill(ParticipantId pid, Side side, Quantity qty) {
    if (pid >= MAX_PARTICIPANTS) return;
    const int64_t q = static_cast<int64_t>(qty);
    filled_[pid].fetch_add(side == Side::Buy ? q : -q, std::memory_order_relaxed);
    working_[pid][sideIndex(side)].fetch_sub(q, std::memory_order_release);
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
                                               Price price, Quantity qty, OrderType type,
                                               Quantity reserve) {
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

    // ── P2-9 POSITION LIMIT (per participant, per side, reserved here) ──────
    const bool reserving = reserve != 0 &&
        positionLimitsActive_.load(std::memory_order_relaxed) && pid < MAX_PARTICIPANTS;
    if (reserving && !tryReserveWorking(pid, side, reserve)) {
        positionRejects_.fetch_add(1, std::memory_order_relaxed);
        logRiskReject("position_limit", sym, pid, RejectReason::PositionLimitExceeded);
        return RejectReason::PositionLimitExceeded;
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
            if (reserving) releaseWorking(pid, side, reserve);
            otrRejects_.fetch_add(1, std::memory_order_relaxed);
            logRiskReject("otr", sym, pid, RejectReason::OrderToTradeRatioExceeded);
            return RejectReason::OrderToTradeRatioExceeded;
        }
    }

    return RejectReason::None;
}

RejectReason MatchingEngine::checkBookValuedNotional(const OrderBook& book, SymbolId sym,
                                                     ParticipantId pid, Side side, OrderType type,
                                                     Price price, Quantity qty, Price stopPrice,
                                                     Price stopLimitPrice, Price trailAmount) {
    if (!OrderBook::valuedOffTheBook(type) || sym >= MAX_RISK_SYMBOLS ||
        !fatFingerActive_.load(std::memory_order_relaxed))
        return RejectReason::None;
    const int64_t maxNotional = ffMaxNotional_[sym].load(std::memory_order_relaxed);
    if (maxNotional == 0) return RejectReason::None;

    Price px = book.notionalPrice(side, type, price, stopPrice, stopLimitPrice, trailAmount);
    if (px == 0) px = refPrice_[sym].load(std::memory_order_relaxed);
    if (px > 0 && orderNotional(px, qty) <= static_cast<__int128>(maxNotional))
        return RejectReason::None;

    fatFingerRejects_.fetch_add(1, std::memory_order_relaxed);
    logRiskReject("fat_finger_notional", sym, pid, RejectReason::FatFingerReject);
    return RejectReason::FatFingerReject;
}

// Runs under the book lock, from cancelReplace / modifyOrder once the book's own
// checks passed. Growth in leaves is reserved like a new order; a shrink is
// released by the book after this returns None. Kill and halt are I6's.
RejectReason MatchingEngine::admitAmend(const Order& o, Price newPrice, Quantity newTotal,
                                        Quantity newLeaves) {
    const Quantity growth = newLeaves > o.remainingQty ? newLeaves - o.remainingQty : 0;
    // A shrink at the same price is a partial cancel: it moves no price and
    // adds no notional, so the band and the notional cap are not asked. The
    // size cap still is — it may have been tightened since the order arrived.
    const bool reduceOnly = growth == 0 && newPrice == o.price;
    // GW-9: anything else is an admission, which a kill switch refuses. Here,
    // against the order's owner; submitCancelReplace could only ask about the
    // requester.
    if (!reduceOnly && (killSwitchActive_.load(std::memory_order_acquire) ||
                        (killSwitch_.engaged() && killSwitch_.blocks(o.symbolId, o.participantId))))
        [[unlikely]] {
        killSwitchRejects_.fetch_add(1, std::memory_order_relaxed);
        return RejectReason::KillSwitchActive;
    }
    if (!anyRiskActive_.load(std::memory_order_relaxed) &&
        !riskActive_.load(std::memory_order_relaxed))
        return RejectReason::None;
    const Price checkedPrice = reduceOnly ? 0 : newPrice;
    const RejectReason r = checkRiskControls(o.symbolId, o.participantId, o.side, checkedPrice,
                                             newTotal, o.type, growth);
    if (r != RejectReason::None) return r;
    // The hierarchy too, growth reserved at every tier. The book's lock is
    // held here, so its mid is read directly.
    const OrderBook* book = getOrderBook(o.symbolId);
    const Price mid = (reduceOnly || !book) ? 0 : book->getMidPrice();
    if (!preTradeRiskCheck(o.participantId, o.side, checkedPrice, newTotal, mid, growth)) {
        releaseWorking(o.participantId, o.side, growth);
        return RejectReason::RiskLimitBreached;
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

    // P2-9: both sides reserved at admission, so the taker's fill is no
    // different from the maker's: working turns into position. Crediting the
    // taker on top of that counted it twice once a parked or repriced order,
    // which reserves before it ever aggresses, traded.
    if (positionLimitsActive_.load(std::memory_order_relaxed)) {
        settleFill(t.buyerId, Side::Buy, t.quantity);
        settleFill(t.sellerId, Side::Sell, t.quantity);
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

// A new mapping puts the trader's resting orders and fills under its tiers:
// re-book every tier from the books, or the first cancel releases what was
// never reserved. So does the first configuration of any kind.
void MatchingEngine::mapParticipant(ParticipantId trader, uint64_t strategyId,
                                    uint64_t accountId, uint64_t firmId) {
    {
        std::lock_guard<std::mutex> lock(riskMutex_);
        riskManager_.mapParticipant(trader, strategyId, accountId, firmId);
    }
    riskActive_.store(true, std::memory_order_relaxed);
    observersActive_.store(true, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(bookMutex_);
    rebuildPositionsLocked();
}

void MatchingEngine::setRiskTierLimits(RiskTier tier, uint64_t entityId, const TierLimits& limits) {
    {
        std::lock_guard<std::mutex> lock(riskMutex_);
        riskManager_.setLimits(tier, entityId, limits);
    }
    observersActive_.store(true, std::memory_order_relaxed);
    if (!riskActive_.exchange(true, std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(bookMutex_);
        rebuildPositionsLocked();
    }
}

int64_t MatchingEngine::riskNetPosition(RiskTier tier, uint64_t entityId) const {
    std::lock_guard<std::mutex> lock(riskMutex_);
    return riskManager_.netPosition(tier, entityId);
}

int64_t MatchingEngine::riskWorkingQty(RiskTier tier, uint64_t entityId, Side side) const {
    std::lock_guard<std::mutex> lock(riskMutex_);
    return riskManager_.workingQty(tier, entityId, side);
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
    // GW-9: the graduated kill switch. One relaxed load while it is at Normal.
    if (killSwitch_.engaged()) [[unlikely]] {
        if (const RejectReason r = killSwitch_.admit(symbolId, participantId);
            r != RejectReason::None) {
            (r == RejectReason::KillSwitchActive ? killSwitchRejects_ : rateLimitedCount_)
                .fetch_add(1, std::memory_order_relaxed);
            logRiskReject("graduated_kill_switch", symbolId, participantId, r);
            return rejectedAsync(r);
        }
    }

    if (!running_.load(std::memory_order_acquire)) {
        return rejectedAsync(RejectReason::EngineStopped);
    }

    // P3-6: once a graceful shutdown has begun, refuse NEW orders. Requests
    // already enqueued keep draining; only fresh submissions are turned away.
    if (shuttingDown_.load(std::memory_order_acquire)) [[unlikely]] {
        return rejectedAsync(RejectReason::EngineStopped);
    }

    // D4: the journal hit a real write error and writes nothing more. Whatever
    // is accepted now could never be made durable.
    if (journalHalted()) [[unlikely]] {
        return rejectedAsync(RejectReason::JournalHalted);
    }

    // ENGTEST-1: position and OTR state is indexed by participant id. Past the
    // arrays those controls were skipped, not enforced, so an id there traded
    // with no limits. Every ingress (gateway, admin) comes
    // through here; checked before the rate limiter so a junk id grows nothing.
    if (participantId >= MAX_PARTICIPANTS) [[unlikely]] {
        return rejectedAsync(RejectReason::InvalidFieldValue);
    }
    if (!orderEnumsDefined(side, type, tif, pegType)) [[unlikely]] {
        return rejectedAsync(RejectReason::InvalidFieldValue);
    }
    // PRIOR-7: expireOrders skips expiryTime 0, so a GTD order without one,
    // or a DAY order without one where no session end is scheduled, rested as
    // GTC while its owner believed it time-bounded.
    if (expiryTime == 0 &&
        (tif == TimeInForce::GTD ||
         (tif == TimeInForce::DAY &&
          !sessionEndScheduled_.load(std::memory_order_acquire)))) [[unlikely]] {
        return rejectedAsync(RejectReason::MissingRequiredField);
    }

    uint64_t sequenceId = nextSequenceId();

    if (rateLimiter_.isEnabled() && !rateLimiter_.allow(participantId)) {
        rateLimitedCount_.fetch_add(1, std::memory_order_relaxed);
        return rejectedAsync(RejectReason::RateLimitExceeded);
    }

    auto* book = getOrderBook(symbolId);
    if (!book) {
        return rejectedAsync(RejectReason::SymbolNotFound);
    }

    // P2-9/10/11 pre-trade risk gate (fat-finger, position, OTR). Enforced at
    // the submitOrder entry point in BOTH sync and async modes, before the order
    // is enqueued or booked. Skipped entirely (one atomic load) when unconfigured.
    if (anyRiskActive_.load(std::memory_order_relaxed)) {
        RejectReason rr = checkRiskControls(symbolId, participantId, side, price, qty, type, qty);
        if (rr != RejectReason::None) [[unlikely]] {
            return rejectedAsync(rr);
        }
    }

    if (async_) {
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
            releaseWorking(participantId, side, qty);
            return rejectedAsync(enqueueRejectReason());
        }
        return acceptedAsync(sequenceId);
    }

    if (const RejectReason rr = checkBookValuedNotional(*book, symbolId, participantId, side, type,
                                                        price, qty, stopPrice, stopLimitPrice,
                                                        trailAmount);
        rr != RejectReason::None) [[unlikely]] {
        releaseWorking(participantId, side, qty);
        return rejectedAsync(rr);
    }

    // GW-4: the hierarchical gate, which the async worker runs at apply time,
    // ran on this path not at all.
    if (riskActive_.load(std::memory_order_relaxed)) [[unlikely]] {
        // The mid under the book lock: another sync caller may be matching.
        Price mid = 0;
        if (type != OrderType::Market) book->withBookLock([&] { mid = book->getMidPrice(); });
        const Price valued =
            book->notionalPrice(side, type, price, stopPrice, stopLimitPrice, trailAmount);
        if (!preTradeRiskCheck(participantId, side, valued, qty, mid, qty)) {
            releaseWorking(participantId, side, qty);
            return rejectedAsync(RejectReason::RiskLimitBreached);
        }
    }

    // Capture everything this order emits so none of it reaches the client
    // before the journal entry behind it is durable. No-op when disabled.
    durabilityGate_.beginOrder();
    const TradingState stateBefore = book->getTradingState();
    AddOrderResult result = book->addOrder(orderId, participantId, side, price, qty,
                                           type, stopPrice, displayQty, tif, expiryTime,
                                           stopLimitPrice, pegType, pegOffset, trailAmount,
                                           minQty, hidden);
    // Refused: no order lives, nothing traded. Admitted, the book answers for
    // the reservation from here.
    const bool admitted = std::holds_alternative<OrderId>(result);
    if (!admitted) {
        releaseWorking(participantId, side, qty);
        releaseHierarchical(participantId, side, qty);
    }
    uint64_t appendOrdinal = 0;
    if (journal_ && admitted) {
        std::lock_guard<std::mutex> lock(journalMutex_);
        logBreakerTrip(symbolId, stateBefore, book->getTradingState());
        journal_->logAddOrder(orderId, participantId, symbolId, side, price, qty, type, tif,
                              expiryTime, stopPrice, stopLimitPrice, displayQty, pegType,
                              pegOffset, trailAmount, minQty, hidden);
        appendOrdinal = journal_->entriesAppended();
    }
    // Its fills reach OCO, fees, hierarchical risk and CAT as the worker's do.
    if (admitted) appendOrdinal = driveOcoSync(symbolId, book, appendOrdinal);
    holdUntilDurable(appendOrdinal);
    if (appendOrdinal) maybeTriggerAutoCheckpoint();
    // The append above may be the commit that failed. Then the order is not
    // on disk and a restart will not have it, so its client is told that —
    // not "accepted" — and the gate keeps everything it produced (D4).
    if (journalHalted()) [[unlikely]] {
        return rejectedAsync(RejectReason::JournalHalted);
    }
    return orderBookResultToSubmitResult(result, sequenceId);
}

void MatchingEngine::holdUntilDurable(uint64_t appendOrdinal) {
    if (appendOrdinal == 0) {
        // No journal entry — a reject, or journalling is off. There is nothing
        // for these events to wait on, and leaving the gate armed would spill
        // them into the next request's group and hold them indefinitely.
        durabilityGate_.abandonOrder();
        return;
    }
    // The commit may already have happened inside the append (Immediate
    // policy, or a full batch), in which case releaseThrough has already run
    // for this ordinal and the group is released at once.
    durabilityGate_.commitOrder(appendOrdinal);
    if (durabilityGate_.enabled()) {
        const uint64_t durable = durableEntries_.load(std::memory_order_acquire);
        if (durable >= appendOrdinal) {
            durabilityGate_.releaseThrough(durable);
        }
    }
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
    if (journalHalted()) [[unlikely]] {
        return rejectedAsync(RejectReason::JournalHalted);
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
            return rejectedAsync(enqueueRejectReason());
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
    // The order's working quantity goes back to the ledger inside the book
    // (OrderBook::untrackOrder), on this path and every other way out.
    // One lock acquisition either way: cancelOrderReleasing() does the same
    // work as cancelOrder() plus reading three fields, which is cheaper than
    // taking bookLock_ a second time to ask about ownership separately.
    // Every request that journals holds its events behind the entry, as
    // submitOrder does — not only new orders (D7). Same for modify, replace,
    // the expiry sweep and the kill and DAY sweeps.
    durabilityGate_.beginOrder();
    const auto exposure = book->cancelOrderReleasing(orderId, requester);
    if (exposure.denied) [[unlikely]] {
        holdUntilDurable(0);
        return rejectedAsync(RejectReason::NotOrderOwner);
    }
    if (!exposure.found) [[unlikely]] {
        holdUntilDurable(0);
        return rejectedAsync(RejectReason::OrderNotFound);
    }
    uint64_t appendOrdinal = 0;
    if (journal_) {
        std::lock_guard<std::mutex> lock(journalMutex_);
        journal_->logCancelOrder(orderId, symbolId);
        appendOrdinal = journal_->entriesAppended();
    }
    holdUntilDurable(appendOrdinal);
    if (journal_) maybeTriggerAutoCheckpoint();
    if (journalHalted()) [[unlikely]] {
        return rejectedAsync(RejectReason::JournalHalted);   // see submitOrder
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
    // ENGTEST-1, as in submitOrder: an id past the risk arrays has no controls
    // behind it. kAnyParticipant is not an id; it is the venue asking.
    if (requester >= MAX_PARTICIPANTS && requester != kAnyParticipant) [[unlikely]] {
        return rejectedAsync(RejectReason::InvalidFieldValue);
    }
    if (newQty == 0) [[unlikely]] {
        return rejectedAsync(RejectReason::InvalidQuantity);
    }
    if (journalHalted()) [[unlikely]] {
        return rejectedAsync(RejectReason::JournalHalted);
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
            return rejectedAsync(enqueueRejectReason());
        }
        return acceptedAsync(sequenceId);
    }

    auto* book = getOrderBook(symbolId);
    if (!book) {
        return rejectedAsync(RejectReason::SymbolNotFound);
    }
    RejectReason modifyReason = RejectReason::None;
    durabilityGate_.beginOrder();
    bool modified = book->modifyOrder(orderId, newQty, modifyReason, requester);
    uint64_t appendOrdinal = 0;
    if (modified && journal_) {
        std::lock_guard<std::mutex> lock(journalMutex_);
        journal_->logModifyOrder(orderId, symbolId, newQty);
        appendOrdinal = journal_->entriesAppended();
    }
    holdUntilDurable(appendOrdinal);
    if (appendOrdinal) maybeTriggerAutoCheckpoint();
    if (appendOrdinal && journalHalted()) [[unlikely]] {
        return rejectedAsync(RejectReason::JournalHalted);   // see submitOrder
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
    // ENGTEST-1, as in submitOrder: an id past the risk arrays has no controls
    // behind it. kAnyParticipant is not an id; it is the venue asking.
    if (requester >= MAX_PARTICIPANTS && requester != kAnyParticipant) [[unlikely]] {
        return rejectedAsync(RejectReason::InvalidFieldValue);
    }
    if (journalHalted()) [[unlikely]] {
        return rejectedAsync(RejectReason::JournalHalted);
    }
    // GW-9 and P2-8: a replace is an admission. Refused here, on both paths,
    // from the requester's id; admitAmend re-checks against the order's owner.
    if (killSwitchActive_.load(std::memory_order_acquire) ||
        (killSwitch_.engaged() && killSwitch_.blocks(symbolId, requester))) [[unlikely]] {
        killSwitchRejects_.fetch_add(1, std::memory_order_relaxed);
        return rejectedAsync(RejectReason::KillSwitchActive);
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
            return rejectedAsync(enqueueRejectReason());
        }
        return acceptedAsync(sequenceId);
    }

    auto* book = getOrderBook(symbolId);
    if (!book) {
        return rejectedAsync(RejectReason::SymbolNotFound);
    }

    RejectReason replaceReason = RejectReason::None;
    durabilityGate_.beginOrder();
    bool replaced = book->cancelReplace(orderId, newPrice, newQty, replaceReason,
                                        requester);
    uint64_t appendOrdinal = 0;
    if (replaced && journal_) {
        std::lock_guard<std::mutex> lock(journalMutex_);
        journal_->logCancelReplace(orderId, symbolId, newPrice, newQty);
        appendOrdinal = journal_->entriesAppended();
    }
    if (replaced) appendOrdinal = driveOcoSync(symbolId, book, appendOrdinal);  // GTS-4
    holdUntilDurable(appendOrdinal);
    if (appendOrdinal) maybeTriggerAutoCheckpoint();
    if (appendOrdinal && journalHalted()) [[unlikely]] {
        return rejectedAsync(RejectReason::JournalHalted);   // see submitOrder
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

    // The engine-wide sweep, scoped to one participant, so each cancel is
    // journaled and its exposure released (D6). cancelAllForParticipant did
    // neither: a restart replayed the killed orders back onto the book.
    return cancelAllRestingOrders(participantId);
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

// H3: a breaker trip happens inside addOrder. It goes into the journal AHEAD
// of the order that caused it, so replay enters the auction first and the
// order replays as an auction arrival; replay never evaluates the breaker
// itself, so a replaying engine with another threshold or reference — the
// reference is not in a checkpoint — still lands in the same state. Caller
// holds journalMutex_.
void MatchingEngine::logBreakerTrip(SymbolId symbolId, TradingState before,
                                    TradingState after) {
    if (after != before) journal_->logTradingState(symbolId, after);
}

// Session control. Each transition and uncross is applied by the book's
// owning thread and journaled there, so the record lands in the journal in
// order with that book's orders and replay re-runs the auction exactly
// (roadmap 1.8-H1, AUCT-1). These used to mutate worker-owned books from the
// caller's thread (THR-1) and journal nothing: replay ran every auction order
// as a continuous one.
//
// In async mode the requests ride the workers' own queues and the caller
// waits for the drain. bookMutex_ only covers the enqueue: a checkpoint takes
// it and then waits for the workers (its cut). A batch is therefore applied per book in
// that book's order flow, not as one instant across books.
uint64_t MatchingEngine::applySessionControl(const OrderRequest& req) {
    auto* book = getOrderBook(req.symbolId);
    if (!book) return 0;
    const auto state = static_cast<TradingState>(req.newQty);
    const auto tradingDate = static_cast<uint32_t>(req.expiryTime);
    if (req.type == OrderRequest::Type::TradingState) {
        if (state == TradingState::PreOpen) book->beginSession(tradingDate);
        else book->setTradingState(state);
        if (book->getTradingState() != state) return 0;  // refused: nothing happened
    } else {
        book->uncross();
    }
    uint64_t appendOrdinal = 0;
    if (journal_) {
        std::lock_guard<std::mutex> lock(journalMutex_);
        if (req.type == OrderRequest::Type::TradingState) {
            journal_->logTradingState(req.symbolId, state, tradingDate);
        } else {
            journal_->logUncross(req.symbolId);
        }
        appendOrdinal = journal_->entriesAppended();
    }
    // An uncross fills OCO legs like any match does.
    if (ocoActive_.load(std::memory_order_relaxed) ||
        observersActive_.load(std::memory_order_relaxed)) {
        driveOco(req.symbolId, book);
    }
    return appendOrdinal;
}

void MatchingEngine::routeSessionControl(SymbolId symbolId, OrderRequest::Type type,
                                         TradingState state, uint32_t tradingDate) {
    OrderRequest req{};
    req.type = type;
    req.symbolId = symbolId;
    req.newQty = static_cast<Quantity>(state);
    req.expiryTime = tradingDate;
    if (async_) {
        enqueueControl(getThreadIndex(symbolId), req);
        return;
    }
    // Sync: hold the cross's fills until the record behind them is durable,
    // as every other sync path does.
    durabilityGate_.beginOrder();
    holdUntilDurable(applySessionControl(req));
}

void MatchingEngine::uncross(SymbolId symbolId) {
    (void)uncrossBatch({symbolId});
}

size_t MatchingEngine::setTradingStateBatch(
        const std::vector<SymbolId>& symbols, TradingState state) {
    return transitionBatch(symbols, state, /*tradingDate=*/0);
}

size_t MatchingEngine::beginTradingSession(const std::vector<SymbolId>& symbols,
                                           uint32_t tradingDate) {
    return transitionBatch(symbols, TradingState::PreOpen, tradingDate);
}

size_t MatchingEngine::transitionBatch(const std::vector<SymbolId>& symbols,
                                       TradingState state, uint32_t tradingDate) {
    {
        std::lock_guard<std::mutex> lock(bookMutex_);
        for (SymbolId s : symbols) {
            if (!getOrderBook(s)) continue;
            routeSessionControl(s, OrderRequest::Type::TradingState, state, tradingDate);
        }
    }
    waitForDrain();
    // Counted after the drain: a book may have refused (decision 7).
    size_t inState = 0;
    for (SymbolId s : symbols) {
        const OrderBook* book = getOrderBook(s);
        if (book && book->getTradingState() == state) ++inState;
    }
    return inState;
}

size_t MatchingEngine::uncrossBatch(const std::vector<SymbolId>& symbols) {
    size_t crossed = 0;
    {
        std::lock_guard<std::mutex> lock(bookMutex_);
        for (SymbolId s : symbols) {
            if (!getOrderBook(s)) continue;
            routeSessionControl(s, OrderRequest::Type::Uncross);
            ++crossed;
        }
    }
    waitForDrain();
    return crossed;
}

size_t MatchingEngine::resumeVolatilityAuctions() {
    // Only session control changes a trading state, and it is serialized by
    // bookMutex_, so the state read here is the one the worker will see.
    // Leaving VolatilityAuction for Continuous is the reopening cross.
    size_t resumed = 0;
    {
        std::lock_guard<std::mutex> lock(bookMutex_);
        for (SymbolId s : symbolIds_) {
            auto* book = getOrderBook(s);
            if (!book || book->getTradingState() != TradingState::VolatilityAuction) continue;
            routeSessionControl(s, OrderRequest::Type::TradingState, TradingState::Continuous);
            ++resumed;
        }
    }
    waitForDrain();
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
    durabilityGate_.beginOrder();
    uint64_t appendOrdinal = 0;
    for (SymbolId symbolId : symbolIds_) {
        std::function<void(OrderId)> onExpire;
        if (journal_) {
            onExpire = [this, symbolId, &appendOrdinal](OrderId id) {
                std::lock_guard<std::mutex> lock(journalMutex_);
                journal_->logCancelOrder(id, symbolId);
                appendOrdinal = journal_->entriesAppended();
            };
        }
        if (auto* book = getOrderBook(symbolId)) {
            book->expireOrders(currentTime, onExpire);
        }
    }
    holdUntilDurable(appendOrdinal);
    // H3: reopen every volatility auction that has run its dwell — the
    // reopening cross and a journaled TradingStateChange, like any reopen.
    for (SymbolId symbolId : symbolIds_) {
        auto* book = getOrderBook(symbolId);
        if (book && book->volatilityAuctionDue(currentTime))
            routeSessionControl(symbolId, OrderRequest::Type::TradingState,
                                TradingState::Continuous);
    }
}

void MatchingEngine::commitJournal() {
    std::lock_guard<std::mutex> lock(journalMutex_);
    if (journal_) journal_->commitPending();
}

void MatchingEngine::commitJournalIfDue() {
    std::lock_guard<std::mutex> lock(journalMutex_);
    if (journal_) journal_->commitIfDue();
}

bool MatchingEngine::enableJournal(const std::string& path, Journal::SyncPolicy policy) {
    std::lock_guard<std::mutex> lock(journalMutex_);
    journal_ = std::make_unique<Journal>(path, policy, 64);
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

    std::vector<JournalEntry> entries;
    {
        std::lock_guard<std::mutex> lock(journalMutex_);
        entries = journal_->readAll(true, true);
    }
    return replayEntries(entries);
}

size_t MatchingEngine::replayEntries(const std::vector<JournalEntry>& entries) {
    for (SymbolId symbolId : symbolIds_) {
        if (auto* book = getOrderBook(symbolId)) {
            book->setReplayMode(true);
        }
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
                book->cancelReplace(entry.orderId, entry.newPrice,
                                    journaledReplaceTotal(
                                        entry, book->getOrder(entry.orderId)->cumQty));
            }
            break;
        }
        case JournalEntry::Type::TradingStateChange:
        case JournalEntry::Type::Uncross: {
            auto* book = getOrderBook(entry.symbolId);
            if (!book) {
                addSymbol(entry.symbolId);
                book = getOrderBook(entry.symbolId);
                if (book) {
                    book->setReplayMode(true);
                }
            }
            if (book) applySessionRecord(*book, entry);
            break;
        }
        case JournalEntry::Type::Counters: {
            auto* book = entry.orderId != 0 ? getOrderBook(entry.symbolId) : nullptr;
            if (!book && entry.orderId != 0) {
                addSymbol(entry.symbolId);
                book = getOrderBook(entry.symbolId);
                if (book) {
                    book->setReplayMode(true);
                }
            }
            restoreCounters(book, entry);
            break;
        }
        case JournalEntry::Type::Position: {
            auto* book = getOrderBook(entry.symbolId);
            if (!book) {
                addSymbol(entry.symbolId);
                book = getOrderBook(entry.symbolId);
                if (book) {
                    book->setReplayMode(true);
                }
            }
            if (book) applyPositionRecord(*book, entry);
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
            if (book && (entry.newQty & SNAPSHOT_PEG_QUEUE)) {
                book->restorePegQueue(entry.orderId);
            } else if (book && !book->getOrder(entry.orderId)) {
                // Put back, not re-admitted: see OrderBook::restoreOrder.
                const RejectReason why =
                    book->restoreOrder(entry.orderId, entry.participantId, entry.side,
                                       entry.price, entry.quantity, entry.orderType,
                                       entry.stopPrice, entry.displayQty, entry.timeInForce,
                                       entry.expiryTime, entry.stopLimitPrice, entry.pegType,
                                       entry.pegOffset, entry.trailAmount, entry.minQty,
                                       entry.hidden);
                if (why != RejectReason::None) {
                    ++restoreRefusals_;
                    obSink().log(obEvent("checkpoint_restore_refused", LogSeverity::Error)
                                     .kv("symbol", (long long)entry.symbolId)
                                     .kv("order", (unsigned long long)entry.orderId)
                                     .kv("price", (long long)entry.price)
                                     .kv("reason", (long long)static_cast<int>(why)));
                    break;
                }
                if (entry.newQty & SNAPSHOT_STOP_ELECTED)
                    book->restoreStopElection(entry.orderId, entry.stopPrice);
                else
                    book->restoreTrailingStop(entry.orderId, entry.stopPrice);
                book->restoreExecutedQty(entry.orderId, snapshotCumQty(entry));
                book->restoreIcebergSlice(entry.orderId, snapshotIcebergSlice(entry));
            }
            break;
        }
        }

        ++replayed;
    }

    setReplayModeAllBooks(false);
    return replayed;
}

void MatchingEngine::setReplayModeAllBooks(bool replay) {
    // Iterate symbolIds_ under bookMutex_, like every other off-hot-path reader.
    std::lock_guard<std::mutex> lock(bookMutex_);
    for (SymbolId symbolId : symbolIds_) {
        if (auto* book = getOrderBook(symbolId)) {
            book->setReplayMode(replay);
        }
    }
    if (!replay) rebuildPositionsLocked();
}

void MatchingEngine::rebuildPositionsLocked() {
    // The P2-9 ledger: working = remaining qty of every live order, parked ones
    // included, each counted once; filled = each book's per-participant net
    // position, which every fill moves, replay included, and a checkpoint's
    // Position record restores. Unconditional, so a limit set after the restart
    // also sees what is held.
    // ponytail: not atomic against a concurrent admission — an order a worker
    // admits between the zeroing and the walk of its book counts twice.
    // replayJournal runs before the workers start, so nothing admits during
    // this walk there. Post it to the workers as a control message if a caller
    // ever leaves replay mode on a running engine.
    for (auto& f : filled_) f.store(0, std::memory_order_relaxed);
    for (auto& sides : working_)
        for (auto& w : sides) w.store(0, std::memory_order_relaxed);
    for (SymbolId symbolId : symbolIds_) {
        const OrderBook* book = getOrderBook(symbolId);
        if (!book) continue;
        book->forEachPositionLocked([this](const OrderBook::ParticipantPosition& p) {
            if (p.participant < MAX_PARTICIPANTS)
                filled_[p.participant].fetch_add(p.netPosition, std::memory_order_relaxed);
        });
        book->forEachOrderLocked([this](const Order& o) {
            if (o.participantId < MAX_PARTICIPANTS)
                working_[o.participantId][sideIndex(o.side)].fetch_add(
                    static_cast<int64_t>(o.remainingQty), std::memory_order_relaxed);
        });
    }
    // The hierarchy the same way (TLA-5): each tier's net position and working
    // quantity, from the same books. riskMutex_ per call, inside each book's
    // lock, the order every other path takes them in.
    {
        std::lock_guard<std::mutex> lock(riskMutex_);
        riskManager_.clearPositions();
    }
    for (SymbolId symbolId : symbolIds_) {
        const OrderBook* book = getOrderBook(symbolId);
        if (!book) continue;
        book->forEachPositionLocked([this](const OrderBook::ParticipantPosition& p) {
            std::lock_guard<std::mutex> lock(riskMutex_);
            const bool longPos = p.netPosition >= 0;
            riskManager_.onFill(p.participant, longPos ? Side::Buy : Side::Sell, 0,
                                static_cast<Quantity>(longPos ? p.netPosition : -p.netPosition));
        });
        book->forEachOrderLocked([this](const Order& o) {
            std::lock_guard<std::mutex> lock(riskMutex_);
            riskManager_.reserve(o.participantId, o.side, o.remainingQty);
        });
    }
}

void MatchingEngine::checkpointInternal(bool alreadyDrained, bool waitIfBusy) {
    if (!journal_) {
        return;
    }
    // D4: the book holds state the disk refused — the request whose commit
    // failed was applied before it was journaled. A snapshot would make that
    // durable after its client was told JournalHalted. The journal refuses
    // the rewrite too; this says so in the log.
    if (journal_->halted()) {
        obSink().log(obEvent("checkpoint_refused_journal_halted", LogSeverity::Error));
        return;
    }

    if (async_ && !alreadyDrained) {
        waitForDrain();
    }

    // One checkpoint at a time (THR-6). The callers run on different threads —
    // main's size-based rotation, the checkpoint thread, gracefulShutdown —
    // and every one of them builds the same
    // <journal>.tmp outside journalMutex_. Overlapped, the second remove()d the
    // first's finished replacement and started its own, the first then renamed
    // the second's half-written file over the live journal, and the second kept
    // writing into it through a non-append handle.
    //
    // Busy means another checkpoint is doing this job, so the periodic callers
    // skip. gracefulShutdown waits instead: its snapshot is the one taken after
    // the drain and any DAY cancels, and when it returns the report it prints
    // says the book is on disk — which must be its checkpoint, finished, and
    // not whatever an overlapping one gathered before those changes.
    std::unique_lock<std::mutex> exclusive(checkpointMutex_, std::try_to_lock);
    if (!exclusive.owns_lock()) {
        if (!waitIfBusy) {
            obSink().log(obEvent("checkpoint_skipped_busy"));
            return;
        }
        obSink().log(obEvent("checkpoint_waiting"));
        exclusive.lock();
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

    // THR-13: on the async path the gather below is a CONSISTENT CUT. The
    // workers stop between two requests (each journals what it changed before
    // it returns), and bookMutex_ shuts out the off-worker sweeps, which hold
    // it across their change and its record. So the copy is exactly the
    // journal's first appendsBefore entries, and whatever is appended while
    // the replacement is built is carried onto it (commitRewrite's tail)
    // instead of forcing a retry. The workers wait only for the copy: the
    // write and sync of the replacement happen after they resume. The sync
    // path has no cut — a caller on another thread may have changed a book
    // without having journaled it yet — so there an append still means retry.
    const bool cut = async_;
    struct CutGuard {
        MatchingEngine& engine;
        bool on;
        ~CutGuard() { if (on) engine.resumeWorkersAfterCut(); }
    };

    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        uint64_t appendsBefore = 0;

        // Phase 1: a point-in-time copy of every resting order, under each
        // book's own lock and never holding journalMutex_. The lock order is
        // bookMutex_ -> bookLock_ -> journalMutex_ because the expiry path
        // takes it that way; inverting it here would deadlock a concurrent
        // ExpireCheck. Order copies are read-only value snapshots — the
        // intrusive next/prev are never dereferenced.
        std::vector<std::pair<SymbolId, Order>> resting;
        // A book's trading state goes in before its orders: replay restores
        // them under it, and an accumulated auction book restored into a
        // Continuous one would trade on arrival (AUCT-1). Continuous is the
        // default, so it needs no record. A begun session goes first, as a
        // PreOpen with its trading date, so a restored book still refuses a
        // second session that date, or an undated one (decision 7); a halted
        // book's pre-halt state goes just before its Halted, so the halt still
        // resumes only to it.
        struct SessionState {
            SymbolId symbolId; TradingState state; bool begun; uint32_t date;
            TradingState preHalt;
        };
        std::vector<SessionState> states;
        // D1: the snapshot drops the trades and the lease records, so it
        // carries the counters they would have restored, and the prices.
        struct BookCounters {
            SymbolId symbolId; uint64_t nextTradeId; OrderBook::CheckpointPrices prices;
        };
        std::vector<BookCounters> counters;
        std::vector<std::pair<SymbolId, OrderBook::ParticipantPosition>> positions;
        std::vector<std::pair<SymbolId, OrderId>> pegQueue;
        uint64_t lease = 0;
        {
            std::lock_guard<std::mutex> booksLock(bookMutex_);
            if (cut) pauseWorkersForCut();
            const CutGuard resume{*this, cut};
            {
                std::lock_guard<std::mutex> lock(journalMutex_);
                if (!journal_) {
                    return;
                }
                appendsBefore = journal_->entriesAppended();
                lease = sequenceLease_.load(std::memory_order_acquire);
            }
            for (SymbolId symbolId : symbolIds_) {
                const OrderBook* book = getOrderBook(symbolId);
                if (!book) {
                    continue;
                }
                const uint64_t next = book->nextTradeIdLocked();
                const OrderBook::CheckpointPrices prices = book->checkpointPricesLocked();
                if (next > 1 || prices.lastTrade != 0 || prices.reference != 0)
                    counters.push_back({symbolId, next, prices});   // books that traded or priced
                if (book->getTradingState() != TradingState::Continuous || book->sessionBegun())
                    states.push_back({symbolId, book->getTradingState(), book->sessionBegun(),
                                      book->sessionDate(), book->preHaltState()});
                book->forEachOrderLocked([&](const Order& order) {
                    resting.emplace_back(symbolId, order);
                });
                book->forEachPeggedOrderLocked([&](const Order& order) {
                    pegQueue.emplace_back(symbolId, order.id);
                });
                book->forEachPositionLocked([&](const OrderBook::ParticipantPosition& p) {
                    positions.emplace_back(symbolId, p);
                });
            }
        }

        const auto writeSnapshot = [&](Journal& snapshotJournal) {
            for (const auto& [symbolId, state, begun, date, preHalt] : states) {
                // Replay applies these unjudged, on a book with no orders yet.
                TradingState replayed = TradingState::Continuous;
                if (begun) {
                    snapshotJournal.logTradingState(symbolId, TradingState::PreOpen, date);
                    replayed = TradingState::PreOpen;
                }
                if (state == TradingState::Halted && preHalt != replayed) {
                    snapshotJournal.logTradingState(symbolId, preHalt);
                    replayed = preHalt;
                }
                if (state != replayed) snapshotJournal.logTradingState(symbolId, state);
            }
            if (lease != 0) snapshotJournal.logCounters(0, 0, lease);
            for (const auto& [symbolId, order] : resting) {
                snapshotJournal.logSnapshot(order.id, order.participantId, symbolId,
                                            order.side, order.price, order.remainingQty,
                                            order.type, order.timeInForce, order.expiryTime,
                                            order.stopPrice, order.stopLimitPrice,
                                            order.displayQty, order.pegType,
                                            order.pegOffset, order.trailAmount,
                                            order.minQty, order.isHidden,
                                            order.isStopTriggered, order.cumQty,
                                            order.type == OrderType::Iceberg
                                                ? order.visibleQty : 0);
            }
            // After the orders, which put the pegs back in level order: the
            // order each book's reprice pass moves them in.
            for (const auto& [symbolId, id] : pegQueue)
                snapshotJournal.logPegQueue(id, symbolId);
            // After the orders: each restored order's add sweeps stops at the
            // last trade, which would fire them into a half-restored book.
            for (const auto& [symbolId, nextTradeId, prices] : counters)
                snapshotJournal.logCounters(symbolId, nextTradeId, 0,
                                            prices.lastTrade, prices.reference,
                                            prices.lastTradeQty);
            // After the orders too: their re-adds count as submissions again.
            for (const auto& [symbolId, p] : positions)
                snapshotJournal.logPosition(symbolId, p.participant, p.netPosition,
                                            p.ordersSubmitted, p.tradesExecuted);
        };

        // Phase 2: build the replacement OUTSIDE journalMutex_. Writing every
        // resting order plus a durability barrier is the expensive part, and
        // it touches nothing the live journal owns.
        if (!journal_->prepareRewrite(writeSnapshot)) {
            obSink().log(obEvent("checkpoint_prepare_failed", LogSeverity::Error)
                             .kv("orders", (unsigned long long)resting.size()));
            return;
        }
        // The replacement is on disk as <journal>.tmp and about to be swapped
        // in. CheckpointConcurrencyTest parks a checkpoint here to force an
        // overlap deterministically.
        obSink().log(obEvent("checkpoint_prepared")
                         .kv("attempt", (long long)attempt)
                         .kv("orders", (unsigned long long)resting.size()));

        // Phase 3: swap. With a cut, carry over what was appended since it;
        // without one, only if nothing was.
        {
            std::lock_guard<std::mutex> lock(journalMutex_);
            if (!journal_) {
                return;
            }
            const uint64_t tail = journal_->entriesAppended() - appendsBefore;
            if (cut || tail == 0) {
                if (!journal_->commitRewrite(tail)) {
                    // Previously discarded. A failed rename leaves the old
                    // journal in place, which is safe — but it means the
                    // checkpoint silently did not happen, and the journal goes
                    // on growing with nobody aware of it.
                    obSink().log(obEvent("checkpoint_commit_failed", LogSeverity::Error)
                                     .kv("orders", (unsigned long long)resting.size()));
                } else {
                    obSink().log(obEvent("checkpoint_committed")
                                     .kv("orders", (unsigned long long)resting.size())
                                     .kv("tail", (unsigned long long)tail));
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
