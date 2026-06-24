"""Nonparametric bootstrap confidence intervals over per-seed metrics.

Shared infrastructure for every experiment from Exp 2 onward: each cell in a
parameter sweep is run across many seeds, and we report the percentile bootstrap
CI of each metric rather than a bare point estimate. Pure functions over plain
sequences / lists of row dicts; no engine dependency.

Determinism: the resampling RNG is seeded explicitly (``seed`` argument) so a
run is reproducible. The point estimate (``mean``) is seed-independent; only the
resampled CI bounds depend on the seed.
"""
from __future__ import annotations

from collections.abc import Mapping, Sequence

import numpy as np

DEFAULT_N_BOOTSTRAP = 2000
DEFAULT_CI = 0.95


def bootstrap_ci(
    values: Sequence[float],
    *,
    n_bootstrap: int = DEFAULT_N_BOOTSTRAP,
    ci: float = DEFAULT_CI,
    seed: int = 0,
) -> dict:
    """Percentile bootstrap CI for the mean of ``values``.

    Returns a JSON-serializable dict: ``mean`` (sample mean), ``lower`` /
    ``upper`` (CI bounds at confidence ``ci``), ``std`` (sample std, ddof=0),
    and ``n`` (sample size). A single value yields a zero-width CI rather than
    an error, which is the sensible degenerate case for a sweep cell.
    """
    arr = np.asarray(values, dtype=float)
    if arr.size == 0:
        raise ValueError("bootstrap_ci requires at least one value")
    if not 0.0 < ci < 1.0:
        raise ValueError(f"ci must be in (0, 1), got {ci}")
    if n_bootstrap <= 0:
        raise ValueError(f"n_bootstrap must be positive, got {n_bootstrap}")

    n = arr.size
    mean = float(arr.mean())
    std = float(arr.std())  # ddof=0, matches numpy default

    if n == 1:
        # Resampling a single observation can only return that observation.
        return {"mean": mean, "lower": mean, "upper": mean, "std": std, "n": n}

    rng = np.random.default_rng(seed)
    # (n_bootstrap, n) resamples in one vectorized draw, then per-row means.
    resamples = rng.choice(arr, size=(n_bootstrap, n), replace=True)
    boot_means = resamples.mean(axis=1)

    alpha = 1.0 - ci
    lower = float(np.percentile(boot_means, 100.0 * (alpha / 2.0)))
    upper = float(np.percentile(boot_means, 100.0 * (1.0 - alpha / 2.0)))
    return {"mean": mean, "lower": lower, "upper": upper, "std": std, "n": n}


def bootstrap_ci_table(
    rows: Sequence[Mapping[str, float]],
    keys: Sequence[str],
    *,
    n_bootstrap: int = DEFAULT_N_BOOTSTRAP,
    ci: float = DEFAULT_CI,
    seed: int = 0,
) -> dict:
    """Build a ``{metric_key: bootstrap_ci(...)}`` table from per-seed rows.

    ``rows`` is a list of per-seed result dicts (as produced by an experiment's
    per-run function); each requested key is extracted into a column and run
    through :func:`bootstrap_ci`. Raises ``ValueError`` on empty ``rows`` and
    ``KeyError`` if a requested key is missing from any row.
    """
    if len(rows) == 0:
        raise ValueError("bootstrap_ci_table requires at least one row")

    table: dict[str, dict] = {}
    for key in keys:
        column = [row[key] for row in rows]  # KeyError if any row lacks key
        table[key] = bootstrap_ci(
            column, n_bootstrap=n_bootstrap, ci=ci, seed=seed
        )
    return table
