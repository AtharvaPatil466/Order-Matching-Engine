#pragma once

#include "Types.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>

namespace OrderMatcher {

// MicrostructureMetrics — header-only, running metrics calculator.
//
// Feed a stream of TradeEvent records via record(). After >= 1 call the
// "instantaneous" metrics are available; Kyle's lambda needs >= 10 events
// to produce a statistically meaningful slope.
//
// All metrics are computed over a circular buffer of the last 100 events.
// The buffer is never heap-allocated; it is a fixed-size stack array.

class MicrostructureMetrics {
public:
    static constexpr int kBufferSize = 100;

    struct TradeEvent {
        Price    price;       // execution price
        Side     side;        // aggressor side
        Quantity qty;         // traded volume
        Price    midpoint;    // mid-price AT time of trade
        uint64_t timestampNs;
    };

    // ── Ingestion ────────────────────────────────────────────────────

    void record(const TradeEvent& e) {
        buf_[head_] = e;
        head_ = (head_ + 1) % kBufferSize;
        if (count_ < kBufferSize) ++count_;
        ++totalCount_;
    }

    // ── Metrics ──────────────────────────────────────────────────────

    // Effective spread = mean of 2 * |trade_price - midpoint| across the
    // buffered window (tick-normalised units, same scale as Price).
    double effectiveSpread() const {
        if (count_ == 0) return 0.0;
        double sum = 0.0;
        forEach([&](const TradeEvent& e) {
            sum += 2.0 * std::abs(static_cast<double>(e.price - e.midpoint));
        });
        return sum / static_cast<double>(count_);
    }

    // Realized spread: 2 * D * (trade_price - midpoint_{t+N}), averaged
    // over all events that have at least forwardWindow subsequent events
    // recorded.
    //
    // D = +1 for buys, -1 for sells.
    // Returns 0 if fewer than forwardWindow+1 events have been recorded.
    double realizedSpread(int forwardWindow = 5) const {
        if (count_ <= forwardWindow) return 0.0;

        double sum  = 0.0;
        int    n    = 0;
        // Walk events [0 .. count_-1-forwardWindow] in chronological order.
        int eligible = count_ - forwardWindow;
        for (int i = 0; i < eligible; ++i) {
            int idx      = indexAt(i);
            int fwdIdx   = indexAt(i + forwardWindow);
            const TradeEvent& ev  = buf_[idx];
            const TradeEvent& fwd = buf_[fwdIdx];
            double D = (ev.side == Side::Buy) ? 1.0 : -1.0;
            sum += 2.0 * D * static_cast<double>(ev.price - fwd.midpoint);
            ++n;
        }
        return n > 0 ? sum / static_cast<double>(n) : 0.0;
    }

    // Kyle's lambda: OLS slope of Δmid ~ signed_order_flow over the window.
    //
    // signed_order_flow_i = +qty_i for buys, -qty_i for sells.
    // Δmid_i              = mid_{i+1} - mid_i
    //
    // Needs at least 10 events (the final event has no Δmid, so we need
    // count_ >= 10 to get >= 9 usable (x,y) pairs).
    //
    // Returns 0 if not enough data or if the flow variance is zero.
    double kylesLambda() const {
        if (count_ < 10) return 0.0;

        // Build (x, y) pairs in chronological order. We have (count_-1)
        // pairs because the last event has no "next" mid.
        int pairs = count_ - 1;
        double sumX = 0.0, sumY = 0.0, sumXX = 0.0, sumXY = 0.0;
        for (int i = 0; i < pairs; ++i) {
            const TradeEvent& cur  = buf_[indexAt(i)];
            const TradeEvent& next = buf_[indexAt(i + 1)];
            double x = signedFlow(cur);
            double y = static_cast<double>(next.midpoint - cur.midpoint);
            sumX  += x;
            sumY  += y;
            sumXX += x * x;
            sumXY += x * y;
        }
        double n   = static_cast<double>(pairs);
        double den = n * sumXX - sumX * sumX;
        if (std::abs(den) < 1e-12) return 0.0;
        return (n * sumXY - sumX * sumY) / den;
    }

    // Amihud illiquidity ratio: mean of |Δmid| / traded_volume over
    // consecutive (event_i, event_{i+1}) pairs.
    //
    // Returns 0 if fewer than 2 events or all volumes are zero.
    double amihudRatio() const {
        if (count_ < 2) return 0.0;
        double sum = 0.0;
        int    n   = 0;
        for (int i = 0; i < count_ - 1; ++i) {
            const TradeEvent& cur  = buf_[indexAt(i)];
            const TradeEvent& next = buf_[indexAt(i + 1)];
            double vol = static_cast<double>(cur.qty);
            if (vol == 0.0) continue;
            double absDeltaMid =
                std::abs(static_cast<double>(next.midpoint - cur.midpoint));
            sum += absDeltaMid / vol;
            ++n;
        }
        return n > 0 ? sum / static_cast<double>(n) : 0.0;
    }

    // Order flow imbalance = (buy_vol - sell_vol) / (buy_vol + sell_vol)
    // over the buffered window.
    // Returns 0 if total volume is zero.
    double orderFlowImbalance() const {
        if (count_ == 0) return 0.0;
        double buyVol  = 0.0;
        double sellVol = 0.0;
        forEach([&](const TradeEvent& e) {
            if (e.side == Side::Buy)
                buyVol  += static_cast<double>(e.qty);
            else
                sellVol += static_cast<double>(e.qty);
        });
        double total = buyVol + sellVol;
        if (total == 0.0) return 0.0;
        return (buyVol - sellVol) / total;
    }

    // ── Housekeeping ─────────────────────────────────────────────────

    // Total events ever recorded (not capped at kBufferSize).
    uint64_t sampleCount() const { return totalCount_; }

    void reset() {
        head_       = 0;
        count_      = 0;
        totalCount_ = 0;
        buf_        = {};
    }

private:
    // buf_ is a circular buffer. head_ points to the NEXT write slot.
    // The oldest element is at indexAt(0), the newest at indexAt(count_-1).
    std::array<TradeEvent, kBufferSize> buf_{};
    int      head_{0};
    int      count_{0};
    uint64_t totalCount_{0};

    // Convert a logical age index (0 = oldest) to a physical buffer index.
    int indexAt(int age) const {
        // Oldest element is at (head_ - count_ + age) mod kBufferSize.
        int raw = head_ - count_ + age;
        // Normalize into [0, kBufferSize).
        return ((raw % kBufferSize) + kBufferSize) % kBufferSize;
    }

    template <typename Fn>
    void forEach(Fn&& fn) const {
        for (int i = 0; i < count_; ++i) {
            fn(buf_[indexAt(i)]);
        }
    }

    static double signedFlow(const TradeEvent& e) {
        double q = static_cast<double>(e.qty);
        return (e.side == Side::Buy) ? q : -q;
    }
};

} // namespace OrderMatcher
