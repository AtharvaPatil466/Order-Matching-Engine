#pragma once

// LULDManager — Limit Up-Limit Down style volatility pauses.
//
// Roadmap Phase 4, Week 14: Limit Up-Limit Down
//
// Tracks a reference price (last auction price or rolling VWAP) per symbol.
// If best bid or ask deviates more than X% from reference, enters a pause
// state. In pause, orders are accepted but not matched. After Y seconds,
// resume with an auction. X and Y are configurable per symbol.

#include "Types.h"
#include <chrono>
#include <cstdint>

namespace OrderMatcher {

struct LULDConfig {
    double   bandPct{0.05};        // 5% default band width
    uint64_t pauseDurationMs{30000}; // 30 second pause duration
    bool     enabled{false};
};

enum class LULDState : uint8_t {
    Normal = 0,     // Trading normally
    Paused = 1,     // Volatility pause — accept orders, no matching
    Resuming = 2    // Post-pause auction in progress
};

class LULDManager {
public:
    LULDManager() = default;

    // Configure LULD for a symbol
    void configure(double bandPct, uint64_t pauseDurationMs) {
        config_.bandPct = bandPct;
        config_.pauseDurationMs = pauseDurationMs;
        config_.enabled = (bandPct > 0.0);
    }

    void enable() { config_.enabled = true; }
    void disable() { config_.enabled = false; }
    bool isEnabled() const { return config_.enabled; }

    // Set the reference price (typically last auction or session VWAP)
    void setReferencePrice(Price refPrice) {
        referencePrice_ = refPrice;
        if (refPrice > 0) {
            computeBands();
        }
    }

    Price getReferencePrice() const { return referencePrice_; }
    Price getUpperBand() const { return upperBand_; }
    Price getLowerBand() const { return lowerBand_; }

    // Check if a price breaches the LULD bands.
    // Returns true if the price is within acceptable bounds.
    bool checkPrice(Price price) const {
        if (!config_.enabled || referencePrice_ == 0) {
            return true;  // No band set, allow all
        }
        return price >= lowerBand_ && price <= upperBand_;
    }

    // Check if best bid/ask should trigger a pause.
    // Call this after every trade or book update.
    bool shouldPause(Price bestBid, Price bestAsk) const {
        if (!config_.enabled || referencePrice_ == 0) {
            return false;
        }
        if (state_ != LULDState::Normal) {
            return false;  // Already paused
        }

        // Check if best bid or ask breaches the band
        if (bestBid > 0 && bestBid > upperBand_) return true;
        if (bestAsk > 0 && bestAsk < lowerBand_) return true;

        return false;
    }

    // Enter pause state. Returns true if state actually changed.
    bool enterPause(uint64_t nowMs) {
        if (state_ != LULDState::Normal) return false;
        state_ = LULDState::Paused;
        pauseStartMs_ = nowMs;
        return true;
    }

    // Check if pause duration has elapsed.
    bool shouldResume(uint64_t nowMs) const {
        if (state_ != LULDState::Paused) return false;
        return (nowMs - pauseStartMs_) >= config_.pauseDurationMs;
    }

    // Transition to resume (auction) state.
    bool enterResume() {
        if (state_ != LULDState::Paused) return false;
        state_ = LULDState::Resuming;
        return true;
    }

    // Complete the resume and return to normal trading.
    bool completeResume(Price newRefPrice) {
        if (state_ != LULDState::Resuming) return false;
        state_ = LULDState::Normal;
        if (newRefPrice > 0) {
            referencePrice_ = newRefPrice;
            computeBands();
        }
        return true;
    }

    // Force reset to normal state (admin override)
    void reset() {
        state_ = LULDState::Normal;
        pauseStartMs_ = 0;
    }

    LULDState getState() const { return state_; }
    const LULDConfig& getConfig() const { return config_; }

    // Time remaining in pause (milliseconds), 0 if not paused
    uint64_t pauseRemainingMs(uint64_t nowMs) const {
        if (state_ != LULDState::Paused) return 0;
        uint64_t elapsed = nowMs - pauseStartMs_;
        if (elapsed >= config_.pauseDurationMs) return 0;
        return config_.pauseDurationMs - elapsed;
    }

private:
    void computeBands() {
        // Bands are reference ± (reference × bandPct)
        // Using fixed-point: multiply then divide
        Price bandWidth = static_cast<Price>(
            static_cast<double>(referencePrice_) * config_.bandPct);
        upperBand_ = referencePrice_ + bandWidth;
        lowerBand_ = referencePrice_ - bandWidth;
        if (lowerBand_ < 0) lowerBand_ = 0;
    }

    LULDConfig config_;
    LULDState  state_{LULDState::Normal};
    Price      referencePrice_{0};
    Price      upperBand_{0};
    Price      lowerBand_{0};
    uint64_t   pauseStartMs_{0};
};

}  // namespace OrderMatcher
