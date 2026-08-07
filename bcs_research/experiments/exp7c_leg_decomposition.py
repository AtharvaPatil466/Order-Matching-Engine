#!/usr/bin/env python3
"""Step 6c — the two legs of §4.7's inversion, measured rather than asserted.

Why this exists. §4.7 explains the maker's gain at small snipe ratios as a race
between two quantities: what the maker earns from noise flow at its widened
spread, and what snipers take from its stale quotes. The surface reports only
the difference (`mm_pnl` delta), so the explanation is currently a story told
about a single number. This driver measures the two legs separately at the
tape-band cells so a reader can see which one moves.

The decomposition. Each maker fill is scored as EDGE against the fundamental at
fill time — positive when the maker captured value — and partitioned by the same
`adverse` test that drives the maker's widening (`BCSMarketMaker.on_fill`):

  revenue leg   sum of edge over non-adverse fills (noise flow crossing the
                quoted spread; positive)
  sniping leg   sum of edge over adverse fills (quotes picked off against the
                true value; negative)

Using the mechanism's own discriminator is deliberate. An alternative such as
"half-spread x noise volume" would be an expected-earnings proxy computed
outside the simulation, and would not reconcile with the PnL the surface
reports; scoring realized fills with the test that actually triggers widening
reports the mechanism instead of re-describing it.

Reconciliation. `mm_pnl` is mark-to-market (cash plus inventory at the mark)
whereas the legs are realized edge at fill time, so the two legs do NOT sum to
it. The gap is inventory carry: value that moved after the fill. Rather than
absorb it into either leg we report it as an explicit third column, so

    revenue + sniping + inventory residual = net delta

holds exactly by construction and the net column equals §4.7's `mm_pnl` delta.
`reconciles` asserts that per cell rather than trusting it.

Scope. §4.7's configuration (single maker) at the two tape-band ratios and four
HFT counts, n=100 — the eight cells the table needs, not the full surface.

Run: bcs_research/.venv/bin/python bcs_research/experiments/exp7c_leg_decomposition.py [n_seeds]
"""
from __future__ import annotations

import json
import sys
import time
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_ROOT = _HERE.parent
for _d in ("build", "agents", "simulation", "metrics", "experiments"):
    _p = str(_ROOT / _d)
    if _p not in sys.path:
        sys.path.insert(0, _p)

from bootstrap import bootstrap_ci                  # noqa: E402
from exp1_primary import CFG, _run                  # noqa: E402
from run_calibrated import calibrated_cfg           # noqa: E402

# 10/2061 = 0.0049 and 20/2061 = 0.0097, bracketing the tape's 0.0072 median.
TAPE_BAND_QTY = (10, 20)
K_GRID = (1, 3, 7, 21)

# Largest absolute reconciliation error tolerated between the three legs and the
# net delta. They sum by construction, so anything above float noise is a bug.
RECONCILE_TOL = 1e-6


def _legs(row: dict) -> tuple[float, float, float]:
    """(revenue, sniping, inventory residual) for one run; sums to mm_pnl."""
    rev, snipe = row["mm_noise_edge"], row["mm_adverse_edge"]
    return rev, snipe, row["mm_pnl"] - rev - snipe


def run_cell(hft_qty: int, n_hfts: int, cfg: dict, base_rows: list[dict],
             n_seeds: int, boot_seed: int = 0) -> dict:
    """One cell: each leg's seed-matched delta, bootstrapped separately."""
    cell_cfg = {**cfg, "hft_qty": hft_qty, "n_hft": n_hfts}
    treat = [_run(s, cell_cfg, with_hft=True, per_maker=True)
             for s in range(1, n_seeds + 1)]

    d_rev, d_snipe, d_resid, d_net = [], [], [], []
    for t, b in zip(treat, base_rows):
        tr, ts_, ti = _legs(t)
        br, bs, bi = _legs(b)
        d_rev.append(tr - br)
        d_snipe.append(ts_ - bs)
        d_resid.append(ti - bi)
        d_net.append(t["mm_pnl"] - b["mm_pnl"])

    cell = {
        "hft_qty": hft_qty,
        "snipe_quote_ratio": hft_qty / cfg["quote_qty"],
        "n_hfts": n_hfts,
        "n_seeds": n_seeds,
        "revenue_leg": bootstrap_ci(d_rev, seed=boot_seed),
        "sniping_leg": bootstrap_ci(d_snipe, seed=boot_seed),
        "inventory_residual": bootstrap_ci(d_resid, seed=boot_seed),
        "net_delta": bootstrap_ci(d_net, seed=boot_seed),
    }
    worst = max(abs(r + s + i - n)
                for r, s, i, n in zip(d_rev, d_snipe, d_resid, d_net))
    cell["max_reconcile_error"] = worst
    cell["reconciles"] = worst < RECONCILE_TOL
    return cell


def main(n_seeds: int = 100, out_name: str = "exp7c_leg_decomposition.json",
         boot_seed: int = 0) -> dict:
    cfg = {**CFG, **calibrated_cfg()}
    t0 = time.time()
    print(f"Leg decomposition — single maker (§4.7 config), quote_qty="
          f"{cfg['quote_qty']}, {n_seeds} seeds, "
          f"{len(TAPE_BAND_QTY)}x{len(K_GRID)} cells")
    print(f"{'ratio':>8}{'k':>4}{'revenue':>12}{'sniping':>12}"
          f"{'inventory':>12}{'net':>12}  reconciles")

    base = [_run(s, {**cfg, "n_hft": 0}, with_hft=False, per_maker=True)
            for s in range(1, n_seeds + 1)]

    cells = []
    for qty in TAPE_BAND_QTY:
        for k in K_GRID:
            c = run_cell(qty, k, cfg, base, n_seeds, boot_seed)
            cells.append(c)
            print(f"{c['snipe_quote_ratio']:>8.4f}{k:>4}"
                  f"{c['revenue_leg']['mean']:>12.1f}"
                  f"{c['sniping_leg']['mean']:>12.1f}"
                  f"{c['inventory_residual']['mean']:>12.1f}"
                  f"{c['net_delta']['mean']:>12.1f}  {c['reconciles']}",
                  flush=True)

    elapsed = time.time() - t0
    report = {
        "config": cfg,
        "n_seeds": n_seeds,
        "grid_hft_qty": list(TAPE_BAND_QTY),
        "grid_n_hfts": list(K_GRID),
        "quote_qty": cfg["quote_qty"],
        "cells": cells,
        "all_reconcile": all(c["reconciles"] for c in cells),
        "max_reconcile_error": max(c["max_reconcile_error"] for c in cells),
        "elapsed_sec": elapsed,
    }
    out_dir = _ROOT / "results" / "experiments"
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / out_name).write_text(json.dumps(report, indent=2, sort_keys=True))
    print(f"\nall cells reconcile: {report['all_reconcile']} "
          f"(max error {report['max_reconcile_error']:.2e})")
    print(f"elapsed {elapsed / 60:.1f} min, wrote results/experiments/{out_name}")
    return report


if __name__ == "__main__":
    main(n_seeds=int(sys.argv[1]) if len(sys.argv) > 1 else 100)
