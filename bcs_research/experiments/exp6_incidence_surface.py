#!/usr/bin/env python3
"""Research step 5 — the (snipe-to-quote ratio, HFT count) incidence surface.

Named exp6 rather than exp5 deliberately: "Experiment 5" is already the Hawkes
endogeneity diagnostic of paper §4.5 (`analysis/exp5_redesigned.py`). This is
step 5 of RESEARCH_PLAN.md, which is a different thing.

Why this exists. §4.1 states "HFT rent is extracted from the market maker" and
§4.6 shows that claim is conditional: at the calibrated main arm's ratio of 1.0
the maker bears the transfer, but with `hft_qty` held at 50 against a 2061-unit
quote (ratio 0.024) the maker's PnL delta turns POSITIVE for k <= 5 and only
goes negative by k >= 8, with noise traders bearing the loss instead. Two ratio
points and one k-sweep do not locate a boundary in a two-dimensional space.
This driver maps the surface and reports where the incidence flips.

What is new here relative to BCS and to Aquilina, Budish & O'Neill (2022).
Both establish that the rent exists and size it. Neither identifies WHO pays it
as a function of market structure. The incidence is governed by the
snipe-to-quote size ratio, which no public feed pins (§3.7) — Binance's depth
stream publishes quantity per level with no order counts, and the tape
under-measures multi-level sweeps exactly where they matter (step 3b). So the
deliverable is a mapped regime boundary over a swept design parameter, with the
band step 3b measured on the real tape marked on it, not a point estimate.

Prerequisite satisfied. Step 4 (competing makers) had to land first: the
maker's gain at small snipe sizes could itself have been a single-maker artifact
— a sole liquidity supplier monetizing flow its own widening created. Step 4
measured and removed that absorption bias, so a boundary mapped now is not an
artifact of the thing step 4 existed to remove. This surface holds maker count
at 1 to nest §4.6's published cells exactly; the competing-maker interaction is
a separate axis, left to future work.

Run: bcs_research/.venv/bin/python bcs_research/experiments/exp6_incidence_surface.py [n_seeds]
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

from bootstrap import bootstrap_ci_table       # noqa: E402
from exp1_primary import CFG, _run             # noqa: E402
from exp2_latency_sweep import _with_fill_rate  # noqa: E402
from run_calibrated import calibrated_cfg      # noqa: E402

# Swept as integer snipe QUANTITIES, not as ratios, so the two load-bearing
# cells nest §4.6 exactly rather than approximately: 50 is its robustness arm
# and 2061 (== the calibrated quote) is its main arm. The realized ratio is
# derived and reported per cell. The dense low end covers the band step 3b
# measured on the real tape (p50 0.0072 unconditional, 0.0283 under the race
# proxy); the middle spans the region where §4.6 implies the maker's sign must
# change somewhere between 0.024 and 1.0.
QTY_GRID = (10, 20, 50, 100, 250, 700, 2061)

# A superset of Exp 3's grid minus its k=0 control (which is the shared baseline
# below), so the ratio-1.0 column reproduces exp3_hft_count_calibrated.json
# rather than re-specifying it. 6 and 7 are the additions: §4.6 locates the
# incidence flip only as "gains at k<=5, loses by k>=8", and the boundary is this
# experiment's deliverable, so the one interval that matters gets unit
# resolution instead of a three-wide bracket.
K_GRID = (1, 2, 3, 5, 6, 7, 8, 13, 21)

CELL_METRICS = (
    "hft_rent", "hft_rent_per_hft", "hft_rent_bp", "mm_pnl", "nt_pnl",
    "zero_sum_residual", "liquidity_gaps", "spread_ticks", "n_trades",
    "hft_snipes", "hft_fills", "hft_fill_rate",
)
DELTA_METRICS = ("mm_pnl", "nt_pnl", "total_welfare_loss", "liquidity_gaps")


def _augment(row: dict, n_hfts: int) -> dict:
    """Add fill rate, per-HFT rent, and rent normalised by traded notional.

    Returns a new dict; the engine row is not mutated. `hft_rent_bp` is the
    quantity §4.6 compares against the ~0.4 bp latency-arbitrage tax that
    Aquilina, Budish & O'Neill (2022) measure on exchange message data — rent as
    a fraction of traded notional, NOT per-maker rent.
    """
    out = _with_fill_rate(row)
    notional = out.get("total_notional") or 0.0
    return {
        **out,
        "hft_rent_per_hft": (out["hft_rent"] / n_hfts) if n_hfts else 0.0,
        "hft_rent_bp": (1e4 * out["hft_rent"] / notional) if notional else 0.0,
    }


def baseline_rows(cfg: dict, n_seeds: int) -> list[dict]:
    """The shared no-HFT arm, computed ONCE for the whole surface.

    Load-bearing for cost: this is why the surface is ~5k runs and not ~10k.
    `_run(..., with_hft=False)` constructs no HFTAgent at all, so neither
    `hft_qty` nor `n_hft` can reach the simulation — the baseline is identical in
    every cell. `tests/test_exp6_incidence.py` pins that invariance rather than
    trusting the reading.
    """
    base_cfg = {**cfg, "n_hft": 0}
    return [_augment(_run(s, base_cfg, with_hft=False, per_maker=True), 0)
            for s in range(1, n_seeds + 1)]


def run_cell(hft_qty: int, n_hfts: int, cfg: dict, base_rows: list[dict],
             n_seeds: int, boot_seed: int = 0) -> dict:
    """One (snipe size, HFT count) cell: treatment, seed-matched delta, CIs."""
    cell_cfg = {**cfg, "hft_qty": hft_qty, "n_hft": n_hfts}
    treat = [_augment(_run(s, cell_cfg, with_hft=True, per_maker=True), n_hfts)
             for s in range(1, n_seeds + 1)]

    deltas = []
    for t, b in zip(treat, base_rows):
        d_mm = t["mm_pnl"] - b["mm_pnl"]
        d_nt = t["nt_pnl"] - b["nt_pnl"]
        deltas.append({
            "mm_pnl": d_mm,
            "nt_pnl": d_nt,
            "total_welfare_loss": -(d_mm + d_nt),
            "liquidity_gaps": t["liquidity_gaps"] - b["liquidity_gaps"],
        })

    return {
        "hft_qty": hft_qty,
        "snipe_quote_ratio": hft_qty / cfg["quote_qty"],
        "n_hfts": n_hfts,
        "n_seeds": n_seeds,
        "treatment_ci": bootstrap_ci_table(treat, CELL_METRICS, seed=boot_seed),
        "delta_ci": bootstrap_ci_table(deltas, DELTA_METRICS, seed=boot_seed),
    }


def classify_incidence(cell: dict) -> str:
    """Who bears the transfer in this cell, at 95% bootstrap confidence.

    The surface's whole point is the SIGN of the maker's PnL delta, so a cell
    whose CI brackets zero is reported as indeterminate rather than folded into
    whichever side its point estimate happens to fall on.
    """
    mm = cell["delta_ci"]["mm_pnl"]
    if mm["lower"] > 0.0:
        return "maker_gains"
    if mm["upper"] < 0.0:
        return "maker_pays"
    return "indeterminate"


def find_boundary(cells: list[dict]) -> dict | None:
    """Locate the k at which incidence flips, for one ratio row.

    Returns the bracketing pair rather than an interpolated crossing: k is a
    count, the grid is coarse (1,2,3,5,8,13,21), and a fractional "boundary at
    k=6.3" would claim resolution the grid does not have.
    """
    ordered = sorted(cells, key=lambda c: c["n_hfts"])
    labels = [classify_incidence(c) for c in ordered]
    for i in range(len(ordered) - 1):
        if labels[i] == "maker_gains" and labels[i + 1] != "maker_gains":
            return {
                "k_last_maker_gains": ordered[i]["n_hfts"],
                "k_first_not_gains": ordered[i + 1]["n_hfts"],
                "flips_to": labels[i + 1],
            }
    return None


def _mean(xs) -> float:
    return float(statistics.fmean(xs)) if xs else 0.0


def _print_row(cell: dict) -> None:
    t, d = cell["treatment_ci"], cell["delta_ci"]
    print(f"{cell['snipe_quote_ratio']:>8.4f}{cell['n_hfts']:>5}"
          f"{t['hft_rent']['mean']:>12.1f}{t['hft_rent_bp']['mean']:>10.3f}"
          f"{d['mm_pnl']['mean']:>12.1f}{d['nt_pnl']['mean']:>12.1f}"
          f"{t['liquidity_gaps']['mean']:>9.2f}"
          f"{t['zero_sum_residual']['mean']:>11.1e}"
          f"  {classify_incidence(cell)}", flush=True)


def main(n_seeds: int = 100, qty_grid=QTY_GRID, k_grid=K_GRID,
         boot_seed: int = 0,
         out_name: str = "exp6_incidence_surface.json") -> dict:
    cfg = {**CFG, **calibrated_cfg()}
    quote_qty = cfg["quote_qty"]
    print(f"Incidence surface — calibrated flow, quote_qty={quote_qty}, "
          f"{n_seeds} seeds, {len(qty_grid)}x{len(k_grid)} cells")
    print(f"{'ratio':>8}{'k':>5}{'rent':>12}{'rent bp':>10}{'dMM':>12}"
          f"{'dNT':>12}{'gaps':>9}{'residual':>11}  incidence")

    base = baseline_rows(cfg, n_seeds)
    print(f"{'base':>8}{0:>5}{'-':>12}{'-':>10}{'-':>12}{'-':>12}"
          f"{_mean([r['liquidity_gaps'] for r in base]):>9.2f}", flush=True)

    rows = []
    for qty in qty_grid:
        cells = [run_cell(qty, k, cfg, base, n_seeds, boot_seed) for k in k_grid]
        for c in cells:
            _print_row(c)
        boundary = find_boundary(cells)
        rows.append({
            "hft_qty": qty,
            "snipe_quote_ratio": qty / quote_qty,
            "cells": cells,
            "boundary": boundary,
        })
        print(f"    -> ratio {qty / quote_qty:.4f}: "
              f"{boundary if boundary else 'no flip on this grid'}", flush=True)

    report = {
        "config": cfg,
        "n_seeds": n_seeds,
        "grid_hft_qty": list(qty_grid),
        "grid_n_hfts": list(k_grid),
        "quote_qty": quote_qty,
        "baseline_liquidity_gaps": _mean([r["liquidity_gaps"] for r in base]),
        "rows": rows,
    }
    out_dir = _ROOT / "results" / "experiments"
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / out_name).write_text(json.dumps(report, indent=2, sort_keys=True))
    print(f"\nwrote results/experiments/{out_name}")
    return report


if __name__ == "__main__":
    main(n_seeds=int(sys.argv[1]) if len(sys.argv) > 1 else 100)
