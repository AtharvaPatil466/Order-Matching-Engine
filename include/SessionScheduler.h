#pragma once

#include "MatchingEngine.h"
#include "Types.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace OrderMatcher {

// SessionScheduler drives a symbol universe through the daily trading
// lifecycle on a wall-clock timetable. Real venues move whole groups of
// symbols together: pre-open accumulation, an opening auction cross,
// continuous trading, a closing auction cross, then post-close. Rather than
// have an operator manually fire each transition, this background timer
// calls the engine's existing batched public API at the scheduled
// ms-of-day boundaries.
//
// The scheduler owns no matching logic of its own — it is purely a clock
// that sequences MatchingEngine::setTradingStateBatch (whose flip out of an
// auction state is that auction's uncross). All
// matching, locking, and state correctness lives in the engine; keeping the
// scheduler a thin sequencer means the timetable can be tested in isolation
// and a missed/duplicate tick can never corrupt book state, only mis-time a
// transition.

// Phase times are expressed in milliseconds-since-midnight (ms-of-day), the
// same unit the injectable clock returns. This keeps the schedule wall-clock
// relative (a venue opens at 09:30 local regardless of date) and lets a test
// step a synthetic clock across a full day without dealing with epoch math.
struct SessionSchedule {
    // US-equity regular-session defaults.
    uint64_t preOpenMs      = 32400000;  // 09:00:00 — start accumulating
    uint64_t openMs         = 34200000;  // 09:30:00 — opening cross, then continuous
    uint64_t closeAuctionMs = 57300000;  // 15:55:00 — enter closing auction
    uint64_t closeMs        = 57600000;  // 16:00:00 — closing cross, then post-close
    uint64_t postCloseMs    = 57600000;  // 16:00:00 — post-close (== closeMs by default)
};

// Parses "HH:MM,HH:MM,HH:MM,HH:MM" — pre-open, open, closing auction, close —
// in the scheduler's clock (UTC ms-of-day by default). Strictly increasing;
// anything else is nullopt. OrderEngine's --session-schedule.
std::optional<SessionSchedule> parseSessionSchedule(const std::string& spec);

// The phase the scheduler has most recently driven the universe into.
// Ordering is significant: the scheduler only ever advances forward through
// this enum within a session, which is what guarantees each transition fires
// exactly once. Idle is the pre-session sentinel (nothing applied yet).
enum class SessionPhase : uint8_t {
    Idle = 0,      // No phase applied yet this session
    PreOpen,       // Accumulating orders for the opening auction
    Continuous,    // Opening cross done; continuous matching live
    CloseAuction,  // Accumulating orders for the closing auction
    PostClose      // Closing cross done; market shut for the day
};

class SessionScheduler {
public:
    // Injectable clock returning ms-of-day. Mirrors MatchingEngine::ClockFn
    // so tests can drive a full session deterministically; defaults to a
    // real system_clock-derived ms-of-day when left unset.
    using ClockFn = std::function<uint64_t()>;

    // The scheduler borrows the engine (does not own it). The engine must
    // outlive the scheduler. `symbols` is the universe transitioned together
    // on each phase change.
    SessionScheduler(MatchingEngine& engine,
                     std::vector<SymbolId> symbols,
                     SessionSchedule schedule = SessionSchedule{});
    ~SessionScheduler();

    // Non-copyable, non-movable: owns a thread and is captured by `this` in
    // the timer loop.
    SessionScheduler(const SessionScheduler&) = delete;
    SessionScheduler& operator=(const SessionScheduler&) = delete;
    SessionScheduler(SessionScheduler&&) = delete;
    SessionScheduler& operator=(SessionScheduler&&) = delete;

    // Background timer lifecycle — mirrors MatchingEngine's expiry timer.
    // start() spawns a thread that wakes every intervalMs and calls tick().
    // stop() is idempotent and joins the thread. Both are safe to call from
    // any thread other than the timer thread itself.
    void start(uint64_t intervalMs = 1000);
    void stop();
    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    // Evaluate the schedule against the current clock and fire any phase
    // boundaries that have been reached but not yet applied this session.
    // Public so a host can drive transitions on its own cadence (or a test
    // can step a manual clock) without spawning the timer thread.
    void tick();

    // Begin a fresh session: forget which phases have been applied so the
    // full sequence can fire again. Called automatically when the trading
    // date changes, and available explicitly for a host that restarts a
    // session out of band.
    void resetSession();

    // Clock seam — mirrors setExpiryClock/clearExpiryClock on the engine.
    // The default is monotonic: the wall clock is read once, then advanced by
    // steady_clock, so an NTP step never moves it (AUCT-7). An injected
    // ms-of-day clock may step backwards; tick() ignores that within a date.
    void setClock(ClockFn clock) { clock_ = std::move(clock); }
    void clearClock() { clock_ = {}; }
    uint64_t now() const;

    // Trading-date seam. A new trading day is a change of this value — never
    // a smaller ms-of-day, which is what an NTP step back looks like too.
    // Any increasing day number works (e.g. yyyymmdd). Default: the UTC day of
    // the default clock. With an injected clock and no date function the date
    // never changes, so only resetSession() starts a new session.
    using DateFn = std::function<uint32_t()>;
    void setTradingDateFn(DateFn date) { dateFn_ = std::move(date); }
    uint32_t tradingDate() const;

    // Inspect the last phase the scheduler drove the universe into. Useful
    // for tests and operational telemetry.
    SessionPhase currentPhase() const { return phase_.load(std::memory_order_acquire); }

    const SessionSchedule& schedule() const { return schedule_; }

private:
    // Apply a single phase's engine actions. Advances phase_ as a side
    // effect. Caller holds tickMutex_.
    void applyPhase(SessionPhase target);

    MatchingEngine& engine_;
    std::vector<SymbolId> symbols_;
    SessionSchedule schedule_;

    // Timer thread state — named to parallel MatchingEngine's expiry timer.
    std::thread timerThread_;
    std::atomic<bool> running_{false};
    uint64_t intervalMs_{1000};

    ClockFn clock_;
    DateFn dateFn_;

    // Default clock: wall-clock epoch ms read once, advanced by steady_clock.
    uint64_t defaultEpochMs() const;
    const uint64_t wallAnchorMs_;
    const std::chrono::steady_clock::time_point steadyAnchor_;

    // Serializes tick() against itself (timer thread vs. a manual caller) and
    // guards lastTick_. phase_ is atomic so currentPhase()/isRunning() can be
    // read without the lock.
    std::mutex tickMutex_;
    std::atomic<SessionPhase> phase_{SessionPhase::Idle};

    // The session clock within the current trading date: the largest
    // ms-of-day seen, so a backward step changes nothing. 0 before any tick.
    uint64_t lastTick_{0};
    // Trading date of the current session; kNoDate before the first tick.
    static constexpr uint64_t kNoDate = UINT64_MAX;
    uint64_t sessionDate_{kNoDate};
};

} // namespace OrderMatcher
