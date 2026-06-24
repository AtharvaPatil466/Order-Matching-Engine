#!/usr/bin/env python3
"""Exp 4 figures — batch auction vs continuous LOB (no seaborn dependency).

Reads results/experiments/exp4_batch_auction.json and writes to results/figures/:

  fig4a_hft_rent.png        HFT rent vs batch interval, 95% CI error bars, with
                            the continuous LOB level as the rightmost point.
  fig4b_welfare_decomp.png  HFT rent, dMM PnL, dNT PnL vs batch interval.
  fig4c_liquidity_gaps.png  Liquidity gap count vs batch interval.

The x-axis is categorical (1,5,...,500,continuous) because 'continuous' has no
numeric interval; it sits at the right as the no-intervention reference.

Run: bcs_research/.venv/bin/python bcs_research/analysis/exp4_figures.py
"""
from __future__ import annotations

import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt        # noqa: E402
import numpy as np                     # noqa: E402

_HERE = Path(__file__).resolve().parent
_ROOT = _HERE.parent

CB = ["#0173B2", "#DE8F05", "#029E73", "#D55E00", "#CC78BC",
      "#CA9161", "#FBAFE4", "#949494", "#ECE133", "#56B4E9"]
FIGSIZE = (6.5, 4.0)
DPI = 300
CONTINUOUS = "continuous"


def _ordered_cells(report: dict) -> list:
    finite = sorted((c for c in report["cells"] if c["batch_interval"] != CONTINUOUS),
                    key=lambda c: c["batch_interval"])
    cont = [c for c in report["cells"] if c["batch_interval"] == CONTINUOUS]
    return finite + cont


def _series(cells: list, table: str, metric: str):
    mean = np.array([c[table][metric]["mean"] for c in cells], dtype=float)
    lo = np.array([c[table][metric]["lower"] for c in cells], dtype=float)
    hi = np.array([c[table][metric]["upper"] for c in cells], dtype=float)
    yerr = np.vstack([mean - lo, hi - mean])
    return mean, yerr


def _labels(cells: list) -> list:
    return [str(c["batch_interval"]) for c in cells]


def _style(ax, cells):
    ax.grid(False)
    ax.set_xlabel("batch interval (ticks)")
    ax.set_xticks(range(len(cells)))
    ax.set_xticklabels(_labels(cells))
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)


def _save(fig, name: str) -> Path:
    out_dir = _ROOT / "results" / "figures"
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / name
    fig.savefig(path, dpi=DPI, bbox_inches="tight")
    plt.close(fig)
    return path


def fig_4a(report: dict) -> Path:
    cells = _ordered_cells(report)
    x = range(len(cells))
    mean, yerr = _series(cells, "treatment_ci", "hft_rent")
    fig, ax = plt.subplots(figsize=FIGSIZE)
    ax.errorbar(x, mean, yerr=yerr, fmt="o-", color=CB[0], capsize=3,
                label="HFT rent (95% CI)")
    # Continuous reference line (last point).
    ax.axhline(mean[-1], color=CB[3], lw=0.8, ls="--", label="continuous level")
    ax.axhline(0.0, color="0.6", lw=0.8, ls=":")
    _style(ax, cells)
    ax.set_ylabel("HFT rent ($)")
    ax.set_title("Exp 4a - batching reduces HFT rent (point estimate)")
    ax.legend(frameon=False, fontsize=8)
    return _save(fig, "fig4a_hft_rent.png")


def fig_4b(report: dict) -> Path:
    cells = _ordered_cells(report)
    x = range(len(cells))
    fig, ax = plt.subplots(figsize=FIGSIZE)
    for (table, metric, label, color) in [
        ("treatment_ci", "hft_rent", "HFT rent", CB[0]),
        ("delta_ci", "mm_pnl", "dMM PnL", CB[1]),
        ("delta_ci", "nt_pnl", "dNT PnL", CB[2]),
    ]:
        mean, yerr = _series(cells, table, metric)
        ax.errorbar(x, mean, yerr=yerr, fmt="o-", color=color, capsize=3, label=label)
    ax.axhline(0.0, color="0.6", lw=0.8, ls=":")
    _style(ax, cells)
    ax.set_ylabel("welfare ($)")
    ax.set_title("Exp 4b - MM welfare recovers as the batch interval grows")
    ax.legend(frameon=False, fontsize=8)
    return _save(fig, "fig4b_welfare_decomp.png")


def fig_4c(report: dict) -> Path:
    cells = _ordered_cells(report)
    x = range(len(cells))
    mean, yerr = _series(cells, "treatment_ci", "liquidity_gaps")
    fig, ax = plt.subplots(figsize=FIGSIZE)
    ax.errorbar(x, mean, yerr=yerr, fmt="o-", color=CB[0], capsize=3)
    ax.axhline(mean[-1], color=CB[3], lw=0.8, ls="--", label="continuous level")
    _style(ax, cells)
    ax.set_ylabel("liquidity gap count")
    ax.set_title("Exp 4c - fragility vs batch interval (interval=1 churns)")
    ax.legend(frameon=False, fontsize=8)
    return _save(fig, "fig4c_liquidity_gaps.png")


def main() -> list:
    exp = _ROOT / "results" / "experiments"
    report = json.loads((exp / "exp4_batch_auction.json").read_text())
    paths = [fig_4a(report), fig_4b(report), fig_4c(report)]
    for p in paths:
        print(f"wrote {p.relative_to(_ROOT)}")
    return paths


if __name__ == "__main__":
    main()
