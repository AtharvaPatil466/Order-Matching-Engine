#pragma once

// ExecutionAnalytics.h — header-only execution quality calculator.
// Measures how well a sequence of fills performed against common benchmarks
// (Implementation Shortfall, VWAP, TWAP, spread capture).
// No external dependencies: only <algorithm>, <cmath>, <numeric>, <vector>.

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace OrderMatcher {

// One fill record: price and signed quantity (positive = buy, negative = sell).
struct FillRecord {
    double   price;
    double   qty;     // signed: > 0 buy, < 0 sell
    uint64_t seqNum;  // monotonically increasing, used to order fills
};

// A reference price series aligned to fill sequence numbers.
// midAtFill[i]    = mid price at the moment fill i occurred.
// postTradeMid[i] = mid N fills after fill i (for realised impact).
struct ReferenceSeries {
    double              arrivalPrice;   // mid at the moment the parent order was submitted
    std::vector<double> midAtFill;      // mid at each fill
    std::vector<double> postTradeMid;   // mid N fills after each fill
    int                 postTradeWindow = 5;
};

class ExecutionAnalytics {
public:
    // -----------------------------------------------------------------------
    // Data ingestion
    // -----------------------------------------------------------------------

    // Append a fill. Multiple calls accumulate; call reset() to start over.
    void record(const FillRecord& f) { fills_.push_back(f); }

    void setReference(const ReferenceSeries& ref) {
        ref_    = ref;
        hasRef_ = true;
    }

    // -----------------------------------------------------------------------
    // Summary
    // -----------------------------------------------------------------------

    uint64_t fillCount()    const { return static_cast<uint64_t>(fills_.size()); }

    double totalQty() const {
        double sum = 0.0;
        for (const auto& f : fills_) sum += std::fabs(f.qty);
        return sum;
    }

    double avgFillPrice() const {
        double absQty = totalQty();
        if (absQty == 0.0) return 0.0;
        double wsum = 0.0;
        for (const auto& f : fills_) wsum += f.price * std::fabs(f.qty);
        return wsum / absQty;
    }

    // -----------------------------------------------------------------------
    // VWAP
    // -----------------------------------------------------------------------

    // Parent-order VWAP = sum(price_i * |qty_i|) / sum(|qty_i|)
    double parentVWAP() const { return avgFillPrice(); }

    // Market VWAP from the reference mid series over the same fill window.
    double marketVWAP() const {
        if (!hasRef_ || fills_.empty()) return 0.0;
        const std::size_t n = std::min(fills_.size(), ref_.midAtFill.size());
        if (n == 0) return 0.0;
        double absQty = 0.0;
        double wsum   = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            double aq = std::fabs(fills_[i].qty);
            wsum   += ref_.midAtFill[i] * aq;
            absQty += aq;
        }
        return (absQty == 0.0) ? 0.0 : wsum / absQty;
    }

    // vwapSlippage = parentVWAP - marketVWAP
    // Positive means we paid more than the market VWAP (worse for buys).
    double vwapSlippage() const {
        if (!hasRef_ || fills_.empty()) return 0.0;
        return parentVWAP() - marketVWAP();
    }

    // -----------------------------------------------------------------------
    // TWAP
    // -----------------------------------------------------------------------

    // TWAP = simple average of midAtFill entries (uniform time spacing assumed).
    double twap() const {
        if (!hasRef_ || ref_.midAtFill.empty()) return 0.0;
        const std::size_t n = std::min(fills_.size(), ref_.midAtFill.size());
        if (n == 0) return 0.0;
        double sum = 0.0;
        for (std::size_t i = 0; i < n; ++i) sum += ref_.midAtFill[i];
        return sum / static_cast<double>(n);
    }

    // twapSlippage = parentVWAP - twap(midAtFill)
    double twapSlippage() const {
        if (!hasRef_ || fills_.empty()) return 0.0;
        return parentVWAP() - twap();
    }

    // -----------------------------------------------------------------------
    // Implementation Shortfall decomposition
    // -----------------------------------------------------------------------

    // direction: +1 if the overall program is a buy, -1 if sell.
    // Inferred from the sign of total signed qty.
    double direction() const {
        double signedQty = 0.0;
        for (const auto& f : fills_) signedQty += f.qty;
        return (signedQty >= 0.0) ? 1.0 : -1.0;
    }

    // realizedImpact = IS = (parentVWAP - arrivalPrice) * direction
    double realizedImpact() const {
        if (!hasRef_ || fills_.empty()) return 0.0;
        return (parentVWAP() - ref_.arrivalPrice) * direction();
    }

    // implementationShortfall — alias for realizedImpact per the spec.
    double implementationShortfall() const { return realizedImpact(); }

    // timingCost = (mean(midAtFill) - arrivalPrice) * direction
    double timingCost() const {
        if (!hasRef_ || fills_.empty()) return 0.0;
        const std::size_t n = std::min(fills_.size(), ref_.midAtFill.size());
        if (n == 0) return 0.0;
        double sum = 0.0;
        for (std::size_t i = 0; i < n; ++i) sum += ref_.midAtFill[i];
        double meanMid = sum / static_cast<double>(n);
        return (meanMid - ref_.arrivalPrice) * direction();
    }

    // marketImpact = realizedImpact - timingCost
    // (the alpha captured by the act of trading itself)
    double marketImpact() const {
        if (!hasRef_ || fills_.empty()) return 0.0;
        return realizedImpact() - timingCost();
    }

    // -----------------------------------------------------------------------
    // Spread capture
    // -----------------------------------------------------------------------

    // For each fill: spreadCapture_i = (fill_price - mid_at_fill) * sign(qty_i)
    //   Positive = resting/maker fill (captured the spread).
    //   Negative = aggressive/taker fill (paid the spread).
    double meanSpreadCapture() const {
        if (!hasRef_ || fills_.empty()) return 0.0;
        const std::size_t n = std::min(fills_.size(), ref_.midAtFill.size());
        if (n == 0) return 0.0;
        double sum = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            double side = (fills_[i].qty >= 0.0) ? 1.0 : -1.0;
            sum += (fills_[i].price - ref_.midAtFill[i]) * side;
        }
        return sum / static_cast<double>(n);
    }

    // -----------------------------------------------------------------------
    // Reset
    // -----------------------------------------------------------------------

    void reset() {
        fills_.clear();
        ref_    = ReferenceSeries{};
        hasRef_ = false;
    }

private:
    std::vector<FillRecord> fills_;
    ReferenceSeries         ref_;
    bool                    hasRef_{false};
};

} // namespace OrderMatcher
