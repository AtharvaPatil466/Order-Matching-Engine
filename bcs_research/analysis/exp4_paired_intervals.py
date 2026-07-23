#!/usr/bin/env python3
"""Matched-seed differencing across batch clearing intervals (paper §4.4).

§4.4 concedes that long-interval batch rent is unmeasurable on the 50,000-tick
grid: per-seed dispersion swamps the point estimate. The obvious remedy is to
exploit the fact that every interval is run over the same seed range and the
fundamental process holds a private generator (agents/fundamental_value.py:30,
drawn unconditionally per tick), so the fundamental path is bit-identical
across arms for a given seed. Differencing against the continuous arm should
then cancel the volatility term.

It does not, and this script quantifies why:

  * The LEVEL cannot improve. mean(c) + mean(x - c) == mean(x) identically, so
    the decomposition reassembles into the original statistic for any anchor.
    Reported here as a reconstruction check that must agree to floating point.
  * The variance barely moves (1055 -> 1028, 2.6%) because the dispersion is
    arm-specific, not common-mode: sd(continuous) is ~16x smaller than
    sd(interval-500), so the cross term in Var(x-c) is negligible.
  * The CONTRAST is nonetheless well estimated, and that is what §4.4 now
    reports: intervals 5/10/25 sit significantly below continuous, while
    interval 1 is indistinguishable from it.

Run: bcs_research/.venv/bin/python bcs_research/analysis/exp4_paired_intervals.py
"""
from __future__ import annotations

import json
from pathlib import Path

import numpy as np

_HERE = Path(__file__).resolve().parent
_ROOT = _HERE.parent
EXP = _ROOT / "results" / "experiments"

SOURCE = "exp4_batch_long.json"      # the 50k-tick grid, where the concession lives
OUT = "exp4_paired_intervals.json"
CONTINUOUS = "continuous"
BOOT_DRAWS = 10_000
BOOT_SEED = 0


def _bootstrap_ci(x: np.ndarray, rng, draws: int = BOOT_DRAWS) -> dict:
    idx = rng.integers(0, len(x), size=(draws, len(x)))
    means = x[idx].mean(axis=1)
    return {
        "mean": float(x.mean()),
        "lower": float(np.percentile(means, 2.5)),
        "upper": float(np.percentile(means, 97.5)),
        "se": float(x.std(ddof=1) / np.sqrt(len(x))),
        "n": int(len(x)),
    }


def main() -> dict:
    report = json.loads((EXP / SOURCE).read_text())
    rent = {c["batch_interval"]: np.asarray([r["hft_rent"] for r in c["treatment_rows"]],
                                            dtype=float)
            for c in report["cells"]}
    cont = rent[CONTINUOUS]
    rng = np.random.default_rng(BOOT_SEED)

    cells = []
    for interval in report["intervals"]:
        if interval == CONTINUOUS:
            continue
        x = rent[interval]
        cells.append({
            "batch_interval": interval,
            "level": _bootstrap_ci(x, rng),
            "paired_delta_vs_continuous": _bootstrap_ci(x - cont, rng),
            "seed_correlation_with_continuous": float(np.corrcoef(x, cont)[0, 1]),
            # Must equal level["mean"] to floating point: differencing cannot
            # move the level, only the contrast.
            "reconstruction": float(cont.mean() + (x - cont).mean()),
        })

    out = {
        "source": SOURCE,
        "n_seeds": report["n_seeds"],
        "ticks": report.get("ticks"),
        "continuous": _bootstrap_ci(cont, rng),
        "cells": cells,
        "bootstrap_draws": BOOT_DRAWS,
        "bootstrap_seed": BOOT_SEED,
    }
    (EXP / OUT).write_text(json.dumps(out, indent=2, sort_keys=True))

    print(f"Matched-seed interval differencing — {SOURCE}, {out['n_seeds']} seeds, "
          f"{out['ticks']} ticks")
    print(f"{'interval':>10}{'level':>26}{'se':>9}{'  Δ vs continuous':>26}{'se':>9}{'corr':>7}")
    for c in cells:
        lv, dl = c["level"], c["paired_delta_vs_continuous"]
        assert abs(c["reconstruction"] - lv["mean"]) < 1e-9, "reconstruction identity broken"
        print(f"{c['batch_interval']:>10}"
              f"{lv['mean']:>10.1f} [{lv['lower']:>7.1f},{lv['upper']:>7.1f}]{lv['se']:>9.1f}"
              f"{dl['mean']:>10.1f} [{dl['lower']:>7.1f},{dl['upper']:>7.1f}]{dl['se']:>9.1f}"
              f"{c['seed_correlation_with_continuous']:>7.2f}")
    cc = out["continuous"]
    print(f"{'continuous':>10}{cc['mean']:>10.1f} [{cc['lower']:>7.1f},{cc['upper']:>7.1f}]"
          f"{cc['se']:>9.1f}")
    print(f"\nwrote results/experiments/{OUT}")
    return out


if __name__ == "__main__":
    main()
