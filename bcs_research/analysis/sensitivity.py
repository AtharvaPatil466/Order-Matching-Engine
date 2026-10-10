"""Global sensitivity and prior machinery for Experiments 13 and 14.

Why this exists. A reviewer asked for two things the per-cell bootstrap CIs
cannot give: (1) which hand-set parameters drive incidence when ALL of them
move at once (variance-based, Sobol, indices), and (2) incidence reported as a
distribution over priors instead of at point values. Bootstrap CIs on a cell
shrink with n however wrong the point is; a Sobol index or a prior-weighted
distribution does not.

Contents, each small and tested on a case with a closed form:
  Param, saltelli_design, ab_matrix, values
      Saltelli (2010) sampling: two independent matrices A, B from one
      scrambled Sobol' sequence of dimension 2d (Saltelli et al. 2010, §5;
      scipy's own sample_A_B does the same), and AB_i = A with column i from B.
      A run's seed is a factor like any other ("seed" kind), so A and AB_i
      share it (common random numbers) unless i IS the seed, and the seed's
      own index measures how much of the output variance is run noise.
  sobol_indices, sobol_with_ci
      First order by Saltelli (2010) Table 2(b), total order by Jansen (1999),
      Table 2(f): the estimators scipy.stats.sobol_indices uses (the tests
      check equality). Re-implemented, not called, because scipy's bootstrap
      takes no seed (BCa on the global RNG), and §2 of the pre-registration
      requires a seeded 2,000-resample percentile bootstrap.
  quantile_prior
      An inverse CDF through published quantiles, for the tape's race-size /
      depth distribution, which race_proxy_v2.json stores as p10..p99 only.
  grid_indices
      Exact first-order variance shares of a function tabulated on a product
      grid of independent discrete factors (Experiment 14's prior grid).

ponytail: the bootstrap resamples the rows of a QMC design as if i.i.d.; that
ignores the Sobol' sequence's balance and so is conservative (wider than the
true sampling error). Randomised-QMC replicates are the upgrade if CIs bind.
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from scipy.stats import qmc

KINDS = ("uniform", "loguniform", "integer", "seed")
SEED_SPAN = 2 ** 31 - 1


@dataclass(frozen=True)
class Param:
    """One input factor: U(0,1) is mapped onto [lo, hi] by `kind`.

    uniform     lo + u (hi - lo)
    loguniform  lo (hi / lo)^u
    integer     uniform on {lo, ..., hi}
    seed        an integer run seed in [0, 2^31 - 1); lo/hi are ignored
    """
    name: str
    lo: float = 0.0
    hi: float = 1.0
    kind: str = "uniform"

    def __post_init__(self):
        if self.kind not in KINDS:
            raise ValueError(f"kind must be one of {KINDS}, got {self.kind!r}")
        if self.kind != "seed" and not self.lo <= self.hi:
            raise ValueError(f"{self.name}: need lo <= hi")
        if self.kind == "loguniform" and self.lo <= 0:
            raise ValueError(f"{self.name}: loguniform needs lo > 0")

    def value(self, u: float):
        if self.kind == "uniform":
            return self.lo + u * (self.hi - self.lo)
        if self.kind == "loguniform":
            return self.lo * (self.hi / self.lo) ** u
        if self.kind == "integer":
            return int(min(self.hi, self.lo + np.floor(u * (self.hi - self.lo + 1))))
        return int(u * SEED_SPAN)


def saltelli_design(d: int, n: int, seed: int) -> tuple[np.ndarray, np.ndarray]:
    """A, B: (n, d) unit-cube matrices from one scrambled Sobol' sequence in 2d."""
    if n < 2 or n & (n - 1):
        raise ValueError("n must be a power of 2 (Sobol' balance)")
    u = qmc.Sobol(d=2 * d, scramble=True, seed=seed).random(n)
    return u[:, :d], u[:, d:]


def ab_matrix(A: np.ndarray, B: np.ndarray, i: int) -> np.ndarray:
    """A with column i taken from B."""
    out = A.copy()
    out[:, i] = B[:, i]
    return out


def values(params, U: np.ndarray) -> list[dict]:
    """Each row of a unit matrix as {param name: value}."""
    return [{p.name: p.value(u) for p, u in zip(params, row)} for row in U]


def _indices(f_A, f_B, f_AB):
    """(S1, ST) per factor. f_A, f_B: (..., n); f_AB: (d, ..., n)."""
    mean = np.mean([f_A, f_B], axis=(0, -1), keepdims=True)[0]
    a, b, ab = f_A - mean, f_B - mean, f_AB - mean
    var = np.var(np.stack([a, b]), axis=(0, -1))
    with np.errstate(divide="ignore", invalid="ignore"):
        s1 = np.mean(b * (ab - a), axis=-1) / var
        st = 0.5 * np.mean((a - ab) ** 2, axis=-1) / var
    return np.nan_to_num(s1), np.nan_to_num(st)


def sobol_indices(f_A, f_B, f_AB) -> tuple[np.ndarray, np.ndarray]:
    """First-order (Saltelli 2010) and total-order (Jansen 1999) indices.

    f_A, f_B: (n,) outputs at A and B; f_AB: (d, n), row i at AB_i.
    A constant output has zero indices (scipy's convention).
    """
    return _indices(np.asarray(f_A, float), np.asarray(f_B, float), np.asarray(f_AB, float))


def sobol_with_ci(f_A, f_B, f_AB, names, n_boot: int = 2000, ci: float = 0.95,
                  seed: int = 0) -> dict:
    """{name: {S1, S1_lo, S1_hi, ST, ST_lo, ST_hi}} plus the output variance.

    Percentile bootstrap over design rows, `n_boot` resamples, seeded.
    """
    f_A, f_B, f_AB = (np.asarray(x, float) for x in (f_A, f_B, f_AB))
    n = f_A.size
    s1, st = _indices(f_A, f_B, f_AB)
    idx = np.random.default_rng(seed).integers(0, n, size=(n_boot, n))
    # Blocks of ~200 resamples keep the (d, block, n) arrays small.
    parts = [_indices(f_A[c], f_B[c], f_AB[:, c]) for c in np.array_split(idx, -(-n_boot // 200))]
    b1 = np.concatenate([p[0] for p in parts], axis=-1)       # (d, n_boot)
    bt = np.concatenate([p[1] for p in parts], axis=-1)
    q = 100 * np.array([(1 - ci) / 2, (1 + ci) / 2])
    lo1, hi1 = np.percentile(b1, q, axis=-1)
    lot, hit = np.percentile(bt, q, axis=-1)
    table = {name: {"S1": float(s1[i]), "S1_lo": float(lo1[i]), "S1_hi": float(hi1[i]),
                    "ST": float(st[i]), "ST_lo": float(lot[i]), "ST_hi": float(hit[i])}
             for i, name in enumerate(names)}
    return {"indices": table, "variance": float(np.var(np.concatenate([f_A, f_B]))),
            "n": n, "n_boot": n_boot, "ci": ci, "boot_seed": seed}


def quantile_prior(probs, quantiles):
    """Inverse CDF u -> x through (probs[i], quantiles[i]), linear in log x.

    Between the published quantiles the CDF is interpolated in log x; beyond
    the outermost ones the end segments are extended (the tape's ratios span
    four decades, so a linear-in-x tail would be meaningless). Quantiles must
    be positive and increasing.
    """
    p = np.asarray(probs, float)
    lx = np.log(np.asarray(quantiles, float))
    if p.size < 2 or np.any(np.diff(p) <= 0) or np.any(np.diff(lx) <= 0):
        raise ValueError("need >= 2 strictly increasing probabilities and quantiles")
    lo_slope = (lx[1] - lx[0]) / (p[1] - p[0])
    hi_slope = (lx[-1] - lx[-2]) / (p[-1] - p[-2])
    knots_p = np.r_[0.0, p, 1.0]
    knots_x = np.r_[lx[0] - lo_slope * p[0], lx, lx[-1] + hi_slope * (1 - p[-1])]

    def ppf(u):
        return np.exp(np.interp(u, knots_p, knots_x))
    return ppf


def grid_indices(Y: np.ndarray, weights) -> dict:
    """First-order variance shares of Y tabulated on a product grid.

    Y has one axis per independent discrete factor; weights[k] are factor k's
    probabilities over its axis. Returns the variance of Y under the product
    measure and, per axis, Var(E[Y | factor k]) / Var(Y) (exact, no sampling).
    1 - sum of the shares is the interaction share.
    """
    Y = np.asarray(Y, float)
    if Y.ndim != len(weights):
        raise ValueError("one weight vector per axis of Y")
    w = [np.asarray(x, float) / np.sum(x) for x in weights]
    if any(len(x) != m for x, m in zip(w, Y.shape)):
        raise ValueError("weight lengths must match Y's shape")
    joint = w[0]
    for x in w[1:]:
        joint = np.multiply.outer(joint, x)
    mean = np.sum(joint * Y)
    var = float(np.sum(joint * (Y - mean) ** 2))
    shares = []
    for k in range(Y.ndim):
        other = tuple(j for j in range(Y.ndim) if j != k)
        cond = np.sum(joint * Y, axis=other) / w[k]          # E[Y | factor k]
        shares.append(float(np.sum(w[k] * (cond - mean) ** 2) / var) if var > 0 else 0.0)
    return {"mean": float(mean), "variance": var, "first_order": shares,
            "interaction": 1.0 - sum(shares) if var > 0 else 0.0}
