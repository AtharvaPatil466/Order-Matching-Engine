#include "SessionScheduler.h"

#include <chrono>

namespace OrderMatcher {

namespace {
// Milliseconds in a calendar day — the modulus for ms-of-day.
constexpr uint64_t kMsPerDay = 24ULL * 60ULL * 60ULL * 1000ULL;
} // namespace

SessionScheduler::SessionScheduler(MatchingEngine& engine,
                                   std::vector<SymbolId> symbols,
                                   SessionSchedule schedule)
    : engine_(engine),
      symbols_(std::move(symbols)),
      schedule_(schedule) {}

SessionScheduler::~SessionScheduler() {
    stop();
}

void SessionScheduler::start(uint64_t intervalMs) {
    if (running_.load(std::memory_order_acquire)) {
        return;
    }

    // intervalMs of 0 would spin the timer thread hot; clamp to 1ms.
    intervalMs_ = intervalMs > 0 ? intervalMs : 1;
    running_.store(true, std::memory_order_release);

    // Mirror MatchingEngine::startExpiryTimer: sleep-then-check-then-work, so
    // a stop() that lands during the sleep is observed before the next tick
    // and the thread exits promptly without one final spurious transition.
    timerThread_ = std::thread([this]() {
        while (running_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs_));
            if (!running_.load(std::memory_order_acquire)) {
                break;
            }
            tick();
        }
    });
}

void SessionScheduler::stop() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    running_.store(false, std::memory_order_release);
    if (timerThread_.joinable()) {
        timerThread_.join();
    }
}

uint64_t SessionScheduler::now() const {
    if (clock_) {
        return clock_();
    }
    // Real-clock fallback: derive ms-of-day from the wall clock. We use UTC
    // (system_clock's epoch) rather than local time so the computation is
    // lock-free and dependency-free; a deployment in a non-UTC venue should
    // inject a clock that applies the local offset. ms-of-day keeps the
    // schedule date-agnostic and matches the unit tests inject.
    const auto sinceEpoch = std::chrono::system_clock::now().time_since_epoch();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(sinceEpoch).count();
    return static_cast<uint64_t>(ms) % kMsPerDay;
}

void SessionScheduler::resetSession() {
    std::lock_guard<std::mutex> lock(tickMutex_);
    phase_.store(SessionPhase::Idle, std::memory_order_release);
    lastTick_ = UINT64_MAX;
}

void SessionScheduler::applyPhase(SessionPhase target) {
    // Each phase issues the same engine calls a venue operator would at that
    // boundary. The opening/closing crosses run BEFORE flipping the trading
    // state so the auction prints against the accumulated book, then the new
    // admission regime takes effect. Ordering matters: uncross first, then
    // setTradingStateBatch.
    switch (target) {
        case SessionPhase::PreOpen:
            // Pre-session accumulation. Orders rest and seed the opening
            // auction; no continuous matching yet.
            engine_.setTradingStateBatch(symbols_, TradingState::PreOpen);
            break;
        case SessionPhase::Continuous:
            // Opening auction: cross the accumulated book into a single set
            // of opening prints, then switch to continuous matching.
            engine_.uncrossBatch(symbols_);
            engine_.setTradingStateBatch(symbols_, TradingState::Continuous);
            break;
        case SessionPhase::CloseAuction:
            // Stop continuous matching and accumulate for the closing cross.
            engine_.setTradingStateBatch(symbols_, TradingState::AuctionClose);
            break;
        case SessionPhase::PostClose:
            // Closing auction: cross into the closing prints, then shut the
            // market for the day (new orders rejected; cancels still allowed).
            engine_.uncrossBatch(symbols_);
            engine_.setTradingStateBatch(symbols_, TradingState::PostClose);
            break;
        case SessionPhase::Idle:
            // Idle is a sentinel, never a transition target. No-op.
            return;
    }
    phase_.store(target, std::memory_order_release);
}

void SessionScheduler::tick() {
    std::lock_guard<std::mutex> lock(tickMutex_);

    const uint64_t now = this->now();

    // Detect a midnight wrap: the clock running backwards means a new
    // calendar day, so the session restarts and the full sequence can fire
    // again. The UINT64_MAX sentinel makes the first tick never look like a
    // wrap.
    if (lastTick_ != UINT64_MAX && now < lastTick_) {
        phase_.store(SessionPhase::Idle, std::memory_order_release);
    }
    lastTick_ = now;

    // Advance forward through the phase sequence, firing every boundary that
    // has been reached but not yet applied. The loop (rather than a single
    // step) means a coarse tick interval — or a host that calls tick()
    // infrequently — that jumps past multiple boundaries still fires each
    // one exactly once and in order, instead of skipping the intervening
    // phases. We never move backward, so each transition is one-shot per
    // session.
    bool advanced = true;
    while (advanced) {
        advanced = false;
        const SessionPhase current = phase_.load(std::memory_order_acquire);

        // Compute the next phase whose scheduled time has arrived. Each arm
        // requires the prior phase to have been applied, enforcing strict
        // forward order even if the schedule times were configured
        // out of sequence.
        if (current == SessionPhase::Idle && now >= schedule_.preOpenMs) {
            applyPhase(SessionPhase::PreOpen);
            advanced = true;
        } else if (current == SessionPhase::PreOpen && now >= schedule_.openMs) {
            applyPhase(SessionPhase::Continuous);
            advanced = true;
        } else if (current == SessionPhase::Continuous && now >= schedule_.closeAuctionMs) {
            applyPhase(SessionPhase::CloseAuction);
            advanced = true;
        } else if (current == SessionPhase::CloseAuction && now >= schedule_.closeMs) {
            applyPhase(SessionPhase::PostClose);
            advanced = true;
        }
    }
}

} // namespace OrderMatcher
