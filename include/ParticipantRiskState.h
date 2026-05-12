#pragma once

// ParticipantRiskState — cache-line-aligned per-participant risk data.
//
// Roadmap Phase 1, Week 1: Data Structure Restructuring
//
// Problem: Risk data for multiple participants sharing cache lines causes
// false sharing when different threads access different participants.
//
// Solution: Every ParticipantRiskState is aligned to 64 bytes (one cache
// line). Hot fields (position limit, current exposure, inflight order
// count) are placed first. The struct is padded to exactly 64 bytes so
// that adjacent participants in an array never share a cache line.
//
// Cache-line layout (64 bytes):
//   ┌─────────────────────────────────────────────────────────────────┐
//   │ Bytes  0–7:  positionLimit      (Quantity, 8B)                 │
//   │ Bytes  8–15: currentExposure    (int64_t,  8B) — signed net   │
//   │ Bytes 16–23: ordersInFlight     (uint64_t, 8B)                │
//   │ Bytes 24–31: maxOrderSize       (Quantity, 8B)                │
//   │ Bytes 32–39: maxOrderNotional   (Price,    8B)                │
//   │ Bytes 40–47: maxPositionSize    (Quantity, 8B)                │
//   │ Bytes 48–55: ordersSubmitted    (uint64_t, 8B)                │
//   │ Bytes 56–63: tradesExecuted     (uint64_t, 8B)                │
//   └─────────────────────────────────────────────────────────────────┘
//
// All fields are non-atomic: accessed only by the owning worker thread.
// Cross-thread reads (admin endpoints, snapshots) must coordinate via
// the existing bookLock_ shared_mutex.

#include "Types.h"
#include <cstdint>
#include <cstring>

namespace OrderMatcher {

struct alignas(64) ParticipantRiskState {
    // ─── Hot path fields (accessed on every order) ───────────────────
    Quantity positionLimit{0};       // Max absolute position (0 = no limit)
    int64_t  currentExposure{0};    // Signed net position (buy - sell)
    uint64_t ordersInFlight{0};     // Currently active orders

    // ─── Risk limits (checked per order, set infrequently) ───────────
    Quantity maxOrderSize{0};       // Max single order size (0 = no limit)
    Price    maxOrderNotional{0};   // Max price×qty notional (0 = no limit)
    Quantity maxPositionSize{0};    // Max absolute position (0 = no limit)

    // ─── Statistics (updated per order/trade) ────────────────────────
    uint64_t ordersSubmitted{0};
    uint64_t tradesExecuted{0};

    // ─── Derived metrics ─────────────────────────────────────────────

    double getOTR() const {
        return static_cast<double>(ordersSubmitted) /
               static_cast<double>(tradesExecuted > 0 ? tradesExecuted : 1);
    }

    int64_t absExposure() const {
        return currentExposure >= 0 ? currentExposure : -currentExposure;
    }

    // Check if a new order would violate risk limits.
    // Returns true if the order is within limits.
    bool checkOrder(Price price, Quantity qty, Side side) const {
        // Max single order size
        if (maxOrderSize > 0 && qty > maxOrderSize) {
            return false;
        }

        // Max order notional (price × qty)
        if (maxOrderNotional > 0) {
            Price notional = price * static_cast<Price>(qty);
            if (notional > maxOrderNotional) {
                return false;
            }
        }

        // Max position: project new exposure
        if (maxPositionSize > 0) {
            int64_t delta = (side == Side::Buy)
                ? static_cast<int64_t>(qty)
                : -static_cast<int64_t>(qty);
            int64_t projected = currentExposure + delta;
            int64_t absProjected = projected >= 0 ? projected : -projected;
            if (static_cast<Quantity>(absProjected) > maxPositionSize) {
                return false;
            }
        }

        return true;
    }

    // Record a trade execution
    void recordTrade(Quantity qty, Side side) {
        int64_t delta = (side == Side::Buy)
            ? static_cast<int64_t>(qty)
            : -static_cast<int64_t>(qty);
        currentExposure += delta;
        ++tradesExecuted;
    }

    // Record order submission
    void recordOrderSubmit() {
        ++ordersSubmitted;
        ++ordersInFlight;
    }

    // Record order completion (fill, cancel, expire)
    void recordOrderComplete() {
        if (ordersInFlight > 0) {
            --ordersInFlight;
        }
    }

    void reset() {
        std::memset(this, 0, sizeof(*this));
    }
};

// Verify our size and alignment constraints
static_assert(sizeof(ParticipantRiskState) == 64,
              "ParticipantRiskState must be exactly one cache line (64 bytes)");
static_assert(alignof(ParticipantRiskState) == 64,
              "ParticipantRiskState must be cache-line aligned");

}  // namespace OrderMatcher
