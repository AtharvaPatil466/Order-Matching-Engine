#!/usr/bin/env python3
"""Exp 3 figures — the socially-wasteful arms race (no seaborn dependency).

Reads results/experiments/exp3_hft_count.json and writes to results/figures/:

  fig3_dual_axis.png            HEADLINE. Per-HFT rent (left, collapses ~1/k)
                                vs total welfare loss (right). The welfare loss
                                is FLAT, not rising: a fixed prize set by the
                                latency opportunity is split into ever-finer
                                slices, and no HFT can profitably exit.
  fig3a_rent_per_hft.png        Per-HFT rent vs n_hfts with 95% bootstrap CIs.
  fig3b_welfare_and_fragility.png  Total welfare loss (flat) + liquidity gaps
                                (rising) — the social cost shows up as fragility.
  fig3c_fill_rate.png           HFT fill rate vs n_hfts (race dissipation, ~1/k).

Conventions: 6 in wide, 300 dpi, no grid lines, error bars on every point,
colorblind-safe palette.

Run: bcs_research/.venv/bin/python bcs_research/analysis/exp3_figures.py
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
FIGSIZE = (6.0, 4.0)
DPI = 300


def _cells(report: dict, min_hfts: int = 0) -> list:
    return sorted((c for c in report["cells"] if c["n_hfts"] >= min_hfts),
                  key=lambda c: c["n_hfts"])


def _series(cells: list, table: str, metric: str):
    x = np.array([c["n_hfts"] for c in cells], dtype=float)
    mean = np.array([c[table][metric]["mean"] for c in cells], dtype=float)
    lo = np.array([c[table][metric]["lower"] for c in cells], dtype=float)
    hi = np.array([c[table][metric]["upper"] for c in cells], dtype=float)
    yerr = np.vstack([mean - lo, hi - mean])
    return x, mean, yerr


def _style(ax):
    ax.grid(False)
    ax.set_xlabel("number of competing HFTs")
    ax.spines["top"].set_visible(False)


def _save(fig, name: str) -> Path:
    out_dir = _ROOT / "results" / "figures"
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / name
    fig.savefig(path, dpi=DPI, bbox_inches="tight")
    plt.close(fig)
    return path


def fig_dual_axis(report: dict) -> Path:
    cells = _cells(report, min_hfts=1)  # per-HFT rent undefined at 0
    fig, ax = plt.subplots(figsize=FIGSIZE)
    x, rph, rph_err = _series(cells, "treatment_ci", "hft_rent_per_hft")
    ax.errorbar(x, rph, yerr=rph_err, fmt="o-", color=CB[0], capsize=3,
                label="rent per HFT")
    ax.set_ylabel("rent per HFT ($)", color=CB[0])
    ax.tick_params(axis="y", labelcolor=CB[0])
    _style(ax)

    ax2 = ax.twinx()
    _, wl, wl_err = _series(cells, "delta_ci", "total_welfare_loss")
    ax2.errorbar(x, wl, yerr=wl_err, fmt="s--", color=CB[3], capsize=3,
                 label="total welfare loss")
    ax2.set_ylabel("total welfare loss ($)", color=CB[3])
    ax2.tick_params(axis="y", labelcolor=CB[3])
    ax2.set_ylim(0, max(wl) * 1.5)
    ax2.spines["top"].set_visible(False)
    ax.set_title("Exp 3 - per-HFT rent collapses ~1/k; total loss stays flat")
    return _save(fig, "fig3_dual_axis.png")


def fig_3a(report: dict) -> Path:
    cells = _cells(report, min_hfts=1)
    fig, ax = plt.subplots(figsize=FIGSIZE)
    x, rph, rph_err = _series(cells, "treatment_ci", "hft_rent_per_hft")
    ax.errorbar(x, rph, yerr=rph_err, fmt="o", color=CB[0], capsize=3,
                label="rent per HFT (95% CI)")
    _style(ax)
    ax.set_ylabel("rent per HFT ($)")
    ax.set_title("Exp 3a - individual HFT rent erodes with competition")
    ax.legend(frameon=False, fontsize=8)
    return _save(fig, "fig3a_rent_per_hft.png")


def fig_3b(report: dict) -> Path:
    cells = _cells(report, min_hfts=0)
    fig, ax = plt.subplots(figsize=FIGSIZE)
    x, wl, wl_err = _series(cells, "delta_ci", "total_welfare_loss")
    ax.errorbar(x, wl, yerr=wl_err, fmt="o-", color=CB[3], capsize=3,
                label="total welfare loss")
    ax.set_ylabel("total welfare loss ($)", color=CB[3])
    ax.tick_params(axis="y", labelcolor=CB[3])
    ax.set_ylim(0, max(wl) * 1.6)
    _style(ax)

    ax2 = ax.twinx()
    _, gaps, gaps_err = _series(cells, "treatment_ci", "liquidity_gaps")
    ax2.errorbar(x, gaps, yerr=gaps_err, fmt="s--", color=CB[0], capsize=3,
                 label="liquidity gaps")
    ax2.set_ylabel("liquidity gap count", color=CB[0])
    ax2.tick_params(axis="y", labelcolor=CB[0])
    ax2.spines["top"].set_visible(False)
    ax.set_title("Exp 3b - flat rent, but fragility rises with HFT count")
    return _save(fig, "fig3b_welfare_and_fragility.png")


def fig_3c(report: dict) -> Path:
    cells = _cells(report, min_hfts=1)
    fig, ax = plt.subplots(figsize=FIGSIZE)
    x, fill, fill_err = _series(cells, "treatment_ci", "hft_fill_rate")
    ax.errorbar(x, fill, yerr=fill_err, fmt="o-", color=CB[0], capsize=3)
    _style(ax)
    ax.set_ylabel("HFT fill rate (fills / snipes)")
    ax.set_title("Exp 3c - race success per HFT falls with competition")
    return _save(fig, "fig3c_fill_rate.png")


def main() -> list:
    exp = _ROOT / "results" / "experiments"
    report = json.loads((exp / "exp3_hft_count.json").read_text())
    paths = [fig_dual_axis(report), fig_3a(report), fig_3b(report), fig_3c(report)]
    for p in paths:
        print(f"wrote {p.relative_to(_ROOT)}")
    return paths


if __name__ == "__main__":
    main()
