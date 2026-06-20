#pragma once

// OptimalExecution.h — header-only optimal execution trajectory planner.
// Implements the Almgren-Chriss (2001) framework for minimising
//   E[cost] + lambda * Var[cost]
// given a position X to unwind over N uniform time steps of width tau.
//
// No external dependencies: only <algorithm>, <cmath>, <cstddef>,
// <numeric>, <vector> from the standard library are used.
//
// Note: ACParams mirrors the fields of ImpactModel.h::AlmgrenChrisParams
// (eta, gamma) but is a self-contained struct that additionally carries
// sigma (per-step price volatility) and lambda (risk-aversion).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <vector>

namespace OrderMatcher {

// ---------------------------------------------------------------------------
// ACParams — Almgren-Chriss model parameters for execution scheduling.
// ---------------------------------------------------------------------------
struct ACParams {
    double eta;    // temporary impact coefficient: temp_cost = eta * v_i (linear)
    double gamma;  // permanent impact coefficient: perm_cost = gamma * v_i per step
    double sigma;  // per-step price volatility (annualized_vol * sqrt(tau))
    double lambda; // risk-aversion parameter (typical range 1e-6 to 1e-4)
};

// ---------------------------------------------------------------------------
// ExecutionSchedule — output of any trajectory-planning function.
// ---------------------------------------------------------------------------
struct ExecutionSchedule {
    std::vector<double> holdings;    // x_j: shares remaining at start of step j (size N+1)
    std::vector<double> trades;      // Delta x_j: shares to trade at step j (size N)
    std::vector<double> tradeRates;  // v_j = trades[j] / tau
    double expectedCost;             // E[total_cost] in price-units-per-share * X
    double costVariance;             // Var[total_cost]
    double sharpeRatio;              // -expectedCost / sqrt(costVariance); 0 if var == 0
};

// ---------------------------------------------------------------------------
// computeExpectedCost — E[cost] for an arbitrary trades vector.
//
// E[cost] = gamma/2 * X^2 + eta * sum_j( v_j^2 * tau )
//
// where X = sum(trades), v_j = trades[j] / tau.
// The gamma/2 * X^2 term is the permanent impact for unwinding the full
// position; the eta term accumulates temporary impact per step.
// ---------------------------------------------------------------------------
inline double computeExpectedCost(const std::vector<double>& trades,
                                   double tau,
                                   const ACParams& params) {
    if (trades.empty() || tau <= 0.0) return 0.0;

    double X = std::accumulate(trades.begin(), trades.end(), 0.0);
    double tempSum = 0.0;
    for (double dxj : trades) {
        double vj = dxj / tau;
        tempSum += vj * vj * tau;
    }
    return params.gamma / 2.0 * X * X + params.eta * tempSum;
}

// ---------------------------------------------------------------------------
// computeCostVariance — Var[cost] for an arbitrary trades vector.
//
// Var[cost] = sigma^2 * sum_j( x_j^2 * tau )
//
// where x_j is the holdings at the start of step j, derived from the trades.
// ---------------------------------------------------------------------------
inline double computeCostVariance(const std::vector<double>& trades,
                                   double tau,
                                   const ACParams& params) {
    if (trades.empty() || tau <= 0.0 || params.sigma == 0.0) return 0.0;

    // Reconstruct holdings: x_0 = sum(all trades), x_{j+1} = x_j - trades[j].
    const std::size_t N = trades.size();
    double X = std::accumulate(trades.begin(), trades.end(), 0.0);

    double varSum = 0.0;
    double xj = X;
    for (std::size_t j = 0; j < N; ++j) {
        varSum += xj * xj * tau;
        xj -= trades[j];
    }
    return params.sigma * params.sigma * varSum;
}

// ---------------------------------------------------------------------------
// Internal helper: fill an ExecutionSchedule from a trades vector.
// ---------------------------------------------------------------------------
namespace detail {
inline ExecutionSchedule buildSchedule(const std::vector<double>& trades,
                                        double tau,
                                        const ACParams& params) {
    const std::size_t N = trades.size();
    ExecutionSchedule s;
    s.trades     = trades;
    s.tradeRates.resize(N);
    s.holdings.resize(N + 1);

    double X = std::accumulate(trades.begin(), trades.end(), 0.0);
    s.holdings[0] = X;
    for (std::size_t j = 0; j < N; ++j) {
        s.holdings[j + 1] = s.holdings[j] - trades[j];
        s.tradeRates[j]   = (tau > 0.0) ? trades[j] / tau : 0.0;
    }

    s.expectedCost = computeExpectedCost(trades, tau, params);
    s.costVariance = computeCostVariance(trades, tau, params);
    s.sharpeRatio  = 0.0;
    if (s.costVariance > 0.0) {
        s.sharpeRatio = -s.expectedCost / std::sqrt(s.costVariance);
    }
    return s;
}
} // namespace detail

// ---------------------------------------------------------------------------
// twapSchedule — uniform TWAP baseline.
// trades[j] = X / N for all j.
// ---------------------------------------------------------------------------
inline ExecutionSchedule twapSchedule(double X, int N, double tau,
                                       const ACParams& params) {
    if (N <= 0) return ExecutionSchedule{};
    std::vector<double> trades(static_cast<std::size_t>(N), X / static_cast<double>(N));
    return detail::buildSchedule(trades, tau, params);
}

// ---------------------------------------------------------------------------
// vwapSchedule — volume-weighted schedule following a pre-specified curve.
// volumeCurve must sum to 1.0 (not enforced; caller responsibility).
// If volumeCurve is empty or wrong size, falls back to TWAP.
// ---------------------------------------------------------------------------
inline ExecutionSchedule vwapSchedule(double X, int N, double tau,
                                       const ACParams& params,
                                       const std::vector<double>& volumeCurve = {}) {
    if (N <= 0) return ExecutionSchedule{};
    const std::size_t Ns = static_cast<std::size_t>(N);

    if (volumeCurve.size() != Ns) {
        return twapSchedule(X, N, tau, params);
    }

    std::vector<double> trades(Ns);
    for (std::size_t j = 0; j < Ns; ++j) {
        trades[j] = X * volumeCurve[j];
    }
    return detail::buildSchedule(trades, tau, params);
}

// ---------------------------------------------------------------------------
// almgrenChrissSchedule — AC-optimal liquidation trajectory.
//
// Minimises E[cost] + lambda * Var[cost] over all adapted strategies that
// start at X and finish at 0.
//
// Optimal holdings:
//
//   x_j = X * sinh(kappa * (T - t_j)) / sinh(kappa * T)
//
//   where kappa = sqrt(lambda * sigma^2 / eta)
//         T     = N * tau
//         t_j   = j * tau
//
// Degenerate case (kappa < 1e-12): falls back to TWAP (risk-neutral limit).
// ---------------------------------------------------------------------------
inline ExecutionSchedule almgrenChrissSchedule(double X, int N, double tau,
                                                const ACParams& params) {
    if (N <= 0) return ExecutionSchedule{};
    const std::size_t Ns = static_cast<std::size_t>(N);

    // Compute kappa = sqrt(lambda * sigma^2 / eta).
    // Guard against eta == 0 or lambda/sigma being zero.
    double kappa = 0.0;
    if (params.eta > 0.0 && params.lambda >= 0.0 && params.sigma >= 0.0) {
        double kappaSq = params.lambda * params.sigma * params.sigma / params.eta;
        kappa = (kappaSq > 0.0) ? std::sqrt(kappaSq) : 0.0;
    }

    const double T = static_cast<double>(N) * tau;

    // Degenerate / risk-neutral limit: fall back to TWAP.
    if (std::fabs(kappa) < 1e-12 || std::fabs(std::sinh(kappa * T)) < 1e-12) {
        return twapSchedule(X, N, tau, params);
    }

    // Build optimal holdings x_j for j = 0 .. N.
    std::vector<double> holdings(Ns + 1);
    const double denominator = std::sinh(kappa * T);
    for (std::size_t j = 0; j <= Ns; ++j) {
        double tj = static_cast<double>(j) * tau;
        holdings[j] = X * std::sinh(kappa * (T - tj)) / denominator;
    }
    // Enforce exact boundary conditions to avoid floating-point drift.
    holdings[0]  = X;
    holdings[Ns] = 0.0;

    // Derive trades from holdings.
    std::vector<double> trades(Ns);
    for (std::size_t j = 0; j < Ns; ++j) {
        trades[j] = holdings[j] - holdings[j + 1];
    }

    return detail::buildSchedule(trades, tau, params);
}

} // namespace OrderMatcher
