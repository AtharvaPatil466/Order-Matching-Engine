#!/usr/bin/env python3
"""§4.6 figures — the calibrated re-run (no seaborn dependency).

Reads the *_calibrated.json results and writes to results/figures/:

  fig6a_rent_per_hft_calibrated.png  Per-HFT rent vs k with the C/k overlay —
                                     the §4.3 hyperbolic collapse, replicated
                                     at calibrated order flow.
  fig6b_fragility_calibrated.png     Liquidity gaps vs k for both hft_qty arms.
                                     The main arm (snipe clears the quote) and
                                     the robustness arm (snipe takes 2.4%) rise
                                     at very different rates from a shared
                                     no-HFT baseline of 19.9.
  fig6c_batch_nonmonotone.png        Exp 4: rent eliminated rises monotonically
                                     with the clearing interval, but gaps do
                                     NOT — interval 1 is far worse than the
                                     continuous arm before the remedy takes
                                     hold. The §4.6 result with no §4.4 analogue.
  fig6d_rent_bp_bracket.png          HFT rent per unit of traded notional:
                                     operating point and calibrated arms
                                     bracket the measured real latency tax.

Conventions follow exp3_figures.py: 6 in wide, 300 dpi, no grid lines, error
bars on every point, colorblind-safe palette.

Run: bcs_research/.venv/bin/python bcs_research/analysis/exp46_figures.py
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
EXP = _ROOT / "results" / "experiments"

CB = ["#0173B2", "#DE8F05", "#029E73", "#D55E00", "#CC78BC",
      "#CA9161", "#FBAFE4", "#949494", "#ECE133", "#56B4E9"]
FIGSIZE = (6.0, 4.0)
DPI = 300

# Rent per unit of traded notional, in basis points. Computed by summing
# tick_volume * mid over every snapshot of the n_hft=3 treatment arm (100
# seeds) and dividing rent by it; see §4.6. The empirical reference is the
# latency-arbitrage tax of Aquilina, Budish & O'Neill (2022).
RENT_BP_OPERATING_POINT = 0.125
RENT_BP_CALIBRATED = 4.421
RENT_BP_MEASURED_ABO = 0.4


def _load(name: str) -> dict:
    return json.loads((EXP / name).read_text())


def _save(fig, name: str) -> Path:
    out_dir = _ROOT / "results" / "figures"
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / name
    fig.savefig(path, dpi=DPI, bbox_inches="tight")
    plt.close(fig)
    return path


def _k_series(report: dict, metric: str, min_k: int = 0):
    cells = sorted((c for c in report["cells"] if c["n_hfts"] >= min_k),
                   key=lambda c: c["n_hfts"])
    x = np.array([c["n_hfts"] for c in cells], dtype=float)
    m = np.array([c["treatment_ci"][metric]["mean"] for c in cells], dtype=float)
    lo = np.array([c["treatment_ci"][metric]["lower"] for c in cells], dtype=float)
    hi = np.array([c["treatment_ci"][metric]["upper"] for c in cells], dtype=float)
    return x, m, np.vstack([m - lo, hi - m])


def fig_rent_per_hft() -> Path:
    r = _load("exp3_hft_count_calibrated.json")
    x, m, yerr = _k_series(r, "hft_rent_per_hft", min_k=1)
    # One-parameter C/k fit, C by least squares on 1/k (as in §4.3).
    inv = 1.0 / x
    C = float(np.sum(inv * m) / np.sum(inv * inv))
    r2 = 1 - np.sum((m - C * inv) ** 2) / np.sum((m - m.mean()) ** 2)

    fig, ax = plt.subplots(figsize=FIGSIZE)
    ax.errorbar(x, m / 1000, yerr=yerr / 1000, fmt="o", color=CB[0],
                capsize=3, label="per-HFT rent (95% CI)")
    grid = np.linspace(x.min(), x.max(), 200)
    ax.plot(grid, (C / grid) / 1000, "-", color=CB[1],
            label=f"$C/k$ fit, $C={C/1000:.1f}$k, $R^2={r2:.5f}$")
    ax.set_xlabel("number of competing HFTs")
    ax.set_ylabel("per-HFT rent (\\$ thousands)")
    ax.set_title("Calibrated: per-HFT rent collapses as $1/k$")
    ax.grid(False)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.legend(frameon=False, fontsize=8)
    return _save(fig, "fig6a_rent_per_hft_calibrated.png")


def fig_fragility() -> Path:
    main = _load("exp3_hft_count_calibrated.json")
    rob = _load("exp3_hft_count_calibrated_hftqty50.json")

    fig, ax = plt.subplots(figsize=FIGSIZE)
    for report, colour, label in (
        (main, CB[0], "snipe clears the quote (hft_qty = 2061)"),
        (rob, CB[3], "snipe takes 2.4% (hft_qty = 50)"),
    ):
        x, m, yerr = _k_series(report, "liquidity_gaps")
        ax.errorbar(x, m, yerr=yerr, fmt="o-", color=colour, capsize=3,
                    label=label, markersize=4, linewidth=1.2)
    ax.set_xlabel("number of competing HFTs")
    ax.set_ylabel("liquidity gaps per run")
    ax.set_title("Calibrated fragility depends on snipe-to-quote size")
    ax.grid(False)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.legend(frameon=False, fontsize=8, loc="upper left")
    return _save(fig, "fig6b_fragility_calibrated.png")


def fig_batch_nonmonotone() -> Path:
    r = _load("exp4_batch_auction_calibrated.json")
    cells = r["cells"]
    cont = next(c for c in cells if c["batch_interval"] == "continuous")
    cont_rent = cont["treatment_ci"]["hft_rent"]["mean"]
    cont_gaps = cont["treatment_ci"]["liquidity_gaps"]["mean"]
    batch = sorted((c for c in cells if c["batch_interval"] != "continuous"),
                   key=lambda c: c["batch_interval"])
    x = np.array([c["batch_interval"] for c in batch], dtype=float)
    elim = np.array([100 * (1 - c["treatment_ci"]["hft_rent"]["mean"] / cont_rent)
                     for c in batch])
    gaps = np.array([c["treatment_ci"]["liquidity_gaps"]["mean"] for c in batch])

    fig, ax = plt.subplots(figsize=FIGSIZE)
    ax.plot(x, elim, "o-", color=CB[0], markersize=4, linewidth=1.2,
            label="rent eliminated (left)")
    ax.set_xscale("log")
    ax.set_xlabel("batch clearing interval (ticks, log scale)")
    ax.set_ylabel("rent eliminated (%)", color=CB[0])
    ax.tick_params(axis="y", labelcolor=CB[0])
    ax.set_ylim(60, 102)
    ax.grid(False)
    ax.spines["top"].set_visible(False)

    ax2 = ax.twinx()
    ax2.plot(x, gaps, "s--", color=CB[3], markersize=4, linewidth=1.2,
             label="liquidity gaps (right)")
    ax2.axhline(cont_gaps, color=CB[7], linestyle=":", linewidth=1.2)
    ax2.annotate(f"continuous arm ({cont_gaps:.0f})", xy=(x[-1], cont_gaps),
                 xytext=(-4, 6), textcoords="offset points", ha="right",
                 fontsize=7, color=CB[7])
    ax2.set_ylabel("liquidity gaps per run", color=CB[3])
    ax2.tick_params(axis="y", labelcolor=CB[3])
    ax2.grid(False)
    ax2.spines["top"].set_visible(False)

    # Within the batch grid gaps fall monotonically; the non-monotonicity is
    # against the CONTINUOUS arm, which is the dotted reference, not a plotted
    # point. Title states the comparison the figure actually shows.
    ax.set_title("Batching below ~10 ticks is worse than not batching")
    lines = ax.get_lines()[:1] + ax2.get_lines()[:1]
    ax.legend(lines, [l.get_label() for l in lines], frameon=False,
              fontsize=8, loc="center right")
    return _save(fig, "fig6c_batch_nonmonotone.png")


def fig_bp_bracket() -> Path:
    labels = ["operating point\n(§4.1–4.5)", "calibrated\n(§4.6)"]
    vals = [RENT_BP_OPERATING_POINT, RENT_BP_CALIBRATED]

    fig, ax = plt.subplots(figsize=FIGSIZE)
    bars = ax.bar(labels, vals, color=[CB[7], CB[0]], width=0.5)
    ax.axhline(RENT_BP_MEASURED_ABO, color=CB[3], linestyle="--", linewidth=1.5)
    # Label sits in the empty gap between the two bars; anchoring it past the
    # right bar puts it outside the axes, where it is silently clipped.
    ax.text(0.5, RENT_BP_MEASURED_ABO * 1.15,
            f"measured latency tax, {RENT_BP_MEASURED_ABO} bp\n"
            "(Aquilina, Budish & O'Neill 2022)",
            ha="center", va="bottom", fontsize=7.5, color=CB[3])
    for b, v in zip(bars, vals):
        ax.annotate(f"{v:.3g} bp", xy=(b.get_x() + b.get_width() / 2, v),
                    xytext=(0, 3), textcoords="offset points",
                    ha="center", fontsize=9)
    ax.set_yscale("log")
    ax.set_ylabel("HFT rent (bp of traded notional, log scale)")
    ax.set_title("The two arms bracket the measured latency tax")
    ax.grid(False)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    return _save(fig, "fig6d_rent_bp_bracket.png")


def main() -> list:
    paths = [fig_rent_per_hft(), fig_fragility(),
             fig_batch_nonmonotone(), fig_bp_bracket()]
    for p in paths:
        print(f"wrote {p.relative_to(_ROOT)}")
    return paths


if __name__ == "__main__":
    main()
