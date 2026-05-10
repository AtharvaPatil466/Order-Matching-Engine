#pragma once

#include "Types.h"
#include "LatencyTracker.h"
#include "FlatHashMap.h"
#include <cstdint>
#include <mutex>

namespace OrderMatcher {

// Token bucket rate limiter for per-participant message throttling.
// Each participant gets a bucket that refills at `ratePerSec` tokens/sec
// with a maximum burst of `burstSize` tokens.
//
// This bounds queue depth (and therefore tail latency) by capping the
// aggregate inbound message rate across all participants.
//
// Thread safety: RateLimiter serializes access with a coarse mutex.
// This keeps ingress throttling correct under concurrent producers without
// complicating the hot matching path itself.

class TokenBucket {
public:
    TokenBucket() = default;

    TokenBucket(uint64_t ratePerSec, uint64_t burstSize)
        : ratePerSec_(ratePerSec)
        , burstSize_(burstSize)
        , tokens_(burstSize)
        , lastRefillNs_(nowNs()) {}

    // Try to consume one token. Returns true if allowed, false if rate-limited.
    bool tryConsume(uint64_t nowNanos) {
        refill(nowNanos);
        if (tokens_ > 0) {
            --tokens_;
            return true;
        }
        return false;
    }

    uint64_t ratePerSec() const { return ratePerSec_; }
    uint64_t burstSize() const { return burstSize_; }
    uint64_t availableTokens() const { return tokens_; }

private:
    void refill(uint64_t nowNanos) {
        uint64_t elapsed = nowNanos - lastRefillNs_;
        // tokens to add = elapsed_ns * rate / 1e9
        uint64_t newTokens = (elapsed * ratePerSec_) / 1'000'000'000ULL;
        if (newTokens > 0) {
            tokens_ = std::min(burstSize_, tokens_ + newTokens);
            lastRefillNs_ = nowNanos;
        }
    }

    uint64_t ratePerSec_ = 0;
    uint64_t burstSize_ = 0;
    uint64_t tokens_ = 0;
    uint64_t lastRefillNs_ = 0;
};

// Per-participant rate limiter registry.
// Lazily creates a TokenBucket for each participant on first access.

class RateLimiter {
public:
    RateLimiter() = default;

    // Configure the default rate applied to all participants
    void setDefaultRate(uint64_t ratePerSec, uint64_t burstSize) {
        std::lock_guard<std::mutex> lock(mutex_);
        defaultRate_ = ratePerSec;
        defaultBurst_ = burstSize;
        enabled_ = (ratePerSec > 0);
    }

    // Override rate for a specific participant
    void setParticipantRate(ParticipantId id, uint64_t ratePerSec, uint64_t burstSize) {
        std::lock_guard<std::mutex> lock(mutex_);
        buckets_.insert(id, TokenBucket(ratePerSec, burstSize));
    }

    // Check if a message from this participant is allowed.
    // Returns true if allowed, false if rate-limited.
    bool allow(ParticipantId id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_) return true;

        auto* bucket = buckets_.find(id);
        if (!bucket) {
            buckets_.insert(id, TokenBucket(defaultRate_, defaultBurst_));
            bucket = buckets_.find(id);
        }
        return bucket->tryConsume(nowNs());
    }

    bool isEnabled() const { return enabled_; }
    uint64_t defaultRate() const { return defaultRate_; }
    uint64_t defaultBurst() const { return defaultBurst_; }

private:
    bool enabled_ = false;
    uint64_t defaultRate_ = 0;
    uint64_t defaultBurst_ = 0;
    FlatHashMap<ParticipantId, TokenBucket> buckets_;
    mutable std::mutex mutex_;
};

} // namespace OrderMatcher
