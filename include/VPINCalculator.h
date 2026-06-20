// VPINCalculator.h — Easley-López de Prado-O'Hara (2012) VPIN metric.
//
// Algorithm:
//  1. Partition cumulative traded volume into equal-sized buckets of size V.
//  2. Within each bucket, classify each trade using bulk volume classification:
//       V_buy  = qty * Phi((price - prevPrice) / sigma)
//       V_sell = qty * (1 - Phi(z))
//     where Phi is the standard normal CDF.
//  3. After each bucket closes: OI = |V_buy - V_sell|.
//  4. VPIN = mean(OI_i for last tau buckets) / V.
//
// Header-only. No external dependencies.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

namespace OrderMatcher {

class VPINCalculator {
public:
    // bucketVolume:   total volume per bucket (V in the algorithm above)
    // windowBuckets:  number of buckets in the rolling VPIN window (tau)
    // sigma:          per-trade price change std-dev for bulk classification.
    //                 Pass 0.0 to use a running Welford estimate.
    explicit VPINCalculator(double bucketVolume  = 1000.0,
                             int    windowBuckets = 50,
                             double sigma         = 0.0)
        : bucketVolume_(bucketVolume)
        , windowBuckets_(windowBuckets)
        , sigmaParam_(sigma)
    {
        imbalances_.resize(static_cast<std::size_t>(windowBuckets_), 0.0);
    }

    // Record a trade.
    //   price:     trade price
    //   qty:       trade quantity (always positive)
    //   prevPrice: last trade price before this one (for bulk classification)
    void recordTrade(double price, double qty, double prevPrice) {
        // Update running sigma estimate (Welford on |price - prevPrice|).
        double delta = price - prevPrice;
        updateSigma(std::fabs(delta));

        // Determine effective sigma for this trade.
        double sigEff = effectiveSigma();

        // Process qty, which may span multiple bucket boundaries.
        double remaining = qty;
        while (remaining > 0.0) {
            double spaceInBucket = bucketVolume_ - bucketVolAccum_;
            double chunk         = std::min(remaining, spaceInBucket);

            // Bulk volume classification for this chunk.
            double vbuy  = chunk * phi(delta, sigEff);
            double vsell = chunk - vbuy;

            bucketVBuy_    += vbuy;
            bucketVSell_   += vsell;
            bucketVolAccum_ += chunk;
            remaining       -= chunk;

            if (bucketVolAccum_ >= bucketVolume_ - 1e-12) {
                closeBucket();
                // After closing, subsequent chunks of the same trade use the
                // same bulk classification direction (same price delta).
            }
        }
    }

    // Current VPIN estimate in [0, 1].
    // Returns 0 if fewer than windowBuckets_ buckets have been completed.
    double vpin() const {
        if (completedBuckets_ < windowBuckets_) return 0.0;
        // imbalances_ is a circular buffer; all slots are valid.
        double sumOI = std::accumulate(imbalances_.begin(), imbalances_.end(), 0.0);
        double mean  = sumOI / static_cast<double>(windowBuckets_);
        return mean / bucketVolume_;
    }

    // Number of completed buckets so far.
    int completedBuckets() const { return completedBuckets_; }

    // Running sigma estimate (population std dev of |price - prevPrice| values).
    double sigmaCurrent() const {
        if (priceChangeCount_ < 2) return 0.0;
        double variance = runningM2_ / static_cast<double>(priceChangeCount_);
        return std::sqrt(variance);
    }

    void reset() {
        bucketVBuy_       = 0.0;
        bucketVSell_      = 0.0;
        bucketVolAccum_   = 0.0;
        completedBuckets_ = 0;
        runningMean_      = 0.0;
        runningM2_        = 0.0;
        priceChangeCount_ = 0;
        std::fill(imbalances_.begin(), imbalances_.end(), 0.0);
    }

private:
    // Standard normal CDF via erfc: Phi(z) = 0.5 * erfc(-z / sqrt(2)).
    static double phi(double priceDelta, double sigEff) {
        if (sigEff <= 0.0) return 0.5;  // defensive: equal split
        double z = priceDelta / sigEff;
        return 0.5 * std::erfc(-z / std::sqrt(2.0));
    }

    // Close the current bucket: push |OI| into the circular buffer and reset.
    void closeBucket() {
        double oi = std::fabs(bucketVBuy_ - bucketVSell_);
        std::size_t slot = static_cast<std::size_t>(completedBuckets_)
                           % static_cast<std::size_t>(windowBuckets_);
        imbalances_[slot] = oi;
        ++completedBuckets_;

        bucketVBuy_     = 0.0;
        bucketVSell_    = 0.0;
        bucketVolAccum_ = 0.0;
    }

    // Welford online update for |price - prevPrice|.
    void updateSigma(double absDelta) {
        ++priceChangeCount_;
        double d1 = absDelta - runningMean_;
        runningMean_ += d1 / static_cast<double>(priceChangeCount_);
        double d2 = absDelta - runningMean_;
        runningM2_  += d1 * d2;
    }

    // Return the sigma to use for classification: explicit param or running estimate.
    double effectiveSigma() const {
        if (sigmaParam_ > 0.0) return sigmaParam_;
        return sigmaCurrent();  // returns 0 when count < 2; phi() handles 0
    }

    // ── Configuration ──────────────────────────────────────────────────────────
    double bucketVolume_;
    int    windowBuckets_;
    double sigmaParam_;           // 0 = use running estimate

    // ── Current-bucket accumulators ───────────────────────────────────────────
    double bucketVBuy_{0.0};
    double bucketVSell_{0.0};
    double bucketVolAccum_{0.0};

    // ── Completed-bucket circular buffer ──────────────────────────────────────
    std::vector<double> imbalances_;   // size == windowBuckets_
    int completedBuckets_{0};

    // ── Running sigma estimation (Welford online) ─────────────────────────────
    double   runningMean_{0.0};
    double   runningM2_{0.0};
    uint64_t priceChangeCount_{0};
};

} // namespace OrderMatcher
