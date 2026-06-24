#!/usr/bin/env python3
"""Experiment 2 — HFT latency-advantage sweep.

Research question: how do HFT rent extraction and market fragility scale with
the latency advantage HFTs hold over market makers, and is there a critical
threshold (phase transition) above which market quality degrades sharply?

On the verified engine the HFT sees the fundamental value immediately
(``hft_latency_us = 0``); the MM observes and re-posts late. The HFT's latency
advantage is therefore exactly the MM's staleness gap, ``mm_latency_us``, which
is the swept variable here. The Exp 1 operating point (mm_latency_us=3000) sits
on the grid, so this experiment generalizes Exp 1 along a single knob.

Each cell runs BOTH arms at the same latency: a no-HFT baseline and an HFT
treatment. The matched baseline is necessary because a staler MM loses more to
noise/fundamental drift even without HFTs, so clean welfare deltas require a
per-cell control. Every metric is reported as a bootstrap CI across seeds.

Run: bcs_research/.venv/bin/python bcs_research/experiments/exp2_latency_sweep.py
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

from exp1_primary import CFG, _run          # noqa: E402  reuse the Exp 1 harness
from bootstrap import bootstrap_ci_table    # noqa: E402

# HFT latency advantage (= MM staleness gap, µs) with hft_latency_us fixed at 0.
# 0 µs is the control (MM as fast as HFT); 3000 µs is the Exp 1 operating point.
#
# The grid is TICK-ALIGNED (multiples of dt_us = 1000 µs). The engine resolves
# latency only at tick granularity: the MM quotes around fundamental.observe(),
# which returns V on the recording grid, and submits on tick boundaries. Sub-tick
# latencies are therefore not distinct conditions — e.g. 250 µs and 500 µs produce
# byte-identical per-seed results (both = 1 tick stale). Probing finer than one
# tick would require shrinking dt_us, which would change the whole simulation and
# break comparability with Exp 1; see the Discussion/Limitations note. So each
# grid point here is a genuinely distinct staleness in ticks: 0,1,2,3,5,8,13,20.
MM_LATENCY_GRID_US = [0, 1000, 2000, 3000, 5000, 8000, 13000, 20000]

# Per-cell metrics carried straight from a _run() row (plus hft_fill_rate).
CELL_METRICS = (
    "hft_rent", "mm_pnl", "nt_pnl", "zero_sum_residual",
    "liquidity_gaps", "kyle_lambda", "spread_ticks", "n_trades",
    "hft_snipes", "hft_fills", "hft_fill_rate",
)
# Treatment-minus-baseline deltas (welfare transfer + fragility shift).
DELTA_METRICS = ("mm_pnl", "nt_pnl", "liquidity_gaps", "kyle_lambda", "spread_ticks")


def _with_fill_rate(row: dict) -> dict:
    """Return a copy of a _run() row with hft_fill_rate = fills / snipes."""
    snipes = row["hft_snipes"]
    return {**row, "hft_fill_rate": (row["hft_fills"] / snipes) if snipes else 0.0}


def run_cell(mm_latency_us: int, cfg: dict, n_seeds: int, boot_seed: int) -> dict:
    """Run both arms across seeds at one latency and bootstrap every metric."""
    cell_cfg = {**cfg, "mm_latency_us": mm_latency_us}
    seeds = range(1, n_seeds + 1)
    treat = [_with_fill_rate(_run(s, cell_cfg, with_hft=True)) for s in seeds]
    base = [_with_fill_rate(_run(s, cell_cfg, with_hft=False)) for s in seeds]
    deltas = [
        {k: treat[i][k] - base[i][k] for k in DELTA_METRICS}
        for i in range(n_seeds)
    ]
    return {
        "mm_latency_us": mm_latency_us,
        "hft_latency_us": cell_cfg["hft_latency_us"],
        "n_seeds": n_seeds,
        "treatment_ci": bootstrap_ci_table(treat, CELL_METRICS, seed=boot_seed),
        "baseline_ci": bootstrap_ci_table(base, CELL_METRICS, seed=boot_seed),
        "delta_ci": bootstrap_ci_table(deltas, DELTA_METRICS, seed=boot_seed),
        "treatment_rows": treat,
        "baseline_rows": base,
    }


def _print_summary(report: dict) -> None:
    print(
        f"Experiment 2 — latency-advantage sweep ({report['n_seeds']} seeds, "
        f"hft_latency_us=0, n_hft={report['config']['n_hft']})"
    )
    cols = ("mm_lat_us", "hft_rent", "rent_lo", "rent_hi",
            "dMM_pnl", "dNT_pnl", "liq_gaps", "kyle_l", "fill_rt")
    print("".join(f"{c:>11}" for c in cols))
    for cell in report["cells"]:
        rent = cell["treatment_ci"]["hft_rent"]
        d = cell["delta_ci"]
        t = cell["treatment_ci"]
        vals = (
            cell["mm_latency_us"], rent["mean"], rent["lower"], rent["upper"],
            d["mm_pnl"]["mean"], d["nt_pnl"]["mean"],
            t["liquidity_gaps"]["mean"], t["kyle_lambda"]["mean"],
            t["hft_fill_rate"]["mean"],
        )
        print("".join(f"{v:>11.3f}" if isinstance(v, float) else f"{v:>11}"
                       for v in vals))


def main(n_seeds: int = 20, grid: list | None = None,
         cfg: dict | None = None, boot_seed: int = 0) -> dict:
    cfg = {**CFG, **(cfg or {}), "hft_latency_us": 0}
    grid = grid or MM_LATENCY_GRID_US
    cells = [run_cell(L, cfg, n_seeds, boot_seed) for L in grid]
    report = {
        "config": cfg,
        "n_seeds": n_seeds,
        "hft_latency_us": 0,
        "grid_mm_latency_us": grid,
        "cells": cells,
    }

    out_dir = _ROOT / "results" / "experiments"
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "exp2_latency_sweep.json").write_text(
        json.dumps(report, indent=2, sort_keys=True)
    )
    _print_summary(report)
    return report


if __name__ == "__main__":
    main()
