// SignalGenerator.h — Alpha signal derivation from microstructure state.
//
// Each signal returns a value in [-1, +1] where +1 = strong buy,
// -1 = strong sell, 0 = neutral.  A companion confidence in [0, 1]
// indicates data-quality reliability.
//
// Header-only. No engine dependencies. Inputs arrive pre-packaged in
// MicrostructureSnapshot.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

namespace OrderMatcher {

// Snapshot of microstructure state passed to signal generators.
struct MicrostructureSnapshot {
    double   vpin;              // from VPINCalculator [0,1]
    double   orderFlowImbalance; // from MicrostructureMetrics [-1,1]
    double   rollSpread;        // from OrderFlowAnalytics::rollSpread()
    double   kylesLambda;       // from MicrostructureMetrics::kylesLambda()
    double   queueImbalance;    // from OrderFlowAnalytics::queueImbalance() [-1,1]
    double   arrivalRate;       // from OrderFlowAnalytics::arrivalRatePerUs()
    double   midPrice;          // current mid
    double   prevMidPrice;      // previous mid (for momentum)
    uint64_t timestampNs;
};

// A single signal reading.
struct Signal {
    double      value;      // [-1, +1]
    double      confidence; // [0, 1]
    uint64_t    timestamp;
    const char* name;       // static string
};

class SignalGenerator {
public:
    // ── 1. Order flow pressure ────────────────────────────────────────────────
    // Combines OFI and queue imbalance, dampened by VPIN toxicity.
    // value      = (0.6 * ofi + 0.4 * queueImbalance) * (1 - vpin)
    // confidence = clamp(|ofi|, 0, 1) * (1 - vpin)
    static Signal orderFlowPressure(const MicrostructureSnapshot& s) {
        double raw  = 0.6 * s.orderFlowImbalance + 0.4 * s.queueImbalance;
        double damp = 1.0 - s.vpin;
        double val  = raw * damp;
        double conf = std::min(std::fabs(s.orderFlowImbalance), 1.0) * damp;
        return { clamp(val), clamp01(conf), s.timestampNs,
                 "order_flow_pressure" };
    }

    // ── 2. Momentum signal ────────────────────────────────────────────────────
    // Short-term price momentum normalised by the Roll spread.
    // value      = clamp((midPrice - prevMidPrice) / rollSpread, -1, 1)
    // confidence = rollSpread > 0 ? 1.0 : 0.0
    static Signal momentumSignal(const MicrostructureSnapshot& s) {
        if (s.rollSpread <= 0.0) {
            return { 0.0, 0.0, s.timestampNs, "momentum" };
        }
        double val  = (s.midPrice - s.prevMidPrice) / s.rollSpread;
        return { clamp(val), 1.0, s.timestampNs, "momentum" };
    }

    // ── 3. Adverse selection pressure ────────────────────────────────────────
    // Kyle's lambda captures price impact per unit signed order flow.
    // High lambda → informed traders dominate → fade current OFI direction.
    // value      = -sign(ofi) * clamp(kylesLambda / referenceImpact, 0, 1)
    // confidence = kylesLambda > 0 ? 1.0 : 0.0
    static Signal adverseSelectionSignal(const MicrostructureSnapshot& s,
                                          double referenceImpact = 1e-4) {
        if (s.kylesLambda <= 0.0) {
            return { 0.0, 0.0, s.timestampNs, "adverse_selection" };
        }
        double denom = (referenceImpact > 0.0) ? referenceImpact : 1e-4;
        double mag   = clamp01(s.kylesLambda / denom);
        double sgn   = (s.orderFlowImbalance >= 0.0) ? 1.0 : -1.0;
        double val   = -sgn * mag;
        return { clamp(val), 1.0, s.timestampNs, "adverse_selection" };
    }

    // ── 4. Composite signal ───────────────────────────────────────────────────
    // Confidence-weighted average of the three signals above.
    // weights = {0.4, 0.3, 0.3} for {orderFlowPressure, momentum, adverseSelection}
    // Returns Signal with value = weighted_mean, confidence = mean(confidences).
    static Signal compositeSignal(const MicrostructureSnapshot& s,
                                   double referenceImpact = 1e-4) {
        Signal ofp = orderFlowPressure(s);
        Signal mom = momentumSignal(s);
        Signal adv = adverseSelectionSignal(s, referenceImpact);

        const double w0 = 0.4, w1 = 0.3, w2 = 0.3;

        double wSum = w0 * ofp.confidence + w1 * mom.confidence
                    + w2 * adv.confidence;
        double val;
        if (wSum <= 0.0) {
            val = 0.0;
        } else {
            val = (w0 * ofp.confidence * ofp.value
                 + w1 * mom.confidence * mom.value
                 + w2 * adv.confidence * adv.value) / wSum;
        }
        double conf = (ofp.confidence + mom.confidence + adv.confidence) / 3.0;
        return { clamp(val), clamp01(conf), s.timestampNs, "composite" };
    }

    // ── 5. Signal history ─────────────────────────────────────────────────────
    void recordSnapshot(const MicrostructureSnapshot& s,
                        double referenceImpact = 1e-4) {
        Signal sig = compositeSignal(s, referenceImpact);
        history_.push_back(sig);
        // Keep only the last windowSize_ entries.
        if (static_cast<int>(history_.size()) > windowSize_) {
            history_.erase(history_.begin());
        }
    }

    double meanSignal() const {
        if (history_.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& s : history_) sum += s.value;
        return sum / static_cast<double>(history_.size());
    }

    double signalStdDev() const {
        if (history_.size() < 2) return 0.0;
        double mean = meanSignal();
        double sq   = 0.0;
        for (const auto& s : history_) {
            double d = s.value - mean;
            sq += d * d;
        }
        return std::sqrt(sq / static_cast<double>(history_.size()));
    }

    // meanSignal / signalStdDev; returns 0 when stdDev == 0.
    double sharpeSignal() const {
        double sd = signalStdDev();
        if (sd < 1e-12) return 0.0;
        return meanSignal() / sd;
    }

    void reset() { history_.clear(); }

    void setWindowSize(int n) {
        windowSize_ = (n > 0) ? n : 1;
        // Trim existing history if needed.
        while (static_cast<int>(history_.size()) > windowSize_) {
            history_.erase(history_.begin());
        }
    }

private:
    std::vector<Signal> history_;
    int windowSize_{20};

    // Clamp to [-1, +1].
    static double clamp(double v) {
        return std::max(-1.0, std::min(1.0, v));
    }

    // Clamp to [0, 1].
    static double clamp01(double v) {
        return std::max(0.0, std::min(1.0, v));
    }
};

} // namespace OrderMatcher
