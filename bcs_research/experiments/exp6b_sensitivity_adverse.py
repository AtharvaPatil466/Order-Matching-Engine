#!/usr/bin/env python3
"""Sensitivity of the §4.7 incidence boundary to `adverse_sensitivity`.

Why this exists. §4.7 maps the incidence boundary at a single value of
`adverse_sensitivity` (0.015, re-calibrated for calibrated flow in §3.7), and
the inversion runs entirely through the maker's widening response — which that
one parameter governs. Paper §5.2 flagged the boundary's dependence on it as
unmapped. This driver is the smallest check that answers whether the inversion
is an artifact of that tuning: four values spanning roughly 0.5x to 4x the
calibrated one, three readings each.

This is a SENSITIVITY CHECK, NOT A SURFACE. It runs n=20 seeds against §4.7's
n=100, so bootstrap intervals are materially wider and cells that §4.7 resolves
may read `indeterminate` here from lost power alone. Every cell therefore
reports the point-estimate sign alongside the CI classification, so a boundary
that moves because the sign moved can be told apart from one that moves because
the interval widened. Do not quote a boundary location from this file without
that distinction.

Reuses exp6's cell/classification machinery unchanged so the two are comparable
by construction rather than by inspection.

Run: bcs_research/.venv/bin/python bcs_research/experiments/exp6b_sensitivity_adverse.py [n_seeds]
"""
from __future__ import annotations

import json
import statistics
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_ROOT = _HERE.parent
for _d in ("build", "agents", "simulation", "metrics", "experiments"):
    _p = str(_ROOT / _d)
    if _p not in sys.path:
        sys.path.insert(0, _p)

from exp1_primary import CFG                                      # noqa: E402
from exp6_incidence_surface import (                              # noqa: E402
    K_GRID, baseline_rows, classify_incidence, find_boundary, run_cell,
)
from run_calibrated import calibrated_cfg                         # noqa: E402

# 0.015 is the calibrated value (§3.7). The others span 0.53x to 4x it; the
# claim the paper may draw from this file is about the 0.008-0.030 range, and
# 0.060 is carried as the value at which the §3.7 calibration criterion itself
# is expected to fail, which is a finding rather than a data point.
SENSITIVITY_GRID = (0.008, 0.015, 0.030, 0.060)

# Reading 1: the band step 3b measured on the tape (0.0049 and 0.0097 against
# the 2061-unit calibrated quote). Reading 2: the row where §4.7 puts the flip
# at k=7. Reading 3: the main calibrated arm.
TAPE_QTYS = (10, 20)
FLIP_QTY = 50
RENT_QTY = 2061
RENT_K = 3

# base spread = 2 * base_half_spread, in ticks; used to report the no-HFT spread
# multiple that §3.7's calibration criterion is stated in.
BASE_SPREAD_TICKS = 2 * CFG["base_half_spread"]


def _mean(xs) -> float:
    return float(statistics.fmean(xs)) if xs else 0.0


def _cell_summary(cell: dict) -> dict:
    """CI classification plus the raw point estimate, kept side by side.

    At n=20 these disagree often, and the disagreement is the whole reason this
    file reports both: `indeterminate` with a positive mean is lost power,
    `indeterminate` with a negative mean is a boundary that actually moved.
    """
    mm = cell["delta_ci"]["mm_pnl"]
    return {
        "k": cell["n_hfts"],
        "class": classify_incidence(cell),
        "mm_delta_mean": mm["mean"],
        "mm_delta_lower": mm["lower"],
        "mm_delta_upper": mm["upper"],
        "point_sign": "gains" if mm["mean"] > 0 else "pays",
        "nt_delta_mean": cell["delta_ci"]["nt_pnl"]["mean"],
        "hft_rent_mean": cell["treatment_ci"]["hft_rent"]["mean"],
        "hft_rent_lower": cell["treatment_ci"]["hft_rent"]["lower"],
        "hft_rent_upper": cell["treatment_ci"]["hft_rent"]["upper"],
        "zero_sum_residual": cell["treatment_ci"]["zero_sum_residual"]["mean"],
    }


def _row(qty: int, k_grid, cfg: dict, base: list[dict], n_seeds: int) -> dict:
    cells = [run_cell(qty, k, cfg, base, n_seeds) for k in k_grid]
    summaries = [_cell_summary(c) for c in cells]
    for s in summaries:
        print(f"      k={s['k']:>3}  dMM={s['mm_delta_mean']:>12.1f} "
              f"[{s['mm_delta_lower']:>11.1f},{s['mm_delta_upper']:>11.1f}]  "
              f"{s['class']:<14} point:{s['point_sign']}", flush=True)
    return {
        "hft_qty": qty,
        "snipe_quote_ratio": qty / cfg["quote_qty"],
        "cells": summaries,
        "boundary": find_boundary(cells),
    }


def run_one(sensitivity: float, n_seeds: int) -> dict:
    """All three readings at one `adverse_sensitivity` value."""
    cfg = {**CFG, **calibrated_cfg(), "adverse_sensitivity": sensitivity}
    print(f"\n{'=' * 78}\nadverse_sensitivity = {sensitivity}  "
          f"({sensitivity / 0.015:.2f}x calibrated), n_seeds={n_seeds}\n{'=' * 78}",
          flush=True)

    base = baseline_rows(cfg, n_seeds)
    base_spread = _mean([r["spread_ticks"] for r in base])
    print(f"  no-HFT spread {base_spread:.1f} ticks = "
          f"{base_spread / BASE_SPREAD_TICKS:.2f}x base "
          f"(3.7 calibration criterion: ~1.1x)", flush=True)

    tape_rows = []
    for qty in TAPE_QTYS:
        print(f"    ratio {qty / cfg['quote_qty']:.4f} (tape band)", flush=True)
        tape_rows.append(_row(qty, K_GRID, cfg, base, n_seeds))

    print(f"    ratio {FLIP_QTY / cfg['quote_qty']:.4f} (4.7 flips at k=7)",
          flush=True)
    flip_row = _row(FLIP_QTY, K_GRID, cfg, base, n_seeds)

    print(f"    ratio {RENT_QTY / cfg['quote_qty']:.4f}, k={RENT_K} (rent)",
          flush=True)
    rent_row = _row(RENT_QTY, (RENT_K,), cfg, base, n_seeds)

    return {
        "adverse_sensitivity": sensitivity,
        "no_hft_spread_ticks": base_spread,
        "no_hft_spread_multiple": base_spread / BASE_SPREAD_TICKS,
        "baseline_liquidity_gaps": _mean([r["liquidity_gaps"] for r in base]),
        "tape_band_rows": tape_rows,
        "flip_row": flip_row,
        "rent_cell": rent_row["cells"][0],
    }


def main(n_seeds: int = 20, grid=SENSITIVITY_GRID,
         out_name: str = "exp6b_sensitivity_adverse.json") -> dict:
    report = {
        "n_seeds": n_seeds,
        "grid_adverse_sensitivity": list(grid),
        "k_grid": list(K_GRID),
        "calibrated_value": 0.015,
        "results": [run_one(s, n_seeds) for s in grid],
    }
    out_dir = _ROOT / "results" / "experiments"
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / out_name).write_text(json.dumps(report, indent=2, sort_keys=True))
    print(f"\nwrote results/experiments/{out_name}")
    return report


if __name__ == "__main__":
    main(n_seeds=int(sys.argv[1]) if len(sys.argv) > 1 else 20)
