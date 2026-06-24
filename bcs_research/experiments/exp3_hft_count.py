#!/usr/bin/env python3
"""Experiment 3 — HFT count sweep (the socially-wasteful arms race).

Research question: as more HFTs compete, does each individual HFT earn less per
race won, even as total welfare loss to market makers and noise traders rises?
BCS predicts both — the arms race is bad for society and for the individual HFT,
yet no HFT has an incentive to exit. The signature is per-HFT rent DOWN while
total welfare loss UP.

Operating point = Exp 1 / Exp 2 (mm_latency_us=3000, adverse_sensitivity=0.1,
spread_decay=0.1, hft_latency_us=0); the only swept variable is n_hfts.

The n_hfts=0 cell is the shared no-HFT baseline. Per seed, a treatment run reuses
the identical fundamental path and noise-trader realization (seeds derive from
`seed` independently of the HFTs), so welfare deltas are cleanly matched: same
exogenous shocks, HFTs added. Every metric is reported as a bootstrap CI.

Run: bcs_research/.venv/bin/python bcs_research/experiments/exp3_hft_count.py
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

from exp1_primary import CFG, _run               # noqa: E402  reuse Exp 1 harness
from exp2_latency_sweep import _with_fill_rate    # noqa: E402  shared row helper
from bootstrap import bootstrap_ci_table          # noqa: E402

N_HFTS_GRID = [0, 1, 2, 3, 5, 8, 13, 21]

CELL_METRICS = (
    "hft_rent", "hft_rent_per_hft", "mm_pnl", "nt_pnl", "zero_sum_residual",
    "liquidity_gaps", "kyle_lambda", "spread_ticks", "n_trades",
    "hft_snipes", "hft_fills", "hft_fill_rate",
)
DELTA_METRICS = ("mm_pnl", "nt_pnl", "total_welfare_loss", "liquidity_gaps")


def _augment(row: dict, n_hfts: int) -> dict:
    """Add hft_fill_rate and per-HFT rent (0 in the no-HFT control)."""
    row = _with_fill_rate(row)
    row["hft_rent_per_hft"] = (row["hft_rent"] / n_hfts) if n_hfts else 0.0
    return row


def _baseline_rows(cfg: dict, n_seeds: int) -> list:
    base_cfg = {**cfg, "n_hft": 0}
    return [_augment(_run(s, base_cfg, with_hft=False), 0)
            for s in range(1, n_seeds + 1)]


def run_cell(n_hfts: int, cfg: dict, baseline_rows: list,
             n_seeds: int, boot_seed: int) -> dict:
    """Run the k-HFT treatment, delta against the shared baseline, bootstrap."""
    if n_hfts == 0:
        treat = baseline_rows
    else:
        cell_cfg = {**cfg, "n_hft": n_hfts}
        treat = [_augment(_run(s, cell_cfg, with_hft=True), n_hfts)
                 for s in range(1, n_seeds + 1)]

    deltas = []
    for i in range(n_seeds):
        d_mm = treat[i]["mm_pnl"] - baseline_rows[i]["mm_pnl"]
        d_nt = treat[i]["nt_pnl"] - baseline_rows[i]["nt_pnl"]
        deltas.append({
            "mm_pnl": d_mm,
            "nt_pnl": d_nt,
            "total_welfare_loss": -(d_mm + d_nt),  # rent extracted from MM+NT
            "liquidity_gaps": treat[i]["liquidity_gaps"] - baseline_rows[i]["liquidity_gaps"],
        })
    return {
        "n_hfts": n_hfts,
        "n_seeds": n_seeds,
        "treatment_ci": bootstrap_ci_table(treat, CELL_METRICS, seed=boot_seed),
        "delta_ci": bootstrap_ci_table(deltas, DELTA_METRICS, seed=boot_seed),
        "treatment_rows": treat,
    }


def _print_summary(report: dict) -> None:
    print(
        f"Experiment 3 — HFT count sweep ({report['n_seeds']} seeds, "
        f"mm_latency_us={report['config']['mm_latency_us']})"
    )
    cols = ("n_hfts", "tot_rent", "rent/hft", "welf_loss", "dMM_pnl",
            "dNT_pnl", "liq_gaps", "fill_rt")
    print("".join(f"{c:>11}" for c in cols))
    for cell in report["cells"]:
        t, d = cell["treatment_ci"], cell["delta_ci"]
        vals = (
            cell["n_hfts"], t["hft_rent"]["mean"], t["hft_rent_per_hft"]["mean"],
            d["total_welfare_loss"]["mean"], d["mm_pnl"]["mean"],
            d["nt_pnl"]["mean"], t["liquidity_gaps"]["mean"],
            t["hft_fill_rate"]["mean"],
        )
        print("".join(f"{v:>11.3f}" if isinstance(v, float) else f"{v:>11}"
                       for v in vals))


def main(n_seeds: int = 20, grid: list | None = None,
         cfg: dict | None = None, boot_seed: int = 0) -> dict:
    cfg = {**CFG, **(cfg or {})}
    grid = grid or N_HFTS_GRID
    baseline_rows = _baseline_rows(cfg, n_seeds)
    cells = [run_cell(k, cfg, baseline_rows, n_seeds, boot_seed) for k in grid]
    report = {
        "config": cfg,
        "n_seeds": n_seeds,
        "grid_n_hfts": grid,
        "cells": cells,
    }

    out_dir = _ROOT / "results" / "experiments"
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "exp3_hft_count.json").write_text(
        json.dumps(report, indent=2, sort_keys=True)
    )
    _print_summary(report)
    return report


if __name__ == "__main__":
    main()
