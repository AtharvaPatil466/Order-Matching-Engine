#pragma once

// ImpactModel.h — header-only market impact model library.
// Provides two models: Almgren-Chriss and the square-root law.
// No external dependencies: only <algorithm>, <cassert>, <cmath>,
// <numeric>, <vector> from the standard library are used.

#include "OrderBook.h"   // Trade, Side
#include "Types.h"       // Price, Quantity, Side

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numeric>
#include <vector>

namespace OrderMatcher {

// ---------------------------------------------------------------------------
// TradeObservation — a single normalised trade data point for model fitting.
// ---------------------------------------------------------------------------
struct TradeObservation {
    double tradeQty;    // signed: positive = buy aggressor, negative = sell aggressor
    double adv;         // average daily volume (normaliser)
    double priceImpact; // (mid_after - mid_before) / mid_before, signed
    double tempImpact;  // transient component (mid recovers); may be 0 if unknown
    double permImpact;  // permanent component
};

// ---------------------------------------------------------------------------
// Square-root law
// impact_i = c * sign(q_i) * sqrt(|q_i| / adv_i),  c = eta * sigma
// Fit a single coefficient c via OLS; report eta and sigma separately
// only as c (eta * sigma product) — the user may decompose if desired.
// ---------------------------------------------------------------------------
struct SqrtLawParams {
    double eta;   // dimensionless impact coefficient (= c when sigma = 1)
    double sigma; // volatility proxy absorbed into c; stored as 1.0 after fitting
    double r2;    // goodness of fit: 1 − SS_res / SS_tot
};

inline SqrtLawParams fitSqrtLaw(const std::vector<TradeObservation>& obs) {
    if (obs.size() < 2) return SqrtLawParams{0.0, 0.0, 0.0};

    // Build predictor x_i = sign(q_i) * sqrt(|q_i| / adv_i)
    // and response  y_i = priceImpact_i.
    // OLS for y = c * x (through origin):  c = sum(x*y) / sum(x*x)
    double sumXY = 0.0;
    double sumXX = 0.0;
    double sumY  = 0.0;
    double sumY2 = 0.0;
    const std::size_t n = obs.size();

    for (const auto& o : obs) {
        double adv = o.adv;
        if (adv <= 0.0) adv = 1.0; // guard against bad input

        double ratio  = std::fabs(o.tradeQty) / adv;
        double sqrtR  = std::sqrt(ratio);
        double sign   = (o.tradeQty >= 0.0) ? 1.0 : -1.0;
        double xi     = sign * sqrtR;

        sumXY += xi * o.priceImpact;
        sumXX += xi * xi;
        sumY  += o.priceImpact;
        sumY2 += o.priceImpact * o.priceImpact;
    }

    double c = (sumXX > 0.0) ? (sumXY / sumXX) : 0.0;

    // R² = 1 − SS_res / SS_tot (use mean-centred SS_tot)
    double meanY  = sumY / static_cast<double>(n);
    double ssTot  = sumY2 - static_cast<double>(n) * meanY * meanY;
    double r2     = 0.0;
    if (ssTot > 0.0) {
        double ssRes = 0.0;
        for (const auto& o : obs) {
            double adv   = (o.adv > 0.0) ? o.adv : 1.0;
            double ratio = std::fabs(o.tradeQty) / adv;
            double sqrtR = std::sqrt(ratio);
            double sign  = (o.tradeQty >= 0.0) ? 1.0 : -1.0;
            double pred  = c * sign * sqrtR;
            double res   = o.priceImpact - pred;
            ssRes += res * res;
        }
        r2 = 1.0 - ssRes / ssTot;
    }

    // Store c = eta*sigma with sigma = 1 so that predictSqrtLaw works cleanly.
    return SqrtLawParams{c, 1.0, r2};
}

inline double predictSqrtLaw(const SqrtLawParams& p, double qty, double adv) {
    if (adv <= 0.0) return 0.0;
    double ratio = std::fabs(qty) / adv;
    double sign  = (qty >= 0.0) ? 1.0 : -1.0;
    return p.eta * p.sigma * sign * std::sqrt(ratio);
}

// ---------------------------------------------------------------------------
// Almgren-Chriss
// temp component: tempImpact_i  = eta  * v_i^0.6,  v_i = |q_i| / adv_i
// perm component: permImpact_i  = gamma * v_i
// Fit eta and gamma independently (one-coefficient OLS through origin each).
// ---------------------------------------------------------------------------
struct AlmgrenChrisParams {
    double eta;    // temporary impact coefficient
    double gamma;  // permanent impact coefficient
    double r2Temp; // goodness of fit for temporary component
    double r2Perm; // goodness of fit for permanent component
};

inline AlmgrenChrisParams fitAlmgrenChriss(const std::vector<TradeObservation>& obs) {
    if (obs.size() < 2) return AlmgrenChrisParams{0.0, 0.0, 0.0, 0.0};

    // OLS through origin for each component:
    // eta   = sum(x_temp * y_temp) / sum(x_temp^2),  x_temp = v^0.6
    // gamma = sum(x_perm * y_perm) / sum(x_perm^2),  x_perm = v
    double sumXtYt = 0.0, sumXtXt = 0.0;
    double sumXpYp = 0.0, sumXpXp = 0.0;
    double sumYt = 0.0, sumYt2 = 0.0;
    double sumYp = 0.0, sumYp2 = 0.0;
    const std::size_t n = obs.size();

    for (const auto& o : obs) {
        double adv = (o.adv > 0.0) ? o.adv : 1.0;
        double v   = std::fabs(o.tradeQty) / adv;

        double xTemp = std::pow(v, 0.6);
        double xPerm = v;

        sumXtYt += xTemp * o.tempImpact;
        sumXtXt += xTemp * xTemp;

        sumXpYp += xPerm * o.permImpact;
        sumXpXp += xPerm * xPerm;

        sumYt  += o.tempImpact;
        sumYt2 += o.tempImpact * o.tempImpact;
        sumYp  += o.permImpact;
        sumYp2 += o.permImpact * o.permImpact;
    }

    double eta   = (sumXtXt > 0.0) ? (sumXtYt / sumXtXt) : 0.0;
    double gamma = (sumXpXp > 0.0) ? (sumXpYp / sumXpXp) : 0.0;

    // R² for each component (mean-centred)
    double dn      = static_cast<double>(n);
    double meanYt  = sumYt / dn;
    double ssTotT  = sumYt2 - dn * meanYt * meanYt;
    double meanYp  = sumYp / dn;
    double ssTotP  = sumYp2 - dn * meanYp * meanYp;

    double r2Temp = 0.0, r2Perm = 0.0;

    if (ssTotT > 0.0) {
        double ssRes = 0.0;
        for (const auto& o : obs) {
            double adv  = (o.adv > 0.0) ? o.adv : 1.0;
            double v    = std::fabs(o.tradeQty) / adv;
            double pred = eta * std::pow(v, 0.6);
            double res  = o.tempImpact - pred;
            ssRes += res * res;
        }
        r2Temp = 1.0 - ssRes / ssTotT;
    }

    if (ssTotP > 0.0) {
        double ssRes = 0.0;
        for (const auto& o : obs) {
            double adv  = (o.adv > 0.0) ? o.adv : 1.0;
            double v    = std::fabs(o.tradeQty) / adv;
            double pred = gamma * v;
            double res  = o.permImpact - pred;
            ssRes += res * res;
        }
        r2Perm = 1.0 - ssRes / ssTotP;
    }

    return AlmgrenChrisParams{eta, gamma, r2Temp, r2Perm};
}

inline double predictTempImpact(const AlmgrenChrisParams& p, double qty, double adv) {
    if (adv <= 0.0) return 0.0;
    double v = std::fabs(qty) / adv;
    return p.eta * std::pow(v, 0.6);
}

inline double predictPermImpact(const AlmgrenChrisParams& p, double qty, double adv) {
    if (adv <= 0.0) return 0.0;
    double v = std::fabs(qty) / adv;
    return p.gamma * v;
}

// ---------------------------------------------------------------------------
// buildObservations — construct TradeObservation vector from engine history.
// midpoints.size() must equal trades.size() + 1.
// Violating this precondition triggers an assertion in debug builds and
// returns an empty vector in release builds (no silent UB in either case).
// ---------------------------------------------------------------------------
inline std::vector<TradeObservation> buildObservations(
    const std::vector<Trade>&  trades,
    const std::vector<double>& midpoints, // size = trades.size() + 1
    double adv)
{
    assert(midpoints.size() == trades.size() + 1 &&
           "buildObservations: midpoints.size() must equal trades.size() + 1");

    if (midpoints.size() != trades.size() + 1) {
        return {};
    }

    std::vector<TradeObservation> result;
    result.reserve(trades.size());

    for (std::size_t i = 0; i < trades.size(); ++i) {
        const Trade& t = trades[i];
        double midBefore = midpoints[i];
        double midAfter  = midpoints[i + 1];

        // Signed quantity: positive if buy-aggressor, negative if sell-aggressor.
        double qty = static_cast<double>(t.quantity);
        if (t.aggressorSide == Side::Sell) qty = -qty;

        double impact = 0.0;
        if (midBefore != 0.0) {
            impact = (midAfter - midBefore) / midBefore;
        }

        // Without separate temp/perm decomposition from the engine we
        // assign the full impact to priceImpact; temp and perm are left
        // at zero for the caller to fill in if available.
        TradeObservation obs;
        obs.tradeQty    = qty;
        obs.adv         = adv;
        obs.priceImpact = impact;
        obs.tempImpact  = 0.0;
        obs.permImpact  = 0.0;
        result.push_back(obs);
    }

    return result;
}

} // namespace OrderMatcher
