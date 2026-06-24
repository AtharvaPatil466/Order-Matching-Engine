"""Unit tests for the univariate (exponential-kernel) Hawkes MLE calibrator.

The calibrator estimates lambda(t) = mu + sum_{t_i<t} alpha*exp(-beta*(t-t_i)) by
maximum likelihood and reports the branching ratio n = alpha/beta (the fraction
of events that are endogenously triggered; n->1 is criticality). Tests use an
Ogata-thinning simulator to generate data with a KNOWN branching ratio so the
recovered n can be checked against ground truth, plus a homogeneous-Poisson
control (n should be near zero) and the basic API/determinism contracts.

Vendored in the BCS repo because the AlphaForge Hawkes module is not yet built
(Phase 1 forthcoming); the API mirrors the plan's intended calibrate_univariate
signature so AlphaForge can adopt it later.
"""
import sys
import pathlib

_R = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(_R / "hawkes"))

import numpy as np
import pytest

from calibrator import calibrate_univariate, HawkesFit


def _simulate_hawkes(mu, alpha, beta, T, seed):
    """Ogata thinning for an exponential-kernel Hawkes process. O(N) via a
    decaying intensity-state variable. Returns sorted event times on [0, T]."""
    rng = np.random.default_rng(seed)
    t = 0.0
    s = 0.0          # sum alpha*exp(-beta*(t - t_i)) for past events, at time t
    events = []
    while t < T:
        lam_bar = mu + s            # intensity just after t (non-increasing until next event)
        if lam_bar <= 0:
            lam_bar = mu
        w = rng.exponential(1.0 / lam_bar)
        t += w
        if t >= T:
            break
        s *= np.exp(-beta * w)      # decay state to the new time
        lam_t = mu + s
        if rng.random() <= lam_t / lam_bar:
            s += alpha              # accept event
            events.append(t)
    return np.array(events)


# --- API / contract ----------------------------------------------------------

def test_fit_returns_hawkesfit_with_branching_ratio():
    times = _simulate_hawkes(mu=1.0, alpha=0.5, beta=1.0, T=500, seed=0)
    fit = calibrate_univariate(times, T=times.max() + 1.0)
    assert isinstance(fit, HawkesFit)
    assert fit.params.branching_ratio == pytest.approx(fit.params.alpha / fit.params.beta)
    assert fit.n_events == len(times)


def test_too_few_events_does_not_converge():
    fit = calibrate_univariate(np.array([1.0, 2.0, 3.0]), T=4.0)
    assert fit.converged is False


def test_empty_input_does_not_converge():
    fit = calibrate_univariate(np.array([]), T=1.0)
    assert fit.converged is False


def test_deterministic_for_same_input():
    times = _simulate_hawkes(mu=1.0, alpha=0.5, beta=1.0, T=400, seed=3)
    a = calibrate_univariate(times, T=times.max() + 1.0)
    b = calibrate_univariate(times, T=times.max() + 1.0)
    assert a.params.branching_ratio == pytest.approx(b.params.branching_ratio)


# --- statistical recovery ----------------------------------------------------

def test_poisson_data_has_low_branching_ratio():
    # Homogeneous Poisson (alpha=0 => no self-excitation): n should be near 0.
    rng = np.random.default_rng(7)
    T = 2000.0
    n_events = rng.poisson(2.0 * T)
    times = np.sort(rng.uniform(0, T, size=n_events))
    fit = calibrate_univariate(times, T=T)
    assert fit.converged is True
    assert fit.params.branching_ratio < 0.4


def test_recovers_known_branching_ratio():
    # Strongly self-exciting: true n = alpha/beta = 0.6. Recovery within a band.
    times = _simulate_hawkes(mu=1.0, alpha=0.6, beta=1.0, T=3000, seed=11)
    assert len(times) > 500
    fit = calibrate_univariate(times, T=3000.0)
    assert fit.converged is True
    assert 0.4 <= fit.params.branching_ratio <= 0.8


def test_self_exciting_exceeds_poisson():
    rng = np.random.default_rng(21)
    T = 2000.0
    pois = np.sort(rng.uniform(0, T, size=rng.poisson(2.0 * T)))
    hawk = _simulate_hawkes(mu=1.0, alpha=0.6, beta=1.0, T=T, seed=22)
    n_pois = calibrate_univariate(pois, T=T).params.branching_ratio
    n_hawk = calibrate_univariate(hawk, T=T).params.branching_ratio
    assert n_hawk > n_pois
