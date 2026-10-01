#include "SessionScheduler.h"

#include <algorithm>
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
      schedule_(schedule),
      wallAnchorMs_(static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch()).count())),
      steadyAnchor_(std::chrono::steady_clock::now()) {}

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

// The wall clock is read once, at construction, and then advanced by
// steady_clock, which never steps. Reading system_clock on every tick let an
// NTP step move the session clock backwards (AUCT-7). UTC rather than local
// time keeps this lock- and dependency-free; a venue outside UTC injects a
// clock (and a date function) that applies its offset.
uint64_t SessionScheduler::defaultEpochMs() const {
    const auto elapsed = std::chrono::steady_clock::now() - steadyAnchor_;
    return wallAnchorMs_ + static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

uint64_t SessionScheduler::now() const {
    if (clock_) {
        return clock_();
    }
    return defaultEpochMs() % kMsPerDay;
}

uint32_t SessionScheduler::tradingDate() const {
    if (dateFn_) return dateFn_();
    if (clock_) return 0;  // injected ms-of-day clock, no date: one long day
    return static_cast<uint32_t>(defaultEpochMs() / kMsPerDay);
}

void SessionScheduler::resetSession() {
    std::lock_guard<std::mutex> lock(tickMutex_);
    phase_.store(SessionPhase::Idle, std::memory_order_release);
    lastTick_ = 0;
}

void SessionScheduler::applyPhase(SessionPhase target) {
    // Each phase issues the same engine calls a venue operator would at that
    // boundary. Leaving an accumulation state for Continuous or PostClose runs
    // that book's uncross under the same lock as the flip (OrderBook::
    // setTradingState), so the opening and closing crosses are the flips
    // themselves. They used to be a separate uncrossBatch first, and orders
    // that arrived between the two calls were carried into continuous trading
    // still crossed (AUCT-4).
    switch (target) {
        case SessionPhase::PreOpen:
            // Pre-session accumulation. Orders rest and seed the opening
            // auction; no continuous matching yet.
            engine_.setTradingStateBatch(symbols_, TradingState::PreOpen);
            break;
        case SessionPhase::Continuous:
            // Opening auction: cross the accumulated book into a single set
            // of opening prints and switch to continuous matching.
            engine_.setTradingStateBatch(symbols_, TradingState::Continuous);
            break;
        case SessionPhase::CloseAuction:
            // Stop continuous matching and accumulate for the closing cross.
            engine_.setTradingStateBatch(symbols_, TradingState::AuctionClose);
            break;
        case SessionPhase::PostClose:
            // Closing auction: cross into the closing prints and shut the
            // market for the day (new orders rejected; cancels still allowed).
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

    // A new trading date starts a new session, and only that does. This used
    // to treat any backward clock reading as midnight, so a 1 ms NTP step in
    // the closing auction replayed the whole day in one tick — including an
    // opening uncross of the closing-auction book (AUCT-7).
    const uint64_t date = tradingDate();
    if (date != sessionDate_) {
        if (sessionDate_ != kNoDate) {
            phase_.store(SessionPhase::Idle, std::memory_order_release);
        }
        sessionDate_ = date;
        lastTick_ = 0;
    }
    // Within a date the session clock never runs backwards.
    const uint64_t now = std::max(this->now(), lastTick_);
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
