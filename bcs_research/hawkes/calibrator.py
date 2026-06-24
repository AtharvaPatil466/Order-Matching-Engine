"""Univariate Hawkes calibrator (exponential kernel) by maximum likelihood.

Estimates the self-exciting intensity

    lambda(t) = mu + sum_{t_i < t} alpha * exp(-beta * (t - t_i))

and reports the BRANCHING RATIO n = alpha / beta — the expected number of
offspring per event, i.e. the fraction of activity that is endogenously
triggered rather than exogenous. n -> 1 is the critical (reflexive) limit
(Filimonov & Sornette 2012). This is the quantity the BCS Hawkes bridge tracks
to test whether endogenous order-flow excitation rises before liquidity gaps.

Exact log-likelihood for events {t_1..t_N} on [0, T] (Ozaki recursion for the
intensity sum, O(N) per evaluation):

    LL = sum_i log(lambda(t_i)) - integral_0^T lambda(s) ds
       = sum_i log(mu + alpha * A_i) - mu*T - (alpha/beta) * sum_i (1 - e^{-beta (T - t_i)})
    A_1 = 0,  A_i = e^{-beta (t_i - t_{i-1})} (1 + A_{i-1})

Optimized over (mu, alpha, beta) > 0 with L-BFGS-B (SciPy). Vendored in the BCS
repo because the AlphaForge Hawkes module is not yet built (Phase 1 forthcoming);
the calibrate_univariate(times, T) -> fit.params.branching_ratio API mirrors the
plan's intended signature so AlphaForge can adopt it later.
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from scipy.optimize import minimize

MIN_EVENTS = 20          # below this the MLE is not stable enough to trust
_FLOOR = 1e-9            # positivity floor for parameters / intensities
_BIG = 1e12             # penalty returned for infeasible parameters


@dataclass(frozen=True)
class HawkesParams:
    mu: float
    alpha: float
    beta: float

    @property
    def branching_ratio(self) -> float:
        return self.alpha / self.beta if self.beta > 0 else float("inf")


@dataclass(frozen=True)
class HawkesFit:
    converged: bool
    params: HawkesParams
    n_events: int
    loglik: float
    T: float


def _neg_loglik(theta: np.ndarray, dt: np.ndarray, t: np.ndarray, T: float) -> float:
    mu, alpha, beta = theta
    if mu <= 0.0 or alpha <= 0.0 or beta <= 0.0:
        return _BIG
    # Ozaki recursion for A_i = sum_{j<i} exp(-beta (t_i - t_j)).
    a = 0.0
    log_intensity_sum = np.log(mu)          # i = 0: A_0 = 0 -> lambda = mu
    for k in range(dt.size):                # dt[k] = t[k+1] - t[k]
        a = np.exp(-beta * dt[k]) * (1.0 + a)
        lam = mu + alpha * a
        if lam <= 0.0:
            return _BIG
        log_intensity_sum += np.log(lam)
    compensator = mu * T + (alpha / beta) * np.sum(1.0 - np.exp(-beta * (T - t)))
    ll = log_intensity_sum - compensator
    if not np.isfinite(ll):
        return _BIG
    return -ll


def calibrate_univariate(times, T: float) -> HawkesFit:
    """MLE-fit a univariate exponential-kernel Hawkes process to event `times`.

    `times` are event timestamps (any consistent unit; the branching ratio is
    dimensionless) and `T` is the absolute end of the observation window. Times
    are sorted and shifted to start at 0 internally. Returns a HawkesFit; for
    fewer than MIN_EVENTS events or a failed optimization, `converged` is False
    and the params carry the (un-trusted) initial/last estimate.
    """
    times = np.asarray(times, dtype=float).ravel()
    n = times.size
    if n < MIN_EVENTS:
        return HawkesFit(False, HawkesParams(float("nan"), float("nan"), float("nan")),
                         n, float("nan"), float(T))

    times = np.sort(times)
    t0 = times[0]
    t = times - t0
    T_eff = float(T) - t0
    if T_eff <= t[-1]:
        T_eff = float(t[-1]) + (t[-1] - t[0]) / max(1, n - 1)  # pad by one mean gap
    dt = np.diff(t)

    mean_gap = float(np.mean(dt)) if dt.size and np.mean(dt) > 0 else 1.0
    mu0 = 0.5 * n / T_eff
    beta0 = 1.0 / mean_gap
    alpha0 = 0.5 * beta0                     # initial branching ratio ~ 0.5
    x0 = np.array([max(mu0, _FLOOR), max(alpha0, _FLOOR), max(beta0, _FLOOR)])
    bounds = [(_FLOOR, None), (_FLOOR, None), (_FLOOR, None)]

    res = minimize(_neg_loglik, x0, args=(dt, t, T_eff), method="L-BFGS-B",
                   bounds=bounds)
    mu, alpha, beta = (float(v) for v in res.x)
    params = HawkesParams(mu, alpha, beta)
    converged = bool(res.success and np.isfinite(params.branching_ratio))
    return HawkesFit(converged, params, n, float(-res.fun), T_eff)
