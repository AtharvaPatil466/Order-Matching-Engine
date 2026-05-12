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

#include "Types.h"
#include "AlertDispatcher.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
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

    GraduatedKillSwitch() = default;

    // ─── Level 1: Throttle ─────────────────────────────────────────────
    // Reduce a participant's rate to throttleFraction (default 10%).
    void throttle(ParticipantId participant, double throttleFraction = 0.10) {
        throttledParticipants_.push_back({participant, throttleFraction});
        currentLevel_.store(static_cast<uint8_t>(KillSwitchLevel::Throttle),
                            std::memory_order_release);
        if (alertDispatcher_) {
            alertDispatcher_->fire(AlertLevel::Warning, "kill_switch",
                "Throttle activated for participant " +
                std::to_string(participant) + " at " +
                std::to_string(static_cast<int>(throttleFraction * 100)) + "%");
        }
    }

    // Check if a participant is throttled. Returns the rate fraction (1.0 = normal).
    double getThrottleFraction(ParticipantId participant) const {
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
        haltedSymbols_.push_back(symbol);
        auto level = currentLevel_.load(std::memory_order_acquire);
        if (level < static_cast<uint8_t>(KillSwitchLevel::SymbolHalt)) {
            currentLevel_.store(static_cast<uint8_t>(KillSwitchLevel::SymbolHalt),
                                std::memory_order_release);
        }
        if (alertDispatcher_) {
            alertDispatcher_->fire(AlertLevel::Warning, "kill_switch",
                "Symbol halt: " + std::to_string(symbol));
        }
    }

    bool isSymbolHalted(SymbolId symbol) const {
        if (currentLevel_.load(std::memory_order_acquire) >=
            static_cast<uint8_t>(KillSwitchLevel::GlobalHalt)) {
            return true;  // Everything halted
        }
        for (SymbolId s : haltedSymbols_) {
            if (s == symbol) return true;
        }
        return false;
    }

    // ─── Level 3: Global Halt ──────────────────────────────────────────
    void globalHalt() {
        currentLevel_.store(static_cast<uint8_t>(KillSwitchLevel::GlobalHalt),
                            std::memory_order_release);
        if (alertDispatcher_) {
            alertDispatcher_->fire(AlertLevel::Critical, "kill_switch",
                "GLOBAL HALT activated — all matching stopped");
        }
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
        currentLevel_.store(static_cast<uint8_t>(KillSwitchLevel::Normal),
                            std::memory_order_release);
        throttledParticipants_.clear();
        haltedSymbols_.clear();
    }

    void clearThrottle(ParticipantId participant) {
        throttledParticipants_.erase(
            std::remove_if(throttledParticipants_.begin(),
                           throttledParticipants_.end(),
                           [participant](const ThrottleEntry& e) {
                               return e.participant == participant;
                           }),
            throttledParticipants_.end());
    }

    void unhaltSymbol(SymbolId symbol) {
        haltedSymbols_.erase(
            std::remove(haltedSymbols_.begin(), haltedSymbols_.end(), symbol),
            haltedSymbols_.end());
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

private:
    struct ThrottleEntry {
        ParticipantId participant;
        double fraction;
    };

    std::atomic<uint8_t> currentLevel_{
        static_cast<uint8_t>(KillSwitchLevel::Normal)};
    std::vector<ThrottleEntry> throttledParticipants_;
    std::vector<SymbolId> haltedSymbols_;
    AlertDispatcher* alertDispatcher_{nullptr};
    CheckpointFn checkpointFn_;
    ShutdownFn shutdownFn_;
};

}  // namespace OrderMatcher
