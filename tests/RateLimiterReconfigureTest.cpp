// RateLimiterReconfigureTest — verifies live reconfiguration of the
// RateLimiter token-bucket registry via reconfigure().
//
// Tests:
//  1. Default (no rate set): all traffic allowed.
//  2. After setDefaultRate(10, 10): exactly 10 requests allowed for a
//     fresh participant; the 11th is rejected.
//  3. After reconfigure(20, 20): existing participant buckets are
//     cleared; the same participant ID is treated as fresh and gets a
//     full 20-token bucket.
//  4. After reconfigure(0, 0): limiter is disabled, all traffic allowed.

#include "RateLimiter.h"
#include <cassert>
#include <cstdio>

int main() {
    using namespace OrderMatcher;

    RateLimiter rl;

    // ── 1. No rate configured: every request passes ──────────────────────
    for (int i = 0; i < 100; ++i)
        assert(rl.allow(1) && "allow() must pass when no rate is set");
    std::puts("default (no rate): 100/100 allowed");

    // ── 2. Set rate 10/10 for participant 2 ──────────────────────────────
    // The bucket is created lazily on first allow() with burst=10, so
    // exactly 10 requests succeed before the bucket drains.
    rl.setDefaultRate(10, 10);
    int allowed = 0;
    for (int i = 0; i < 20; ++i) {
        if (rl.allow(2)) ++allowed;
    }
    assert(allowed == 10 &&
           "setDefaultRate(10,10): exactly 10 of 20 requests must be allowed");
    std::puts("setDefaultRate(10,10): 10/20 allowed for participant 2");

    // ── 3. reconfigure(20, 20): old buckets reset, participant 2 gets 20 ─
    // reconfigure() calls buckets_.clear(), so the previously-drained
    // bucket for participant 2 is gone. A fresh 20-token bucket is
    // created on first allow().
    rl.reconfigure(20, 20);
    allowed = 0;
    for (int i = 0; i < 30; ++i) {
        if (rl.allow(2)) ++allowed;
    }
    assert(allowed == 20 &&
           "reconfigure(20,20): exactly 20 of 30 requests must be allowed "
           "after bucket reset");
    std::puts("reconfigure(20,20): 20/30 allowed for participant 2 (fresh bucket)");

    // ── 4. reconfigure(0, 0): disabled — all traffic allowed ─────────────
    rl.reconfigure(0, 0);
    assert(!rl.isEnabled() && "reconfigure(0,0) must disable the limiter");
    for (int i = 0; i < 100; ++i)
        assert(rl.allow(3) && "allow() must pass when limiter is disabled");
    std::puts("reconfigure(0,0): 100/100 allowed (disabled)");

    std::puts("RateLimiterReconfigureTest: all assertions passed");
    return 0;
}
