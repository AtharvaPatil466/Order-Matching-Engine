#pragma once

// SpreadDecomposition.h — header-only microstructure spread decomposition.
// Huang-Stoll (1997) three-component and Glosten-Harris (1988) two-component
// models.  OLS-through-the-origin on trade-direction indicators.
// No engine types required — pure math, double throughout.

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numeric>
#include <vector>

namespace OrderMatcher {

// One observation: a single trade with surrounding midpoint quotes.
struct SpreadObservation {
    double tradePrice;
    double midBefore;   // midpoint just BEFORE this trade
    double midAfter;    // midpoint just AFTER this trade (quote revision)
    int    direction;   // +1 = buy-initiated, -1 = sell-initiated
};

// Huang-Stoll (1997) three-component decomposition result.
struct HuangStollResult {
    double alpha;       // adverse selection fraction of half-spread [0, 1]
    double psi;         // inventory holding fraction [0, 1]
    double phi;         // order processing fraction = 1 - alpha - psi [0, 1]
    double halfSpread;  // mean(|tradePrice - midBefore|) — empirical half-spread
    double r2;          // R² of the OLS regression
    bool   valid;       // false if too few obs or degenerate inputs
};

// Glosten-Harris (1988) two-component decomposition result.
// NOTE: intentional single-'h' spelling in struct name.
struct GlostenhHarrisResult {
    double lambda;                  // permanent impact (adverse selection)
    double theta;                   // transient component (order processing)
    double adverseSelectionFraction;// lambda / (lambda + theta)
    double r2Lambda;                // R² for lambda regression
    double r2Theta;                 // R² for theta regression
    bool   valid;
};

// Input record for buildSpreadObservations.
struct TradeRecord {
    double price;
    double qty;   // signed: positive = buy-initiated, negative = sell-initiated
};

// Build a SpreadObservation sequence from trades + midpoints.
// mids.size() must equal trades.size() + 1 (mids[i] = mid before trade i).
// Direction from qty sign; zero qty falls back to Lee-Ready vs midBefore.
inline std::vector<SpreadObservation> buildSpreadObservations(
    const std::vector<TradeRecord>& trades,
    const std::vector<double>&      mids)
{
    if (mids.size() != trades.size() + 1) {
        assert(false && "mids.size() must equal trades.size() + 1");
        return {};
    }

    std::vector<SpreadObservation> obs;
    obs.reserve(trades.size());

    for (std::size_t i = 0; i < trades.size(); ++i) {
        SpreadObservation o{};
        o.tradePrice = trades[i].price;
        o.midBefore  = mids[i];
        o.midAfter   = mids[i + 1];

        if (trades[i].qty > 0.0)
            o.direction = +1;
        else if (trades[i].qty < 0.0)
            o.direction = -1;
        else {
            // Lee-Ready sign rule
            if (trades[i].price > mids[i])
                o.direction = +1;
            else if (trades[i].price < mids[i])
                o.direction = -1;
            else
                o.direction = +1;  // tie-break: classify as buy
        }

        obs.push_back(o);
    }
    return obs;
}

// OLS-through-origin helper: beta = Σ(y·x)/Σ(x²), R² vs mean(y).
namespace detail {

struct OlsResult { double beta; double r2; };

inline OlsResult olsOrigin(const std::vector<double>& y,
                            const std::vector<double>& x)
{
    assert(y.size() == x.size());
    const std::size_t n = y.size();
    if (n == 0) return {0.0, 0.0};

    double sumXX = 0.0, sumXY = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        sumXX += x[i] * x[i];
        sumXY += x[i] * y[i];
    }
    if (sumXX == 0.0) return {0.0, 0.0};

    const double beta = sumXY / sumXX;

    // R² relative to mean(y)
    double meanY = 0.0;
    for (double v : y) meanY += v;
    meanY /= static_cast<double>(n);

    double ssTot = 0.0, ssRes = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        double diff = y[i] - meanY;
        ssTot += diff * diff;
        double res = y[i] - beta * x[i];
        ssRes += res * res;
    }
    const double r2 = (ssTot == 0.0) ? 0.0 : 1.0 - ssRes / ssTot;
    return {beta, r2};
}

} // namespace detail

// Huang-Stoll fit: ΔM_t = β·Q_{t-1} + ε, β = (α+ψ)·S/2.
// ψ = 0 assumed (two-component within HS frame).  Min 5 obs, both sides.
inline HuangStollResult fitHuangStoll(const std::vector<SpreadObservation>& obs)
{
    HuangStollResult result{};
    result.valid = false;

    if (obs.size() < 5) return result;

    // Check both directions present
    bool hasBuy = false, hasSell = false;
    for (const auto& o : obs) {
        if (o.direction > 0) hasBuy  = true;
        if (o.direction < 0) hasSell = true;
    }
    if (!hasBuy || !hasSell) return result;

    // Build regression vectors: ΔM_t and Q_{t-1} for t in [1, N-1]
    const std::size_t N = obs.size();
    std::vector<double> dM, Q;
    dM.reserve(N - 1);
    Q.reserve(N - 1);

    for (std::size_t t = 1; t < N; ++t) {
        dM.push_back(obs[t].midAfter - obs[t - 1].midAfter);
        Q.push_back(static_cast<double>(obs[t - 1].direction));
    }

    // Empirical half-spread
    double sumHS = 0.0;
    for (const auto& o : obs)
        sumHS += std::fabs(o.tradePrice - o.midBefore);
    const double halfSpread = sumHS / static_cast<double>(N);

    result.halfSpread = halfSpread;

    // OLS
    auto [beta, r2] = detail::olsOrigin(dM, Q);
    result.r2 = r2;

    if (halfSpread == 0.0) {
        // No spread at all: all cost components are zero / undefined
        result.alpha = 0.0;
        result.psi   = 0.0;
        result.phi   = 1.0;
        result.valid = true;
        return result;
    }

    double alphaPlusPsi = beta / halfSpread;

    // Simplification: ψ = 0 (pure two-component within Huang-Stoll frame)
    double alpha = alphaPlusPsi;
    double psi   = 0.0;
    double phi   = 1.0 - alpha - psi;

    // Clamp to [0, 1] to handle numerical noise
    alpha = std::max(0.0, std::min(1.0, alpha));
    psi   = std::max(0.0, std::min(1.0, psi));
    phi   = std::max(0.0, std::min(1.0, phi));

    // Re-normalise so components sum exactly to 1
    double total = alpha + psi + phi;
    if (total > 0.0) { alpha /= total; psi /= total; phi /= total; }

    result.alpha = alpha;
    result.psi   = psi;
    result.phi   = phi;
    result.valid = true;
    return result;
}

// Glosten-Harris fit: two OLS regressions on Q_t (min 5 obs, both sides).
//   lambda: ΔM_t = λ·Q_t   (midAfter - midBefore)
//   theta:  ΔP_t = θ·Q_t   (tradePrice - midBefore)
inline GlostenhHarrisResult fitGlostenhHarris(
    const std::vector<SpreadObservation>& obs)
{
    GlostenhHarrisResult result{};
    result.valid = false;

    if (obs.size() < 5) return result;

    bool hasBuy = false, hasSell = false;
    for (const auto& o : obs) {
        if (o.direction > 0) hasBuy  = true;
        if (o.direction < 0) hasSell = true;
    }
    if (!hasBuy || !hasSell) return result;

    const std::size_t N = obs.size();
    std::vector<double> dM(N), dP(N), Q(N);

    for (std::size_t i = 0; i < N; ++i) {
        dM[i] = obs[i].midAfter  - obs[i].midBefore;
        dP[i] = obs[i].tradePrice - obs[i].midBefore;
        Q[i]  = static_cast<double>(obs[i].direction);
    }

    auto [lambda, r2L] = detail::olsOrigin(dM, Q);
    auto [theta,  r2T] = detail::olsOrigin(dP, Q);

    result.lambda   = lambda;
    result.theta    = theta;
    result.r2Lambda = r2L;
    result.r2Theta  = r2T;

    const double denom = lambda + theta;
    result.adverseSelectionFraction =
        (denom == 0.0) ? 0.0
                       : std::max(0.0, std::min(1.0, lambda / denom));

    result.valid = true;
    return result;
}

} // namespace OrderMatcher
