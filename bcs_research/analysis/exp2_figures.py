#!/usr/bin/env python3
"""Exp 2 figures — publication-quality matplotlib (no seaborn dependency).

Reads results/experiments/exp2_latency_sweep.json (+ exp2_phase_transition.json
for the fitted linear trend) and writes three figures to results/figures/:

  fig2a_hft_rent.png        HFT rent vs latency advantage, 95% CI error bars,
                            linear fit overlay, significance threshold marked.
  fig2b_welfare_decomp.png  HFT rent, dMM PnL, dNT PnL on one axis (zero-sum).
  fig2c_liquidity_gaps.png  Liquidity gap count (left axis) + HFT fill rate
                            (right axis) vs latency advantage.

Conventions: 6 in wide, 300 dpi, no grid lines, error bars on every point,
colorblind-safe palette.

Run: bcs_research/.venv/bin/python bcs_research/analysis/exp2_figures.py
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

# Seaborn "colorblind" palette, inlined to avoid the dependency.
CB = ["#0173B2", "#DE8F05", "#029E73", "#D55E00", "#CC78BC",
      "#CA9161", "#FBAFE4", "#949494", "#ECE133", "#56B4E9"]
FIGSIZE = (6.0, 4.0)
DPI = 300
US_PER_TICK = 1000


def _cells(report: dict) -> list:
    return sorted(report["cells"], key=lambda c: c["mm_latency_us"])


def _series(cells: list, table: str, metric: str):
    """Return (x, mean, yerr[2,N]) for a metric from a per-cell CI table."""
    x = np.array([c["mm_latency_us"] for c in cells], dtype=float)
    mean = np.array([c[table][metric]["mean"] for c in cells], dtype=float)
    lo = np.array([c[table][metric]["lower"] for c in cells], dtype=float)
    hi = np.array([c[table][metric]["upper"] for c in cells], dtype=float)
    yerr = np.vstack([mean - lo, hi - mean])
    return x, mean, yerr


def _style(ax):
    ax.grid(False)
    ax.set_xlabel("HFT latency advantage = MM staleness gap (us)")
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)


def _save(fig, name: str) -> Path:
    out_dir = _ROOT / "results" / "figures"
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / name
    fig.savefig(path, dpi=DPI, bbox_inches="tight")
    plt.close(fig)
    return path


def fig_2a(report: dict, pt: dict) -> Path:
    cells = _cells(report)
    x, mean, yerr = _series(cells, "treatment_ci", "hft_rent")

    fig, ax = plt.subplots(figsize=FIGSIZE)
    ax.errorbar(x, mean, yerr=yerr, fmt="o-", color=CB[0], capsize=3,
                label="HFT rent (95% CI)")
    # Fitted linear trend (the BIC-preferred model).
    a = pt["linear_intercept"]
    b = pt["linear_slope_per_tick"] / US_PER_TICK
    xs = np.linspace(x.min(), x.max(), 100)
    ax.plot(xs, a + b * xs, "--", color=CB[3],
            label=f"linear fit (${pt['linear_slope_per_tick']:.1f}/tick)")
    ax.axhline(0.0, color="0.6", lw=0.8, ls=":")
    sig = pt.get("significance_threshold_us")
    if sig is not None:
        ax.axvline(sig, color=CB[2], lw=0.8, ls="-.")
        ax.annotate(f"rent>0 @ {sig/1000:.0f} ticks", xy=(sig, 0),
                    xytext=(sig + 1500, mean.max() * 0.25),
                    color=CB[2], fontsize=8)
    _style(ax)
    ax.set_ylabel("HFT rent ($)")
    ax.set_title("Exp 2a - HFT rent scales with the latency advantage")
    ax.legend(frameon=False, fontsize=8)
    return _save(fig, "fig2a_hft_rent.png")


def fig_2b(report: dict) -> Path:
    cells = _cells(report)
    fig, ax = plt.subplots(figsize=FIGSIZE)
    for (table, metric, label, color) in [
        ("treatment_ci", "hft_rent", "HFT rent", CB[0]),
        ("delta_ci", "mm_pnl", "dMM PnL", CB[1]),
        ("delta_ci", "nt_pnl", "dNT PnL", CB[2]),
    ]:
        x, mean, yerr = _series(cells, table, metric)
        ax.errorbar(x, mean, yerr=yerr, fmt="o-", color=color, capsize=3, label=label)
    ax.axhline(0.0, color="0.6", lw=0.8, ls=":")
    _style(ax)
    ax.set_ylabel("welfare ($, treatment - baseline)")
    ax.set_title("Exp 2b - zero-sum welfare transfer vs latency")
    ax.legend(frameon=False, fontsize=8)
    return _save(fig, "fig2b_welfare_decomp.png")


def fig_2c(report: dict) -> Path:
    cells = _cells(report)
    fig, ax = plt.subplots(figsize=FIGSIZE)
    x, gaps, gaps_err = _series(cells, "treatment_ci", "liquidity_gaps")
    ax.errorbar(x, gaps, yerr=gaps_err, fmt="o-", color=CB[0], capsize=3,
                label="liquidity gaps")
    ax.set_ylabel("liquidity gap count", color=CB[0])
    ax.tick_params(axis="y", labelcolor=CB[0])
    _style(ax)

    ax2 = ax.twinx()
    _, fill, fill_err = _series(cells, "treatment_ci", "hft_fill_rate")
    ax2.errorbar(x, fill, yerr=fill_err, fmt="s--", color=CB[3], capsize=3,
                 label="HFT fill rate")
    ax2.set_ylabel("HFT fill rate", color=CB[3])
    ax2.tick_params(axis="y", labelcolor=CB[3])
    ax2.spines["top"].set_visible(False)
    ax.set_title("Exp 2c - fragility and sniping success vs latency")
    return _save(fig, "fig2c_liquidity_gaps.png")


def main() -> list:
    exp = _ROOT / "results" / "experiments"
    report = json.loads((exp / "exp2_latency_sweep.json").read_text())
    pt = json.loads((exp / "exp2_phase_transition.json").read_text())
    paths = [fig_2a(report, pt), fig_2b(report), fig_2c(report)]
    for p in paths:
        print(f"wrote {p.relative_to(_ROOT)}")
    return paths


if __name__ == "__main__":
    main()
