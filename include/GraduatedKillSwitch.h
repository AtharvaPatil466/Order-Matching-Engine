#pragma once

// GraduatedKillSwitch — four-level graduated kill switch.
//
// Roadmap Phase 4, Week 14: Graduated Kill Switch
//
// Four escalation levels:
//   Throttle:     Per-participant token bucket rate reduced to 10%.
//   SymbolHalt:   Stop matching for one symbol, accept cancels only.
//   GlobalHalt:   Stop all matching, accept cancels only.
//   Kill:         Terminate process after writing checkpoint.
//
// Each level has a distinct admin endpoint and alert severity.
//
// Wired (roadmap 1.9 I6, GW-9): MatchingEngine consults it on every new order
// and every replace that grows or reprices — one relaxed load (engaged()) while
// it is at Normal — and installs the halt function, so a symbol or global halt
// halts the books themselves and reset() / unhaltSymbol() resume the ones it
// halted. Cancels and modifies down are always accepted. It was a library
// nothing read: no level stopped an order.
//
// The levels only rise on their own. A throttle under a global halt leaves the
// halt in force; clearThrottle and unhaltSymbol drop back to the highest level
// still in use, never below a global halt (reset() is the way out of that).

#include "Types.h"
#include "AlertDispatcher.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace OrderMatcher {

enum class KillSwitchLevel : uint8_t {
    Normal   = 0,   // No intervention
    Throttle = 1,   // Reduce rate to 10%
    SymbolHalt = 2, // Halt one symbol
    GlobalHalt = 3, // Halt all symbols
    Kill = 4        // Terminate after checkpoint
};

inline const char* killSwitchLevelStr(KillSwitchLevel level) {
    switch (level) {
        case KillSwitchLevel::Normal:     return "NORMAL";
        case KillSwitchLevel::Throttle:   return "THROTTLE";
        case KillSwitchLevel::SymbolHalt: return "SYMBOL_HALT";
        case KillSwitchLevel::GlobalHalt: return "GLOBAL_HALT";
        case KillSwitchLevel::Kill:       return "KILL";
    }
    return "UNKNOWN";
}

inline AlertLevel killSwitchAlertLevel(KillSwitchLevel level) {
    switch (level) {
        case KillSwitchLevel::Normal:     return AlertLevel::Info;
        case KillSwitchLevel::Throttle:   return AlertLevel::Warning;
        case KillSwitchLevel::SymbolHalt: return AlertLevel::Warning;
        case KillSwitchLevel::GlobalHalt: return AlertLevel::Critical;
        case KillSwitchLevel::Kill:       return AlertLevel::Fatal;
    }
    return AlertLevel::Critical;
}

class GraduatedKillSwitch {
public:
    using CheckpointFn = std::function<void()>;
    using ShutdownFn   = std::function<void()>;
    // Called, outside the switch's lock, after any halt changes: the engine
    // brings each book in line with isSymbolHalted().
    using HaltFn       = std::function<void()>;
    // Called, outside the lock and after HaltFn, after every change but kill():
    // the engine journals the state, so a restart keeps it.
    using ChangeFn     = std::function<void()>;

    // What a restart needs: the throttles (a fraction of 0 is a killed
    // participant), the halted symbols and a global halt. Throttle credit is
    // not kept; it restarts at 0. kill() ends the process and is not kept.
    struct PersistentState {
        std::vector<std::pair<ParticipantId, double>> throttles;
        std::vector<SymbolId> haltedSymbols;
        bool globalHalt{false};
        bool empty() const { return throttles.empty() && haltedSymbols.empty() && !globalHalt; }
    };

    GraduatedKillSwitch() = default;

    // ─── Level 1: Throttle ─────────────────────────────────────────────
    // Reduce a participant's rate to throttleFraction (default 10%): of its
    // orders, that fraction is admitted, evenly. 0 refuses every order and
    // replace it sends — a killed participant — until clearThrottle or reset.
    // (MatchingEngine::killSwitch(pid) cancels what it has resting.)
    void throttle(ParticipantId participant, double throttleFraction = 0.10) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = std::find_if(throttledParticipants_.begin(), throttledParticipants_.end(),
                                   [&](const ThrottleEntry& e) { return e.participant == participant; });
            if (it != throttledParticipants_.end()) *it = {participant, throttleFraction, 0.0};
            else throttledParticipants_.push_back({participant, throttleFraction, 0.0});
            raiseLevel(KillSwitchLevel::Throttle);
        }
        if (alertDispatcher_) {
            alertDispatcher_->fire(AlertLevel::Warning, "kill_switch",
                "Throttle activated for participant " +
                std::to_string(participant) + " at " +
                std::to_string(static_cast<int>(throttleFraction * 100)) + "%");
        }
        if (changeFn_) changeFn_();
    }

    // Check if a participant is throttled. Returns the rate fraction (1.0 = normal).
    double getThrottleFraction(ParticipantId participant) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& t : throttledParticipants_) {
            if (t.participant == participant) {
                return t.fraction;
            }
        }
        return 1.0;  // Not throttled
    }

    bool isThrottled(ParticipantId participant) const {
        return getThrottleFraction(participant) < 1.0;
    }

    // ─── Level 2: Symbol Halt ──────────────────────────────────────────
    void haltSymbol(SymbolId symbol) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (std::find(haltedSymbols_.begin(), haltedSymbols_.end(), symbol) ==
                haltedSymbols_.end())
                haltedSymbols_.push_back(symbol);
            raiseLevel(KillSwitchLevel::SymbolHalt);
        }
        if (haltFn_) haltFn_();
        if (alertDispatcher_) {
            alertDispatcher_->fire(AlertLevel::Warning, "kill_switch",
                "Symbol halt: " + std::to_string(symbol));
        }
        if (changeFn_) changeFn_();
    }

    bool isSymbolHalted(SymbolId symbol) const {
        if (currentLevel_.load(std::memory_order_acquire) >=
            static_cast<uint8_t>(KillSwitchLevel::GlobalHalt)) {
            return true;  // Everything halted
        }
        std::lock_guard<std::mutex> lock(mutex_);
        for (SymbolId s : haltedSymbols_) {
            if (s == symbol) return true;
        }
        return false;
    }

    // ─── Level 3: Global Halt ──────────────────────────────────────────
    void globalHalt() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            raiseLevel(KillSwitchLevel::GlobalHalt);
        }
        if (haltFn_) haltFn_();
        if (alertDispatcher_) {
            alertDispatcher_->fire(AlertLevel::Critical, "kill_switch",
                "GLOBAL HALT activated — all matching stopped");
        }
        if (changeFn_) changeFn_();
    }

    bool isGloballyHalted() const {
        return currentLevel_.load(std::memory_order_acquire) >=
               static_cast<uint8_t>(KillSwitchLevel::GlobalHalt);
    }

    // ─── Level 4: Kill ─────────────────────────────────────────────────
    void kill() {
        currentLevel_.store(static_cast<uint8_t>(KillSwitchLevel::Kill),
                            std::memory_order_release);
        if (alertDispatcher_) {
            alertDispatcher_->fire(AlertLevel::Fatal, "kill_switch",
                "KILL activated — writing checkpoint and terminating");
        }
        // Write checkpoint before shutdown
        if (checkpointFn_) {
            checkpointFn_();
        }
        if (shutdownFn_) {
            shutdownFn_();
        }
    }

    // ─── Reset ─────────────────────────────────────────────────────────
    void reset() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            currentLevel_.store(static_cast<uint8_t>(KillSwitchLevel::Normal),
                                std::memory_order_release);
            throttledParticipants_.clear();
            haltedSymbols_.clear();
        }
        if (haltFn_) haltFn_();
        if (changeFn_) changeFn_();
    }

    void clearThrottle(ParticipantId participant) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            throttledParticipants_.erase(
                std::remove_if(throttledParticipants_.begin(),
                               throttledParticipants_.end(),
                               [participant](const ThrottleEntry& e) {
                                   return e.participant == participant;
                               }),
                throttledParticipants_.end());
            lowerLevel();
        }
        if (changeFn_) changeFn_();
    }

    void unhaltSymbol(SymbolId symbol) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            haltedSymbols_.erase(
                std::remove(haltedSymbols_.begin(), haltedSymbols_.end(), symbol),
                haltedSymbols_.end());
            lowerLevel();
        }
        if (haltFn_) haltFn_();
        if (changeFn_) changeFn_();
    }

    // ─── Persistence ───────────────────────────────────────────────────
    PersistentState persistentState() const {
        std::lock_guard<std::mutex> lock(mutex_);
        PersistentState out;
        for (const auto& t : throttledParticipants_) out.throttles.emplace_back(t.participant, t.fraction);
        out.haltedSymbols = haltedSymbols_;
        out.globalHalt = currentLevel_.load(std::memory_order_relaxed) ==
                         static_cast<uint8_t>(KillSwitchLevel::GlobalHalt);
        return out;
    }

    // Replay: put a persisted state back. No HaltFn (the books' halts are
    // journaled on their own), no alert, no ChangeFn.
    void restoreState(const PersistentState& state) {
        std::lock_guard<std::mutex> lock(mutex_);
        throttledParticipants_.clear();
        for (const auto& [p, f] : state.throttles) throttledParticipants_.push_back({p, f, 0.0});
        haltedSymbols_ = state.haltedSymbols;
        currentLevel_.store(static_cast<uint8_t>(KillSwitchLevel::Normal), std::memory_order_relaxed);
        if (state.globalHalt) raiseLevel(KillSwitchLevel::GlobalHalt);
        else lowerLevel();
    }

    // ─── Configuration ─────────────────────────────────────────────────
    void setAlertDispatcher(AlertDispatcher* dispatcher) {
        alertDispatcher_ = dispatcher;
    }

    void setCheckpointFunction(CheckpointFn fn) {
        checkpointFn_ = std::move(fn);
    }

    void setShutdownFunction(ShutdownFn fn) {
        shutdownFn_ = std::move(fn);
    }

    // Set by MatchingEngine's constructor; see HaltFn.
    void setHaltFunction(HaltFn fn) {
        haltFn_ = std::move(fn);
    }

    // Set by MatchingEngine's constructor; see ChangeFn.
    void setChangeFunction(ChangeFn fn) {
        changeFn_ = std::move(fn);
    }

    // ─── State ─────────────────────────────────────────────────────────
    KillSwitchLevel currentLevel() const {
        return static_cast<KillSwitchLevel>(
            currentLevel_.load(std::memory_order_acquire));
    }

    // Should matching proceed for this symbol?
    bool canMatch(SymbolId symbol) const {
        auto level = currentLevel();
        if (level >= KillSwitchLevel::GlobalHalt) return false;
        if (level >= KillSwitchLevel::SymbolHalt && isSymbolHalted(symbol)) return false;
        return true;
    }

    // Should this order be accepted? (Cancels always accepted)
    bool canAcceptOrder(SymbolId symbol) const {
        return canMatch(symbol);
    }

    // Cancels are always accepted, even during halts
    bool canAcceptCancel() const {
        auto level = currentLevel();
        return level < KillSwitchLevel::Kill;
    }

    // ─── Admission (MatchingEngine) ────────────────────────────────────
    // The hot-path test: anything to consult at all? One relaxed load.
    bool engaged() const {
        return currentLevel_.load(std::memory_order_relaxed) !=
               static_cast<uint8_t>(KillSwitchLevel::Normal);
    }

    // A new order: KillSwitchActive while its symbol or everything is halted
    // or the participant is throttled to 0, RateLimitExceeded for a throttled
    // participant's order beyond its fraction, None to admit. Spends throttle
    // credit, so call once per order.
    RejectReason admit(SymbolId symbol, ParticipantId participant) {
        if (blocks(symbol, participant)) return RejectReason::KillSwitchActive;
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& t : throttledParticipants_) {
            if (t.participant != participant) continue;
            t.credit += t.fraction;
            if (t.credit < 1.0) return RejectReason::RateLimitExceeded;
            t.credit -= 1.0;
        }
        return RejectReason::None;
    }

    // Whether the switch refuses this symbol or participant outright: the
    // worker's re-check of a queued order and a replace's check. Spends nothing.
    bool blocks(SymbolId symbol, ParticipantId participant) const {
        if (!canAcceptOrder(symbol)) return true;
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& t : throttledParticipants_)
            if (t.participant == participant && t.fraction <= 0.0) return true;
        return false;
    }

private:
    struct ThrottleEntry {
        ParticipantId participant;
        double fraction;
        double credit;   // admit() spends 1.0 of it per admitted order
    };

    // Caller holds mutex_. Levels rise only here.
    void raiseLevel(KillSwitchLevel level) {
        if (currentLevel_.load(std::memory_order_relaxed) < static_cast<uint8_t>(level))
            currentLevel_.store(static_cast<uint8_t>(level), std::memory_order_release);
    }
    // Caller holds mutex_. Back to the highest level still in use, never out
    // of a global halt or a kill.
    void lowerLevel() {
        if (currentLevel_.load(std::memory_order_relaxed) >=
            static_cast<uint8_t>(KillSwitchLevel::GlobalHalt))
            return;
        const KillSwitchLevel level = !haltedSymbols_.empty() ? KillSwitchLevel::SymbolHalt
                                    : !throttledParticipants_.empty() ? KillSwitchLevel::Throttle
                                    : KillSwitchLevel::Normal;
        currentLevel_.store(static_cast<uint8_t>(level), std::memory_order_release);
    }

    std::atomic<uint8_t> currentLevel_{
        static_cast<uint8_t>(KillSwitchLevel::Normal)};
    // Guards the two lists. Taken only when engaged(), and never while
    // calling out (alerts, halt, checkpoint, shutdown).
    mutable std::mutex mutex_;
    std::vector<ThrottleEntry> throttledParticipants_;
    std::vector<SymbolId> haltedSymbols_;
    AlertDispatcher* alertDispatcher_{nullptr};
    CheckpointFn checkpointFn_;
    ShutdownFn shutdownFn_;
    HaltFn haltFn_;
    ChangeFn changeFn_;
};

}  // namespace OrderMatcher
