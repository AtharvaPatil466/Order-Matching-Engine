#pragma once

// OrderFlowAnalytics.h — header-only microstructure research statistics.
//
// Implements:
//   - Roll (1984) spread estimator
//   - Queue imbalance (single-level and weighted multi-level)
//   - Order arrival rate (Poisson MLE)
//   - Fill probability (logistic regression via Newton-Raphson)
//   - Order-to-trade ratio

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace OrderMatcher {

class OrderFlowAnalytics {
public:
    // -------------------------------------------------------------------------
    // Roll (1984) spread estimator
    // -------------------------------------------------------------------------

    // Record a trade price for the rolling Roll estimator.
    void recordTradePrice(double price) {
        prices_.push_back(price);
    }

    // Roll spread = 2 * sqrt(-cov(Δp_t, Δp_{t-1}))
    // where cov = mean(Δp_t * Δp_{t-1}) over all consecutive pairs.
    // Returns 0 if fewer than 3 prices have been recorded, or if the
    // first-order autocovariance is non-negative (no bounce).
    double rollSpread() const {
        if (prices_.size() < 3) return 0.0;

        // Compute price changes Δp_i = prices_[i+1] - prices_[i]
        const std::size_t n = prices_.size();
        std::vector<double> diffs;
        diffs.reserve(n - 1);
        for (std::size_t i = 0; i + 1 < n; ++i)
            diffs.push_back(prices_[i + 1] - prices_[i]);

        // First-order autocovariance = mean(Δp_t * Δp_{t-1})
        // pairs: (diffs[1], diffs[0]), (diffs[2], diffs[1]), ...
        const std::size_t pairs = diffs.size() - 1;  // >= 1 because n >= 3
        double sumProd = 0.0;
        for (std::size_t i = 0; i < pairs; ++i)
            sumProd += diffs[i + 1] * diffs[i];

        double cov = sumProd / static_cast<double>(pairs);
        if (cov >= 0.0) return 0.0;
        return 2.0 * std::sqrt(-cov);
    }

    // -------------------------------------------------------------------------
    // Queue imbalance
    // -------------------------------------------------------------------------

    // imbalance = (bidVol - askVol) / (bidVol + askVol), range [-1, +1].
    // Returns 0 if total volume is zero.
    static double queueImbalance(double bidVol, double askVol) {
        double total = bidVol + askVol;
        if (total == 0.0) return 0.0;
        return (bidVol - askVol) / total;
    }

    // Weighted queue imbalance: weight level i by 1/(i+1).
    // bidLevels[0] = best bid volume, askLevels[0] = best ask volume.
    // Returns 0 if weighted total volume is zero.
    static double weightedQueueImbalance(const std::vector<double>& bidLevels,
                                          const std::vector<double>& askLevels) {
        const std::size_t n = std::min(bidLevels.size(), askLevels.size());
        double wBid = 0.0, wAsk = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            double w = 1.0 / static_cast<double>(i + 1);
            wBid += w * bidLevels[i];
            wAsk += w * askLevels[i];
        }
        double total = wBid + wAsk;
        if (total == 0.0) return 0.0;
        return (wBid - wAsk) / total;
    }

    // -------------------------------------------------------------------------
    // Order arrival rate (Poisson intensity MLE)
    // -------------------------------------------------------------------------

    // Record an arrival at timestampUs (microseconds). The first call simply
    // anchors the clock; subsequent calls contribute an inter-arrival time.
    void recordArrival(uint64_t timestampUs) {
        arrivals_.push_back(timestampUs);
    }

    // MLE estimate: λ = 1 / mean(inter-arrival times).
    // Returns 0 if fewer than 2 arrivals have been recorded (no intervals).
    double arrivalRatePerUs() const {
        if (arrivals_.size() < 2) return 0.0;

        double sumInterArrival = 0.0;
        for (std::size_t i = 1; i < arrivals_.size(); ++i) {
            // Guard against non-monotonic timestamps
            if (arrivals_[i] > arrivals_[i - 1])
                sumInterArrival += static_cast<double>(arrivals_[i] - arrivals_[i - 1]);
        }
        std::size_t intervals = arrivals_.size() - 1;
        if (sumInterArrival == 0.0) return 0.0;
        double meanInterArrival = sumInterArrival / static_cast<double>(intervals);
        return 1.0 / meanInterArrival;
    }

    // -------------------------------------------------------------------------
    // Fill probability (logistic regression)
    // -------------------------------------------------------------------------

    struct FillObservation {
        double distanceTicks;
        int    filled;  // 0 or 1
    };

    // Record an observation for the logistic fit.
    void recordFillObservation(double distanceTicks, bool filled) {
        fillObs_.push_back({distanceTicks, filled ? 1 : 0});
        logisticFit_ = false;  // invalidate cached fit
    }

    // Estimate P(fill | distanceTicks) via logistic regression.
    // Model: P = 1 / (1 + exp(alpha + beta * distance))
    // Requires >= 4 observations with at least one fill and one non-fill.
    // Returns 0.5 if conditions are not met.
    double fillProbability(double distanceTicks) const {
        if (!logisticFit_) fitLogistic();
        if (!logisticFit_) return 0.5;
        double arg = logisticAlpha_ + logisticBeta_ * distanceTicks;
        // Clamp to avoid overflow in exp().
        arg = std::max(-500.0, std::min(500.0, arg));
        return 1.0 / (1.0 + std::exp(arg));
    }

    // -------------------------------------------------------------------------
    // Order-to-trade ratio (OTR)
    // -------------------------------------------------------------------------

    void recordOrderEvent() { ++orderCount_; }
    void recordTradeEvent() { ++tradeCount_; }

    // Returns orders / trades, or 0 if no trades have been recorded.
    double orderToTradeRatio() const {
        if (tradeCount_ == 0) return 0.0;
        return static_cast<double>(orderCount_) / static_cast<double>(tradeCount_);
    }

    // -------------------------------------------------------------------------
    // Reset
    // -------------------------------------------------------------------------

    void reset() {
        prices_.clear();
        arrivals_.clear();
        fillObs_.clear();
        logisticAlpha_ = 0.0;
        logisticBeta_  = 0.0;
        logisticFit_   = false;
        orderCount_    = 0;
        tradeCount_    = 0;
    }

private:
    std::vector<double>          prices_;
    std::vector<uint64_t>        arrivals_;
    std::vector<FillObservation> fillObs_;
    mutable double               logisticAlpha_{0.0};
    mutable double               logisticBeta_{0.0};
    mutable bool                 logisticFit_{false};
    uint64_t                     orderCount_{0};
    uint64_t                     tradeCount_{0};

    // Fit logistic regression via Newton-Raphson (5 iterations).
    // Sets logisticAlpha_, logisticBeta_, and logisticFit_ on success.
    void fitLogistic() const {
        const std::size_t n = fillObs_.size();
        if (n < 4) return;

        // Require at least one fill and one non-fill.
        int sumFilled = 0;
        for (const auto& obs : fillObs_) sumFilled += obs.filled;
        if (sumFilled == 0 || sumFilled == static_cast<int>(n)) return;

        double alpha = 0.0;
        double beta  = -0.5;

        for (int iter = 0; iter < 5; ++iter) {
            // Gradient and Hessian of the negative log-likelihood.
            // grad_alpha = sum(p_i - y_i)
            // grad_beta  = sum((p_i - y_i) * x_i)
            // H_aa = sum(p_i * (1 - p_i))
            // H_ab = sum(p_i * (1 - p_i) * x_i)
            // H_bb = sum(p_i * (1 - p_i) * x_i^2)
            double gA = 0.0, gB = 0.0;
            double Haa = 0.0, Hab = 0.0, Hbb = 0.0;

            for (const auto& obs : fillObs_) {
                double x   = obs.distanceTicks;
                double arg = alpha + beta * x;
                arg = std::max(-500.0, std::min(500.0, arg));
                double p   = 1.0 / (1.0 + std::exp(arg));
                double r   = static_cast<double>(obs.filled) - p;
                double w   = p * (1.0 - p);

                gA  += r;
                gB  += r * x;
                Haa += w;
                Hab += w * x;
                Hbb += w * x * x;
            }

            // Newton step: [alpha, beta] -= H^{-1} * grad
            double det = Haa * Hbb - Hab * Hab;
            if (std::abs(det) < 1e-12) break;

            double dAlpha = (Hbb * gA - Hab * gB) / det;
            double dBeta  = (Haa * gB - Hab * gA) / det;

            alpha -= dAlpha;
            beta  -= dBeta;
        }

        logisticAlpha_ = alpha;
        logisticBeta_  = beta;
        logisticFit_   = true;
    }
};

} // namespace OrderMatcher
