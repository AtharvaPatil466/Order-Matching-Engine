#!/usr/bin/env python3
"""Exp 1 hardening — closes the three pre-registered soft spots.

  1. BOOTSTRAP CIs ON EXP 1 (n=20). Rerun the operating point with 20 seeds and
     bootstrap CIs on every reported metric, replacing the n=10 point estimates.

  2. CALIBRATION ROBUSTNESS (3x3). Sweep sens in {0.05,0.10,0.20} x decay in
     {0.05,0.10,0.20} around the operating point, 20 seeds, both arms. Show the
     primary result (HFT rent significantly > 0; welfare closes to zero) holds in
     every cell, and report the range of HFT rent across the grid.

  3. BASELINE GAP CHARACTERIZATION (100 seeds). Run the no-HFT baseline 100 times,
     report the baseline gap rate with CI, and show the HFT treatment produces
     significantly more gaps than the fragile-but-quiet baseline (non-overlapping
     CIs) — addresses the "slow MM is already a bit fragile" concern honestly.

Everything reuses the verified-engine harness (exp1._run) and the bootstrap
infrastructure; no new mechanism.

Run: bcs_research/.venv/bin/python bcs_research/experiments/exp1_hardening.py
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_ROOT = _HERE.parent
for _d in ("build", "agents", "simulation", "metrics", "experiments"):
    _p = str(_ROOT / _d)
    if _p not in sys.path:
        sys.path.insert(0, _p)

from exp1_primary import CFG, _run               # noqa: E402  verified-engine harness
from bootstrap import bootstrap_ci, bootstrap_ci_table  # noqa: E402

SENS_GRID = [0.05, 0.10, 0.20]
DECAY_GRID = [0.05, 0.10, 0.20]

CELL_METRICS = (
    "hft_rent", "mm_pnl", "nt_pnl", "zero_sum_residual",
    "liquidity_gaps", "kyle_lambda", "spread_ticks", "n_trades",
)
DELTA_METRICS = ("mm_pnl", "nt_pnl", "liquidity_gaps")
ZERO_SUM_TOL = 1e-6   # residual must be machine-zero through the closed market


# --- pure verdict helpers (unit tested) --------------------------------------

def rent_significant(rent_ci: dict) -> bool:
    """HFT rent is significantly positive when its 95% CI lower bound > 0."""
    return rent_ci["lower"] > 0.0


def zero_sum_ok(zerosum_ci: dict, tol: float = ZERO_SUM_TOL) -> bool:
    """Welfare closes to zero when the mean residual is within tolerance."""
    return abs(zerosum_ci["mean"]) < tol


def ci_separated(lower_ci: dict, upper_ci: dict) -> bool:
    """True when lower_ci is strictly below upper_ci (intervals do not overlap)."""
    return lower_ci["upper"] < upper_ci["lower"]


# --- soft spot 1: CI-annotated Exp 1 -----------------------------------------

def harden_exp1(n_seeds: int = 20, cfg: dict | None = None, boot_seed: int = 0) -> dict:
    cfg = {**CFG, **(cfg or {})}
    seeds = range(1, n_seeds + 1)
    treat = [_run(s, cfg, with_hft=True) for s in seeds]
    base = [_run(s, cfg, with_hft=False) for s in seeds]
    deltas = [{k: treat[i][k] - base[i][k] for k in DELTA_METRICS}
              for i in range(n_seeds)]
    treatment_ci = bootstrap_ci_table(treat, CELL_METRICS, seed=boot_seed)
    baseline_ci = bootstrap_ci_table(base, CELL_METRICS, seed=boot_seed)
    delta_ci = bootstrap_ci_table(deltas, DELTA_METRICS, seed=boot_seed)
    return {
        "n_seeds": n_seeds,
        "treatment_ci": treatment_ci,
        "baseline_ci": baseline_ci,
        "delta_ci": delta_ci,
        "treatment_rows": treat,
        "baseline_rows": base,
        "verdict": {
            "rent_significant": rent_significant(treatment_ci["hft_rent"]),
            "zero_sum_ok": zero_sum_ok(treatment_ci["zero_sum_residual"]),
            "gaps_increase": ci_separated(baseline_ci["liquidity_gaps"],
                                          treatment_ci["liquidity_gaps"]),
        },
    }


# --- soft spot 2: 3x3 calibration robustness ---------------------------------

def calibration_sweep(n_seeds: int = 20, sens_grid: list | None = None,
                      decay_grid: list | None = None, boot_seed: int = 0) -> dict:
    sens_grid = sens_grid or SENS_GRID
    decay_grid = decay_grid or DECAY_GRID
    cells = []
    for sens in sens_grid:
        for decay in decay_grid:
            cfg = {**CFG, "adverse_sensitivity": sens, "spread_decay": decay}
            seeds = range(1, n_seeds + 1)
            treat = [_run(s, cfg, with_hft=True) for s in seeds]
            treatment_ci = bootstrap_ci_table(treat, CELL_METRICS, seed=boot_seed)
            cells.append({
                "sens": sens, "decay": decay,
                "treatment_ci": treatment_ci,
                "rent_significant": rent_significant(treatment_ci["hft_rent"]),
                "zero_sum_ok": zero_sum_ok(treatment_ci["zero_sum_residual"]),
            })
    rents = [c["treatment_ci"]["hft_rent"]["mean"] for c in cells]
    return {
        "n_seeds": n_seeds,
        "sens_grid": sens_grid,
        "decay_grid": decay_grid,
        "cells": cells,
        "rent_range": [min(rents), max(rents)],
        "all_rent_significant": all(c["rent_significant"] for c in cells),
        "all_zero_sum_ok": all(c["zero_sum_ok"] for c in cells),
    }


# --- soft spot 3: baseline gap characterization ------------------------------

def baseline_gaps(n_seeds: int = 100, cfg: dict | None = None,
                  boot_seed: int = 0) -> dict:
    cfg = {**CFG, **(cfg or {})}
    rows = [_run(s, cfg, with_hft=False) for s in range(1, n_seeds + 1)]
    gaps = [r["liquidity_gaps"] for r in rows]
    return {
        "n_seeds": n_seeds,
        "gaps_ci": bootstrap_ci(gaps, seed=boot_seed),
    }


# --- driver ------------------------------------------------------------------

def main(n_seeds: int = 20, baseline_seeds: int = 100, boot_seed: int = 0) -> dict:
    exp1 = harden_exp1(n_seeds=n_seeds, boot_seed=boot_seed)
    calib = calibration_sweep(n_seeds=n_seeds, boot_seed=boot_seed)
    base_gaps = baseline_gaps(n_seeds=baseline_seeds, boot_seed=boot_seed)
    base_gaps["treatment_gaps_ci"] = exp1["treatment_ci"]["liquidity_gaps"]
    base_gaps["gaps_significantly_higher"] = ci_separated(
        base_gaps["gaps_ci"], exp1["treatment_ci"]["liquidity_gaps"])

    report = {"operating_point": {k: CFG[k] for k in
                                  ("n_hft", "mm_latency_us", "adverse_sensitivity",
                                   "spread_decay")},
              "exp1_hardened": exp1,
              "calibration": calib,
              "baseline_gaps": base_gaps}

    out_dir = _ROOT / "results" / "experiments"
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "exp1_hardening.json").write_text(json.dumps(report, indent=2, sort_keys=True))

    _print_summary(report)
    return report


def _print_summary(report: dict) -> None:
    e = report["exp1_hardened"]
    t = e["treatment_ci"]
    print(f"Exp 1 hardened ({e['n_seeds']} seeds, bootstrap CIs):")
    for k in ("hft_rent", "mm_pnl", "nt_pnl", "liquidity_gaps", "kyle_lambda",
              "zero_sum_residual"):
        ci = t[k]
        print(f"  {k:<18} {ci['mean']:>9.3f}  [{ci['lower']:>8.3f}, {ci['upper']:>8.3f}]")
    print(f"  verdict: {e['verdict']}")

    c = report["calibration"]
    print(f"\nCalibration 3x3 ({c['n_seeds']} seeds): "
          f"rent_range=[{c['rent_range'][0]:.2f}, {c['rent_range'][1]:.2f}]  "
          f"all_rent_sig={c['all_rent_significant']}  all_zero_sum={c['all_zero_sum_ok']}")
    print(f"  {'sens':>6}{'decay':>7}{'rent':>9}{'rent_lo':>9}{'rent_hi':>9}{'sig':>5}{'zs':>4}")
    for cell in c["cells"]:
        r = cell["treatment_ci"]["hft_rent"]
        print(f"  {cell['sens']:>6}{cell['decay']:>7}{r['mean']:>9.2f}"
              f"{r['lower']:>9.2f}{r['upper']:>9.2f}"
              f"{str(cell['rent_significant']):>5}{str(cell['zero_sum_ok']):>4}")

    b = report["baseline_gaps"]
    bg, tg = b["gaps_ci"], b["treatment_gaps_ci"]
    print(f"\nBaseline gaps ({b['n_seeds']} seeds): "
          f"{bg['mean']:.2f} [{bg['lower']:.2f}, {bg['upper']:.2f}]  vs  "
          f"treatment {tg['mean']:.2f} [{tg['lower']:.2f}, {tg['upper']:.2f}]")
    print(f"  treatment gaps significantly higher (non-overlapping CIs): "
          f"{b['gaps_significantly_higher']}")


if __name__ == "__main__":
    main()
