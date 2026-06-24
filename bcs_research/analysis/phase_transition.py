"""Phase-transition detection: linear vs continuous two-segment (hinge) fit.

Exp 2 sweeps the HFT latency advantage and asks whether HFT rent rises smoothly
(one linear trend) or undergoes a threshold above which it climbs steeply. We
fit two least-squares models to the per-seed (latency, rent) points:

  linear:    y = a + b * x
  segmented: y = a + b1 * x + b2 * max(0, x - c)   (continuous, knee at c)

and compare them by BIC. The segmented model has a free breakpoint c found by a
grid search that minimizes residual sum of squares. A large positive
``delta_bic`` (BIC_linear - BIC_segmented) is evidence the relationship has a
knee — i.e. a phase transition — and the recovered ``c`` is the critical
threshold. Pure numpy; no scipy dependency.
"""
from __future__ import annotations

import numpy as np

# ΔBIC > 10 is conventionally "very strong" evidence for the better model.
STRONG_BIC = 10.0


def _bic(rss: float, n: int, k: int) -> float:
    """Gaussian BIC up to an additive constant common to both models.

    k counts free regression parameters; the +1 for the noise variance is
    common to both models and cancels in ΔBIC, so it is omitted for simplicity.
    A guard keeps log(0) finite for (near-)perfect fits.
    """
    rss = max(float(rss), 1e-12)
    return n * np.log(rss / n) + k * np.log(n)


def _validate(x, y, min_points: int) -> tuple:
    x = np.asarray(x, dtype=float).ravel()
    y = np.asarray(y, dtype=float).ravel()
    if x.size != y.size:
        raise ValueError(f"x and y must be the same length, got {x.size} != {y.size}")
    if x.size < min_points:
        raise ValueError(f"need at least {min_points} points, got {x.size}")
    if np.ptp(x) == 0:
        raise ValueError("x has no variation; cannot fit a trend")
    return x, y


def fit_linear(x, y) -> dict:
    """Ordinary least-squares line y = a + b*x."""
    x, y = _validate(x, y, min_points=2)
    design = np.column_stack([np.ones_like(x), x])
    coef, *_ = np.linalg.lstsq(design, y, rcond=None)
    resid = y - design @ coef
    rss = float(resid @ resid)
    n = x.size
    return {"a": float(coef[0]), "b": float(coef[1]), "rss": rss,
            "n": n, "k": 2, "bic": _bic(rss, n, 2)}


def _fit_hinge_at(x: np.ndarray, y: np.ndarray, c: float) -> tuple:
    """Least-squares continuous two-segment fit with a fixed knee at c."""
    design = np.column_stack([np.ones_like(x), x, np.maximum(0.0, x - c)])
    coef, *_ = np.linalg.lstsq(design, y, rcond=None)
    resid = y - design @ coef
    return float(resid @ resid), coef


def fit_segmented(x, y, breakpoints=None) -> dict:
    """Continuous two-segment fit; breakpoint chosen to minimize RSS.

    ``breakpoints`` is the candidate knee grid; by default a 50-point grid spanning
    the interior of x (endpoints excluded, where a segment would be empty).
    """
    x, y = _validate(x, y, min_points=4)
    if breakpoints is None:
        lo, hi = np.min(x), np.max(x)
        span = hi - lo
        grid = np.linspace(lo + 0.05 * span, hi - 0.05 * span, 50)
        # Include the observed interior x-values: a knee often sits exactly at a
        # tested point (for Exp 2, at one of the swept latencies).
        xs = np.unique(x)
        interior_x = xs[(xs > lo) & (xs < hi)]
        breakpoints = np.concatenate([grid, interior_x])
    breakpoints = np.asarray(breakpoints, dtype=float).ravel()

    best = None
    for c in breakpoints:
        rss, coef = _fit_hinge_at(x, y, c)
        if best is None or rss < best[0]:
            best = (rss, coef, c)
    rss, coef, c = best
    n = x.size
    return {"a": float(coef[0]), "b1": float(coef[1]), "b2": float(coef[2]),
            "breakpoint": float(c), "rss": float(rss),
            "n": n, "k": 4, "bic": _bic(rss, n, 4)}


def detect_phase_transition(x, y, breakpoints=None) -> dict:
    """Compare linear vs segmented fits; positive delta_bic favors segmented."""
    linear = fit_linear(x, y)
    segmented = fit_segmented(x, y, breakpoints=breakpoints)
    delta_bic = linear["bic"] - segmented["bic"]
    return {
        "linear": linear,
        "segmented": segmented,
        "delta_bic": float(delta_bic),
        "prefers_segmented": bool(delta_bic > 0.0),
        "strong_evidence": bool(delta_bic > STRONG_BIC),
        "breakpoint": segmented["breakpoint"],
    }
