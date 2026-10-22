#!/usr/bin/env python3
"""Figures for §4: each confirmatory result against its prediction, intervals and verdicts shown.

Why this exists. §4 reports the pre-registered predictions P1-P11 as verdicts on
intervals. These figures draw the same intervals against the same bands the
drivers judged, read straight from each driver's summary JSON
(results/experiments/<driver>.json); nothing is re-estimated here. One function
per figure takes the driver summaries (a path or a dict each) and returns a
matplotlib Figure:

  headline_bcs_recovery  exp9 + exp10    the model recovers BCS: race-loss share
                                         against (N-1)/N (P1) and the rent free entry
                                         dissipates against floor(R/c) c / R (P4)
  validation_p1_p3       exp9 + exp17    P1's residuals inside their bands (race loss,
                                         h-hat / h*); P3's profit per snipe by spread
  race_p2                exp16           H2a cancel-first share against lag / eps per k,
                                         with P_win and its band; H2d maker-gains cells
                                         against the earlier maker's latency
  makers_p8_p10          exp12 + exp15   P8's Nash half-spread steps and M = 5 against
                                         breakeven; P10 / P10b hit rates
  welfare_p5_p7          exp18 + exp19 + exp20   P5's fee ratios, P6's participation and
                                         deadweight, P7's loss ratio against 1 - P_win
  batch_p9               exp11           maker's sniping loss against tau, batch and
                                         continuous, with 1.2 x footnote 34's bound
  sensitivity_p11        exp13           Sobol S1 of sniper rent, ST of maker P&L change

Verdicts. A primary's badge shows its Holm verdict when the report JSON
(paper/confirmatory_results.json, written by confirmatory_report.py over the same
results directory) is present, else the driver's own 95% verdict, labelled
"95%, before Holm". Secondaries show their unadjusted 95% verdicts (§2).

Units. Money at the operating point is in simulation money units per second (the
drivers' JSON calls them USD: one engine unit is the 0.10 USD tick on one lot);
the BCS-units markets (P1, P3, P4, P5's H5a) are in ticks. Intervals are the
drivers': 95% percentile bootstraps over seeds, Wilson for proportions.

Style follows the dataviz method: thin marks, hairline grid, one y-scale per
panel, an ordinal blue ramp for ordered series (k, jump ratio), emphasis in blue
against grey, status colours only on verdict badges and always with their text.
Light only: these go to print.

Run (from bcs_research/), after the batch and the report:
    .venv/bin/python analysis/confirmatory_report.py
    .venv/bin/python analysis/confirmatory_figures.py
    .venv/bin/python analysis/confirmatory_figures.py --results-dir DIR --out-dir DIR --allow-smoke
A figure whose input is missing or not confirmatory is skipped with a message;
--allow-smoke renders it anyway, stamped NOT CONFIRMATORY (layout checks only).
"""
from __future__ import annotations

import argparse
import functools
import json
import math
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from matplotlib.lines import Line2D  # noqa: E402
from matplotlib.patches import Patch, Rectangle  # noqa: E402
from matplotlib.ticker import MaxNLocator, NullLocator  # noqa: E402

_ROOT = Path(__file__).resolve().parents[1]
for _d in ("experiments", "analysis"):
    if str(_ROOT / _d) not in sys.path:
        sys.path.insert(0, str(_ROOT / _d))

import operating_point as op  # noqa: E402  (puts build/, agents/, metrics/, ... on sys.path)
import exp11_batch_semantics as x11  # noqa: E402
import exp15_heldout_who_pays as x15  # noqa: E402
import exp16_race_incidence as x16  # noqa: E402
import exp18_fee_schedules as x18  # noqa: E402
import exp19_noise_heterogeneity as x19  # noqa: E402
import exp20_maker_strategies as x20  # noqa: E402
from bcs_theory import race_win_probability  # noqa: E402
from confirmatory_report import OUT_MD  # noqa: E402

OUT_DIR = _ROOT / "results" / "figures" / "confirmatory"
MONEY = "simulation money units per second"

# --- style (dataviz reference palette, light; ramps validated with validate_palette.js) ---
SURFACE, INK, INK2, MUTED = "#ffffff", "#0b0b0b", "#52514e", "#898781"
GRID, AXIS = "#e1e0d9", "#c3c2b7"
BLUE, ORANGE = "#2a78d6", "#eb6834"
RAMP3 = ("#86b6ef", "#2a78d6", "#104281")                 # ordinal: jump ratio
RAMP4 = ("#86b6ef", "#3987e5", "#1c5cab", "#0d366b")      # ordinal: snipers k
MARKERS = ("o", "s", "^", "D")
STATUS = {"HIT": "#0ca30c", "MISS": "#d03b3b", "INCONCLUSIVE": "#fab219"}
THEORY = {"color": INK2, "lw": 1.0, "ls": (0, (4, 2)), "zorder": 2}
BAND = {"color": GRID, "lw": 0, "zorder": 1}
STYLE = {
    "font.family": "sans-serif",
    "font.sans-serif": ["Helvetica Neue", "Helvetica", "Arial", "DejaVu Sans"],
    "font.size": 7.5, "axes.titlesize": 8.5, "axes.labelsize": 7.5,
    "xtick.labelsize": 7, "ytick.labelsize": 7, "legend.fontsize": 6.8,
    "axes.edgecolor": AXIS, "axes.linewidth": 0.8, "axes.labelcolor": INK2,
    "xtick.color": AXIS, "ytick.color": AXIS, "xtick.labelcolor": INK2, "ytick.labelcolor": INK2,
    "axes.spines.top": False, "axes.spines.right": False,
    "axes.grid": True, "axes.grid.axis": "y", "grid.color": GRID, "grid.linewidth": 0.6,
    "grid.linestyle": "-", "axes.axisbelow": True,
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
    "legend.frameon": False, "svg.fonttype": "none",
}


def _load(x) -> dict:
    return json.loads(Path(x).read_text()) if isinstance(x, (str, Path)) else x


def _styled(fn):
    """Figure functions take paths or dicts and draw inside the style's rc context."""
    @functools.wraps(fn)
    def wrapper(*summaries, holm=None):
        with plt.rc_context(STYLE):
            return fn(*(_load(s) for s in summaries), holm=holm)
    return wrapper


def _est(ci: dict):
    return ci.get("mean", ci.get("rate", ci.get("estimate")))


def _interval(ax, at, ci, color, marker="o", label=None, horizontal=False, shift=0.0, scale=1.0):
    """One estimate and its interval: a whisker and a surface-ringed marker. Values are
    drawn as (v - shift) * scale, so residuals and ratios reuse the driver's interval."""
    if not ci or _est(ci) is None:
        return
    m, lo, hi = ((v - shift) * scale if v is not None else math.nan
                 for v in (_est(ci), ci.get("lower"), ci.get("upper")))
    whisker = ax.hlines if horizontal else ax.vlines
    whisker(at, lo, hi, color=color, lw=1.4, zorder=3)
    xy = (m, at) if horizontal else (at, m)
    ax.plot(*xy, marker, color=color, mec=SURFACE, mew=0.9, ms=5, label=label, zorder=4)


def _dodge(i: int, n: int, width: float) -> float:
    return (i - (n - 1) / 2) * width


def _half(band) -> float:
    return (band[1] - band[0]) / 2


def _title(ax, text: str) -> None:
    ax.set_title(text, loc="left", color=INK, fontweight="bold", pad=6)


def _badges(ax, items, corner: str = "upper left") -> None:
    """Verdict badges, stacked: ink text in a frame of the verdict's status colour."""
    x, ha = (0.02, "left") if "left" in corner else (0.98, "right")
    top = corner.startswith("upper")
    for i, (label, verdict) in enumerate(items):
        ax.annotate(f"{label}: {verdict}", xy=(x, 0.97 if top else 0.03), xycoords="axes fraction",
                    xytext=(0, -14 * i if top else 14 * i), textcoords="offset points",
                    ha=ha, va="top" if top else "bottom", fontsize=6.6, color=INK, zorder=6,
                    bbox={"boxstyle": "round,pad=0.25", "fc": SURFACE,
                          "ec": STATUS.get(verdict, MUTED), "lw": 1.1})


def _primary(pid: str, v95: str, holm: dict | None) -> tuple:
    """The Holm verdict when the report gives one, else the driver's 95%, labelled."""
    if holm and holm.get(pid, ("",))[0] in STATUS:
        verdict, tag = holm[pid]
        return f"{pid} primary ({tag})", verdict
    return f"{pid} primary (95%, before Holm)", v95


def _secondary(label: str, v: str) -> tuple:
    return f"{label} (95%)", v


def _note(ax, text: str, x: float = 0.02, y: float = 0.03, ha: str = "left", va: str = "bottom"):
    ax.text(x, y, text, transform=ax.transAxes, ha=ha, va=va, fontsize=6.4, color=INK2, zorder=5)


# --- headline: the model recovers BCS (P1, P4) -----------------------------------------

@_styled
def headline_bcs_recovery(exp9, exp10, holm=None):
    """P1's race-loss share against (N-1)/N in every cell, and P4's dissipated rent
    against floor(R/c) c / R at each cost: the closed forms the event model must recover."""
    fig, (a, b) = plt.subplots(1, 2, figsize=(7.2, 3.1), constrained_layout=True)
    _race_loss_vs_n(a, exp9, holm)
    _dissipation(b, exp10, holm)
    return fig


def _race_loss_vs_n(ax, s, holm):
    cells = s["final"]["cells"]
    ratios = sorted({c["ratio"] for c in cells})
    ns = sorted({c["n_firms"] for c in cells})
    grid = np.geomspace(min(ns) * 0.85, max(ns) * 1.15, 200)
    half = _half(cells[0]["race_loss"]["band"])
    ax.fill_between(grid, 1 - 1 / grid - half, 1 - 1 / grid + half, **BAND,
                    label=f"pre-registered band ±{half:g}")
    ax.plot(grid, 1 - 1 / grid, **THEORY, label="BCS: (N − 1) / N")
    for j, r in enumerate(ratios):
        for i, c in enumerate(c for c in cells if c["ratio"] == r):
            _interval(ax, c["n_firms"] * math.exp(_dodge(j, len(ratios), 0.07)), c["race_loss"],
                      RAMP3[j % 3], MARKERS[j % 4], label=f"λj / λi = {r:g}" if i == 0 else None)
    ax.set_xscale("log")
    ax.set_xticks(ns, [str(n) for n in ns])
    ax.xaxis.set_minor_locator(NullLocator())
    ax.set_xlabel("firms N (one liquidity provider, N − 1 snipers)")
    ax.set_ylabel("race-loss share (LP's cancel loses the race)")
    ax.legend(loc="lower right")
    _title(ax, "(a) Race-loss share against BCS's (N − 1)/N")
    _badges(ax, [_primary("P1", s["p1"]["primary_race_loss"], holm)])
    _note(ax, "95% Wilson intervals, pooled over seeds", y=0.82, va="top")


def _sawtooth(lo: float, hi: float):
    x = np.linspace(lo, hi, 4000)
    y = np.floor(x) / x
    y[1:][np.diff(np.floor(x)) > 0] = np.nan      # no vertical stroke at each integer
    return x, y


def _dissipation(ax, s, holm):
    p4 = s["p4"]
    cells = sorted(p4["H4b"]["cells"], key=lambda c: c["rent_multiple"])
    x, y = _sawtooth(2.0, cells[-1]["rent_multiple"] + 1.5)
    ax.plot(x, y, **THEORY, label="BCS free entry: ⌊R/c⌋ c / R")
    h4a = next((c for c in cells if c["theory_n_star"] == p4["H4a"]["prediction_n_star"]), None)
    if h4a is not None:
        lo, hi = p4["H4a"]["waste_band"]
        ax.add_patch(Rectangle((h4a["rent_multiple"] - 0.22, lo), 0.44, hi - lo, **BAND))
    for i, c in enumerate(cells):
        _interval(ax, c["rent_multiple"], c["waste_share"], BLUE,
                  label="simulated, 95% interval" if i == 0 else None)
        ax.annotate(f"N* = {c['entry']['n_star']}", (c["rent_multiple"], c["waste_share"]["upper"]),
                    xytext=(5, 4), textcoords="offset points", fontsize=6.8, color=INK2)  # off the line
    handles, labels = ax.get_legend_handles_labels()
    ax.legend(handles + [Patch(color=GRID)], labels + ["P4 band 0.909 ± 0.05"], loc="center right",
              bbox_to_anchor=(1.0, 0.3))
    ax.set_ylim(0.55, 1.04)
    ax.set_xlabel("rent over the cost of speed, R / c")
    ax.set_ylabel("rent dissipated on speed, N* c / R")
    _title(ax, "(b) Free entry dissipates ⌊R/c⌋ c / R of the rent")
    _badges(ax, [_primary("P4", p4["H4a"]["verdict"], holm),
                 _secondary("H4b", p4["H4b"]["verdict"])], "lower left")


# --- validation: P1 residuals, P3 ----------------------------------------------------------

@_styled
def validation_p1_p3(exp9, exp17, holm=None):
    """P1's twelve cells as residuals inside their bands (race loss, h-hat / h*), and
    P3's profit per successful snipe against the half-spread, jumps and random walk."""
    fig, axes = plt.subplots(1, 3, figsize=(7.2, 2.9), constrained_layout=True,
                             gridspec_kw={"width_ratios": [1, 1, 1.15]})
    cells, p1 = exp9["final"]["cells"], exp9["p1"]
    _cell_residuals(axes[0], cells, "race_loss", lambda c: c["race_loss"]["theory"], lambda c: 1.0)
    axes[0].set_ylabel("race-loss share − (N − 1)/N")
    axes[0].legend(loc="lower right", ncols=3, handletextpad=0.2, columnspacing=0.8)
    _title(axes[0], "(a) Race loss, residual")
    _badges(axes[0], [_primary("P1", p1["primary_race_loss"], holm)])
    _cell_residuals(axes[1], cells, "h_hat", lambda c: c["h_star"], lambda c: 100 / c["h_star"])
    axes[1].set_ylabel("ĥ / h* − 1 (%)")
    _title(axes[1], "(b) Indifference spread ĥ vs h*")
    _badges(axes[1], [_secondary("ĥ within ±3%", p1["h_hat_all_cells"]),
                      _secondary("ĥ flat in N", p1["h_hat_slope"])])
    _p3_per_snipe(axes[2], exp17, holm)
    return fig


def _cell_residuals(ax, cells, key, ref, scale):
    ratios = sorted({c["ratio"] for c in cells})
    ns = sorted({c["n_firms"] for c in cells})
    for c in cells:
        j, i = ratios.index(c["ratio"]), ns.index(c["n_firms"])
        r, k = ref(c), scale(c)
        lo, hi = ((v - r) * k for v in c[key]["band"])
        ax.add_patch(Rectangle((i - 0.38, lo), 0.76, hi - lo, **BAND))
        _interval(ax, i + _dodge(j, len(ratios), 0.22), c[key], RAMP3[j % 3], MARKERS[j % 4],
                  label=f"λj/λi = {c['ratio']:g}" if i == 0 else None, shift=r, scale=k)
    ax.axhline(0, **THEORY)
    ax.set_xticks(range(len(ns)), [f"N = {n}" for n in ns])
    ax.set_xlim(-0.6, len(ns) - 0.4)
    bands = [abs(v - ref(c)) * scale(c) for c in cells for v in c[key]["band"]]
    ax.set_ylim(-1.9 * max(bands), 1.9 * max(bands))


def _p3_per_snipe(ax, s, holm):
    h3a, h3b = s["H3a"], s["H3b"]
    keys = sorted(h3b["laplace_per_snipe"], key=float)
    hs = [float(h) for h in keys]
    lap = [h3b["laplace_per_snipe"][h] for h in keys]
    ax.axhspan(*lap[0]["band"], **BAND, label="b ± 5%")
    ax.axhline(lap[0]["theory"], **THEORY, label="closed form")
    ax.plot(hs, [h3b["rw_per_snipe_theory"][h] for h in keys], **THEORY)
    for i, (h, key) in enumerate(zip(hs, keys)):
        _interval(ax, h, lap[i], BLUE, "o", label="Laplace jumps" if i == 0 else None)
        _interval(ax, h, h3b["rw_per_snipe"][key], ORANGE, "s",
                  label="Gaussian random walk" if i == 0 else None)
    ax.set_ylim(0, lap[0]["band"][1] * 1.35)
    ax.set_xlabel("half-spread h (ticks)")
    ax.set_ylabel("profit per successful snipe (ticks)")
    ax.legend(loc="upper left", bbox_to_anchor=(0.0, 0.66))
    _title(ax, "(c) Jumps against a random walk")
    _badges(ax, [_primary("P3", h3a["verdict"], holm), _secondary("H3b", h3b["verdict"])],
            "upper right")
    _note(ax, f"rent rate ratio, jumps ÷ walk, at h = {h3a.get('h', 567)}:\n"
              f"{_est(h3a):.3f} [{h3a['lower']:.3f}, {h3a['upper']:.3f}]; "
              f"predicted {h3a['theory']:.2f} ± 10%", y=0.19)


# --- the race (P2) ---------------------------------------------------------------------

@_styled
def race_p2(exp16, holm=None):
    """H2a's 16 cells against P_win(lag, eps, k) with its band, beside H2d's maker-gains
    count against the earlier maker's latency (paper band of record, tape band beside)."""
    fig, (a, b) = plt.subplots(1, 2, figsize=(7.2, 3.2), constrained_layout=True,
                               gridspec_kw={"width_ratios": [1.3, 1]})
    eps = op.JITTER_NS
    _race_grid(a, exp16["H2a"]["cells"], x16.K_RACE, x16.LAG_FRACS,
               lambda lf, k: race_win_probability(lf * eps, eps, k), {"loc": "center right"})
    a.set_ylabel("maker's cancel first (share of snipe-able jumps)")
    a.set_xlabel(f"maker's lag ℓ / ε (ε = {eps / 1e3:g} µs race jitter)")
    _title(a, "(a) Cancel-first share against P_win(ℓ, ε, k)")
    _badges(a, [_primary("P2", exp16["H2a"]["verdict"], holm)], "upper right")
    _h2d(b, exp16["H2d"])
    return fig


def _cell_key(k, lf) -> str:
    return f"k={k},lag/eps={lf}"            # the drivers' (exp16 h2a, exp20 h7a) cell names


def _race_grid(ax, cells, ks, lags, theory, legend: dict):
    grid = np.linspace(0, max(lags) * 1.07, 300)
    for i, k in enumerate(ks):
        color = RAMP4[i % 4]
        half = _half(cells[_cell_key(k, lags[0])]["band"])
        th = np.array([theory(lf, k) for lf in grid])
        ax.fill_between(grid, np.clip(th - half, 0, None), th + half, color=color, alpha=0.13, lw=0,
                        zorder=1)
        ax.plot(grid, th, color=color, lw=1.0, zorder=2)
        for j, lf in enumerate(lags):
            _interval(ax, lf + _dodge(i, len(ks), 0.025), cells[_cell_key(k, lf)], color,
                      MARKERS[i % 4], label=f"snipers k = {k}" if j == 0 else None)
        ax.annotate(f"k = {k}", (0, th[0]), xytext=(-7, 0), textcoords="offset points",
                    ha="right", va="center", fontsize=6.6, color=INK2)
    ax.set_xlim(-0.3, grid[-1])
    ax.set_xticks(lags, [f"{lf:g}" for lf in lags])
    ax.legend(**legend, title="lines: closed form ± band", title_fontsize=6.4)


def _h2d(ax, h2d):
    by = h2d["by_band"]
    lats = sorted(by["paper"]["counts"], key=float)
    n_cells = next(iter(by["paper"].get("by_latency_ms", {}).values()), {}).get("n_cells", 18)
    for band, color, marker, label in (("tape", MUTED, "s", "corrected tape band"),
                                       ("paper", BLUE, "o", "paper band (of record)")):
        if band in by:
            ax.plot([float(x) for x in lats], [by[band]["counts"][x] for x in lats], marker + "-",
                    color=color, lw=1.6, ms=5, mec=SURFACE, mew=0.9, label=label, zorder=3)
    lo = float(lats[0])
    ax.hlines(x16.FAST_HIT_MAX, lo, x16.FAST_MS, **THEORY)
    ax.annotate(f"HIT: ≤ {x16.FAST_HIT_MAX} at ≤ {x16.FAST_MS:g} ms", (lo, x16.FAST_HIT_MAX),
                xytext=(0, 4), textcoords="offset points", fontsize=6.4, color=INK2)
    ax.hlines(x16.SLOW_HIT_MIN, x16.SLOW_MS / 2.5, x16.SLOW_MS * 1.6, **THEORY)
    ax.annotate(f"HIT: ≥ {x16.SLOW_HIT_MIN} at {x16.SLOW_MS:g} ms", (x16.SLOW_MS * 1.6, x16.SLOW_HIT_MIN),
                xytext=(0, 4), textcoords="offset points", ha="right", fontsize=6.4, color=INK2)
    ax.set_xscale("log")
    ax.set_xticks([float(x) for x in lats], [f"{float(x):g}" for x in lats])
    ax.xaxis.set_minor_locator(NullLocator())
    ax.set_ylim(-0.5, n_cells + 0.5)
    ax.yaxis.set_major_locator(MaxNLocator(integer=True))
    ax.set_xlabel("earlier maker's one-way latency (ms)")
    ax.set_ylabel(f"maker-gains cells of {n_cells}\n(95% interval of maker P&L change > 0)")
    ax.legend(loc="center left", bbox_to_anchor=(0.0, 0.62))
    _title(ax, "(b) Earlier maker: gains only when slow")
    _badges(ax, [_secondary("H2d, paper band", h2d["verdict"])])


# --- incidence among makers (P8, P10, P10b) ------------------------------------------------

@_styled
def makers_p8_p10(exp12, exp15, holm=None):
    """P8's paired Nash half-spread steps in M and M = 5 against breakeven; P10's and
    P10b's hit rates over held-out configurations (paper band of record, tape beside)."""
    fig = plt.figure(figsize=(7.2, 5.4), constrained_layout=True)
    gs = fig.add_gridspec(2, 2, width_ratios=[1.7, 1], height_ratios=[1, 1.15])
    _p8_steps(fig.add_subplot(gs[0, 0]), exp12, holm)
    _p8_level(fig.add_subplot(gs[0, 1]), exp12)
    _p10(fig.add_subplot(gs[1, :]), exp15, holm)
    return fig


def _p8_steps(ax, s, holm):
    h = s["p8"]["H8a"]
    for i, st in enumerate(h["steps"]):
        _interval(ax, i, st["diff"], BLUE)
    ax.axhline(0, **THEORY)
    ends = [0.0] + [v for st in h["steps"] if st["diff"] for v in (st["diff"]["lower"], st["diff"]["upper"])]
    span = (max(ends) - min(ends)) or 1.0
    ax.set_ylim(min(ends) - 0.25 * span, max(ends) + 0.35 * span)
    ax.set_xticks(range(len(h["steps"])), [f"M = {st['from_M']} → {st['to_M']}" for st in h["steps"]])
    ax.set_xlim(-0.6, len(h["steps"]) - 0.4)
    ax.set_ylabel("paired change in Nash quoted\nhalf-spread (ticks)")
    _title(ax, "(a) Nash half-spread as makers are added")
    _badges(ax, [_primary("P8", h["verdict"], holm)], "upper right")
    _note(ax, "non-increasing: every 95% upper bound ≤ 0")


def _p8_level(ax, s):
    h = s["p8"]["H8a"]
    ax.axhspan(*h["band"], **BAND, label="breakeven ± 20%")
    ax.axhline(h["breakeven_ticks"], **THEORY, label="BCS breakeven, N = k + 1")
    ax.axhline(min(s["space"]["half_spreads_ticks"]), color=MUTED, lw=1.0, ls=":",
               label="smallest half-spread on the grid")
    _interval(ax, 0, h["top_M_half_spread_ticks"], BLUE)
    top = (h["top_M_half_spread_ticks"] or {}).get("upper") or h["band"][1]
    ax.set_ylim(0, 1.6 * max(top, h["band"][1], min(s["space"]["half_spreads_ticks"])))
    ax.set_xticks([0], [f"M = {h['top_M']}"])
    ax.set_xlim(-1, 1)
    ax.set_ylabel("Nash quoted half-spread (ticks)")
    ax.legend(loc="lower center", fontsize=6.2)
    _title(ax, f"(b) M = {h['top_M']} against breakeven")
    _badges(ax, [_secondary(f"M = {h['top_M']} clause", h["top_M_verdict"])])


P10_ROWS = (("slowest maker pays", "p10", "hit_rate"),
            ("Spearman ρ(latency, P&L change) < 0", "p10", "spearman_negative_share"),
            ("slowest pays, closely spaced sets", "p10", "closely_spaced_hit_rate"),
            ("P10b queue front pays, where named", "p10b", "hit_rate_where_named"),
            ("P10b queue front pays, literal", "p10b", "hit_rate"))


def _p10(ax, s, holm):
    bands = s["bands"]
    for band, dy, color, marker, label in (("paper", -0.14, BLUE, "o", "paper band (of record)"),
                                           ("tape", 0.14, MUTED, "s", "corrected tape band")):
        for i, (_, part, key) in enumerate(P10_ROWS):
            if band in bands:
                _interval(ax, i + dy, bands[band][part].get(key), color, marker, horizontal=True,
                          label=label if i == 0 else None)
    _rows(ax, [r[0] for r in P10_ROWS], headroom=1.0, footroom=0.7)
    ax.axvline(x15.HIT_BAND, **THEORY)
    ax.annotate(f"HIT: lower bound ≥ {x15.HIT_BAND:g}", (x15.HIT_BAND, 0), xycoords=("data", "axes fraction"),
                xytext=(-3, 3), textcoords="offset points", ha="right", fontsize=6.4, color=INK2)
    chance = bands["paper"]["p10"].get("chance_rate")
    if chance is not None:
        ax.axvline(chance, color=MUTED, lw=1.0, ls=":")
        ax.annotate("chance, mean 1/M", (chance, 0), xycoords=("data", "axes fraction"),
                    xytext=(3, 3), textcoords="offset points", fontsize=6.4, color=INK2)
    ax.set_xlim(0, 1)
    ax.set_xlabel("share of held-out latency configurations (Wilson 95%)")
    ax.legend(loc="upper right", ncols=2, frameon=True, facecolor=SURFACE, edgecolor="none",
              framealpha=1)
    _title(ax, "(c) Who pays among makers")
    paper = bands["paper"]
    _badges(ax, [_primary("P10", paper["p10"]["verdict"], holm),
                 _secondary("P10b, where named", paper["p10b"].get("verdict_where_named")
                            or paper["p10b"]["verdict"])])


# --- welfare and waste (P5, P6, P7) --------------------------------------------------------

@_styled
def welfare_p5_p7(exp18, exp19, exp20, holm=None):
    """P5's four fee ratios against their closed forms, P6's participation ratio and
    deadweight per arm, and P7's 16 cells against 1 - P_win with its band."""
    fig, ((a, b), (c, d)) = plt.subplots(2, 2, figsize=(7.2, 6.0), constrained_layout=True)
    _p5(a, exp18, holm)
    _p6(b, c, exp19, holm)
    eps = op.JITTER_NS
    _race_grid(d, exp20["H7a"]["cells"], x20.K_RACE, x20.LAG_FRACS,
               lambda lf, k: 1 - race_win_probability(lf * eps, eps, k),
               {"loc": "lower right", "bbox_to_anchor": (1.0, 0.1)})
    d.set_ylabel("maker loss per snipe-able jump,\ncancelling ÷ non-cancelling")
    d.set_xlabel(f"maker's lag ℓ / ε (ε = {eps / 1e3:g} µs)")
    _title(d, "(d) Cancelling saves P_win of the loss")
    _badges(d, [_primary("P7", exp20["H7a"]["verdict"], holm)], "lower right")
    return fig


def _p5(ax, s, holm):
    tests, rows = s["H5a"]["tests"], []
    for f in x18.H5A_FT:
        rows += [(f"snipes per jump\n÷ e^(−f_t/b), f_t = {f:g}", tests[f"snipes_ratio_f{f:g}"]),
                 (f"net profit per snipe\n÷ b, f_t = {f:g}", tests[f"net_per_snipe_f{f:g}"])]
    first = rows[0][1]
    ax.axvspan(*(v / first["theory"] for v in first["band"]), **BAND, label="±5% band")
    ax.axvline(1, **THEORY)
    for i, (_, t) in enumerate(rows):
        _interval(ax, i, t, BLUE, horizontal=True, scale=1 / t["theory"])
    _rows(ax, [r[0] for r in rows], headroom=0.9)
    ends = [v / t["theory"] for _, t in rows for v in (t["lower"], t["upper"])]
    ends += [v / first["theory"] for v in first["band"]]
    pad = 0.15 * (max(ends) - min(ends))
    ax.set_xlim(min(ends) - pad, max(ends) + pad)
    ax.set_xlabel(f"measured ÷ closed form (fees in ticks, b = {x18.B:g} ticks)")
    _title(ax, "(a) Fees: snipes fall, profit per snipe stays b")
    _badges(ax, [_primary("P5", s["H5a"]["verdict"], holm)])


def _rows(ax, labels, headroom: float = 1.2, footroom: float = 0.0) -> None:
    """Categorical rows top to bottom, with an empty band on top for badges and legends."""
    ax.set_yticks(range(len(labels)), labels)
    ax.set_ylim(len(labels) - 0.5 + footroom, -0.5 - headroom)
    ax.grid(axis="x")
    ax.grid(axis="y", visible=False)


def _p6(ax_part, ax_dwl, s, holm):
    arms = s["H6a"]["by_arm"]
    names = list(arms)
    for ax, key in ((ax_part, "participation_over_predicted"), (ax_dwl, "deadweight_usd_per_s")):
        for i, arm in enumerate(names):
            for band, dy, color, marker, label in (("paper", -0.12, BLUE, "o", "paper band (of record)"),
                                                   ("tape", 0.12, MUTED, "s", "corrected tape band")):
                if band in arms[arm]:
                    _interval(ax, i + dy, arms[arm][band][key], color, marker, horizontal=True,
                              label=label if i == 0 else None)
        _rows(ax, [f"design elasticity {a.split('=')[-1]}\nv̄ = {arms[a]['vbar_ticks']:.0f} ticks"
                   for a in names], headroom=1.3)
    ax_dwl.legend(loc="upper right")                  # one legend serves both panels
    ax_part.axvspan(1 - x19.PART_BAND, 1 + x19.PART_BAND, **BAND)
    ax_part.set_xlim(min(ax_part.get_xlim()[0], 1 - 1.5 * x19.PART_BAND),
                     max(ax_part.get_xlim()[1], 1 + 1.5 * x19.PART_BAND))
    ax_part.axvline(1, **THEORY)
    ax_part.set_xlabel("participation ÷ predicted e^(−(h̄_HFT − h̄₀)/v̄)")
    _title(ax_part, "(b) Participation with HFTs present")
    _badges(ax_part, [_primary("P6", s["H6a"]["verdict"], holm)])
    ax_dwl.axvline(0, **THEORY)
    ax_dwl.set_xlabel(f"deadweight loss ({MONEY})")
    _title(ax_dwl, "(c) Deadweight loss from HFT presence")
    _note(ax_dwl, "P6 needs the interval above 0", x=0.98, ha="right")


# --- market design (P9) --------------------------------------------------------------------

@_styled
def batch_p9(exp11, holm=None):
    """The fast maker's sniping loss against the batch interval, against continuous
    trading and 1.2 x BCS footnote 34's bound lambda_j (eps / 2 tau) prize(h)."""
    fig, ax = plt.subplots(figsize=(4.8, 3.3), constrained_layout=True)
    arms, p9 = exp11["arms"], exp11["p9"]
    cont = arms[x11._arm_key("fast", "stale", x11.CONTINUOUS)]["maker_sniping_loss"]
    taus = np.array(x11.CONFIRM_TAUS_NS, float)
    grid = np.geomspace(taus[0] / 2, taus[-1] * 2, 100)
    ax.axhspan(cont["lower"], cont["upper"], color=ORANGE, alpha=0.15, lw=0, zorder=1)
    ax.axhline(_est(cont), color=ORANGE, lw=1.6, zorder=2, label="continuous trading")
    ax.plot(grid / 1e6, [1.2 * x11.footnote34_bound(t) for t in grid], **THEORY,
            label="1.2 × BCS footnote-34 bound")
    batch = [arms[x11._arm_key("fast", "stale", (int(t), "time", "engine"))]["maker_sniping_loss"]
             for t in taus]
    ax.plot(taus / 1e6, [_est(b) for b in batch], color=BLUE, lw=1.2, zorder=2)
    for i, (t, b) in enumerate(zip(taus, batch)):
        _interval(ax, t / 1e6, b, BLUE, label="frequent batch auction" if i == 0 else None)
    ax.axhline(0, color=AXIS, lw=0.8, zorder=1)
    ax.set_xscale("log")
    ax.set_yscale("symlog", linthresh=1e-3, linscale=0.6)
    ax.set_ylim(min(-0.01, 1.5 * min(b["lower"] for b in batch)), 4 * max(cont["upper"], 1e-3))
    ax.set_xticks(taus / 1e6, [f"{t / 1e6:g}" for t in taus])
    ax.xaxis.set_minor_locator(NullLocator())
    ax.set_xlabel("batch interval τ (ms)")
    ax.set_ylabel(f"maker's sniping loss\n({MONEY})")
    ax.legend(loc="center left", bbox_to_anchor=(0.0, 0.62))
    _title(ax, "Batch auctions against continuous trading")
    _badges(ax, [_primary("P9", p9["H9a"]["verdict"], holm),
                 _secondary("H9c no backfire", p9["H9c"]["verdict"])], "lower left")
    _note(ax, "symmetric log scale, linear within ±0.001", x=0.98, y=0.97, ha="right", va="top")
    return fig


# --- sensitivity (P11) ---------------------------------------------------------------------

FACTOR_LABELS = {
    "jump_rate": "jump rate λj", "jump_scale_bp": "jump scale b", "sniper_lat_ns": "sniper latency",
    "lag_over_eps": "maker lag ℓ / ε", "noise_rate": "investor arrival rate",
    "noise_eps_bp": "investor value scale", "half_spread_ticks": "maker half-spread",
    "adverse_sensitivity": "adverse sensitivity", "decay_per_s": "spread decay",
    "snipe_ratio": "snipe-to-quote ratio", "fee_over_b": "taker fee ÷ b",
    "n_snipers": "snipers k", "n_makers": "makers M", "seed": "seed (run noise)",
}


@_styled
def sensitivity_p11(exp13, holm=None):
    """First-order indices of sniper rent (H11a: latency below 0.05, lambda_j and b
    together at least half) and total-order indices of the maker's P&L change (H11b)."""
    fig, (a, b) = plt.subplots(1, 2, figsize=(7.2, 3.8), constrained_layout=True)
    p11, sob = exp13["p11"], exp13["sobol"]
    _sobol(a, sob["sniper_rent"]["indices"], "S1",
           {"sniper_lat_ns": BLUE, "jump_rate": ORANGE, "jump_scale_bp": ORANGE})
    a.axvline(0.05, **THEORY)
    jb = p11["H11a"]["S1_jump_rate_plus_b"]
    _note(a, f"S1(λj) + S1(b) = {_est(jb):.2f} [{jb['lower']:.2f}, {jb['upper']:.2f}];\n"
             "P11 needs its lower bound ≥ 0.5", x=0.98, ha="right")
    a.legend([Line2D([], [], color=BLUE, marker="o", lw=1.4), Line2D([], [], color=ORANGE, marker="o", lw=1.4),
              Line2D([], [], **THEORY)],
             ["sniper latency: P11 needs < 0.05", "λj and b: together ≥ 0.5", "0.05"],
             loc="lower right", bbox_to_anchor=(1.0, 0.13))
    a.set_xlabel(f"first-order Sobol index S1, sniper rent (95%, N = {exp13.get('n_base', 512)})")
    _title(a, "(a) Sniper rent: what moves it")
    _badges(a, [_primary("P11", p11["H11a"]["verdict"], holm)])
    top3 = p11["H11b"].get("top3_total_order", [])
    _sobol(b, sob["maker_delta"]["indices"], "ST",
           {"adverse_sensitivity": BLUE, "snipe_ratio": BLUE}, bold=top3, headroom=2.4)
    b.set_xlabel("total-order index ST, maker P&L change (95%)")
    _title(b, "(b) Maker P&L change (bold: top three)")
    _badges(b, [_secondary("H11b", p11["H11b"]["verdict"]),
                _secondary("H11c", p11.get("H11c", {}).get("verdict"))])
    return fig


def _sobol(ax, table, which, accent, bold=(), headroom: float = 1.3):
    names = sorted(table, key=lambda n: table[n][which], reverse=True)
    for i, n in enumerate(names):
        r = table[n]
        _interval(ax, i, {"mean": r[which], "lower": r[f"{which}_lo"], "upper": r[f"{which}_hi"]},
                  accent.get(n, MUTED), horizontal=True)
    _rows(ax, [FACTOR_LABELS.get(n, n) for n in names], headroom=headroom)
    for label, n in zip(ax.get_yticklabels(), names):
        if n in bold:
            label.set_fontweight("bold")
    ax.axvline(0, color=AXIS, lw=0.8, zorder=1)


# --- CLI -------------------------------------------------------------------------------

FIGURES = {
    "headline_bcs_recovery": (headline_bcs_recovery, ("exp9_bcs_replication", "exp10_speed_entry")),
    "validation_p1_p3": (validation_p1_p3, ("exp9_bcs_replication", "exp17_jump_vs_walk")),
    "race_p2": (race_p2, ("exp16_race_incidence",)),
    "makers_p8_p10": (makers_p8_p10, ("exp12_endogenous_makers", "exp15_heldout_who_pays")),
    "welfare_p5_p7": (welfare_p5_p7, ("exp18_fee_schedules", "exp19_noise_heterogeneity",
                                      "exp20_maker_strategies")),
    "batch_p9": (batch_p9, ("exp11_batch_semantics",)),
    "sensitivity_p11": (sensitivity_p11, ("exp13_global_sensitivity",)),
}
STAMP = "NOT CONFIRMATORY: smoke or scratch input, layout check only"


def load_holm(report: Path | None, results_dir: Path) -> dict | None:
    """{pid: (Holm verdict, tag)} from confirmatory_report's JSON, if it was built from
    results_dir; otherwise None, and the badges show the drivers' 95% verdicts."""
    if report is None or not Path(report).exists():
        return None
    r = json.loads(Path(report).read_text())
    if Path(r.get("results_dir", "")).resolve() != Path(results_dir).resolve():
        print(f"note: {report} was built from {r.get('results_dir')}, not {results_dir}; ignored")
        return None
    tag = "Holm" if r.get("complete") else "Holm, partial batch"
    return {e["pid"]: (e["holm_verdict"], tag) for e in r["primaries"]}


def _inputs(results_dir: Path, drivers, allow_smoke: bool) -> tuple:
    """(summaries, problems, any_not_confirmatory) for one figure's drivers."""
    summaries, problems, smoke = [], [], False
    for name in drivers:
        path = Path(results_dir) / f"{name}.json"
        if not path.exists():
            problems.append(f"{path.name} missing")
            continue
        s = json.loads(path.read_text())
        if s.get("status") != op.CONFIRMATORY_STATUS:
            smoke = True
            if not allow_smoke:
                problems.append(f"{path.name} not confirmatory (status: {s.get('status', '<none>')})")
        summaries.append(s)
    return summaries, problems, smoke


def render_all(results_dir: Path, out_dir: Path, holm: dict | None = None,
               allow_smoke: bool = False) -> dict:
    """Render every figure whose inputs exist; {name: rendered | smoke | skipped | failed}."""
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    result = {}
    for name, (fn, drivers) in FIGURES.items():
        summaries, problems, smoke = _inputs(results_dir, drivers, allow_smoke)
        if problems:
            print(f"SKIP {name}: " + "; ".join(problems))
            result[name] = "skipped"
            continue
        try:
            fig = fn(*summaries, holm=holm)
        except (KeyError, TypeError, ValueError, IndexError) as exc:   # a schema it does not know
            print(f"FAILED {name}: {type(exc).__name__}: {exc}", file=sys.stderr)
            result[name] = "failed"
            continue
        if smoke:
            fig.suptitle(STAMP, fontsize=8, color=INK,
                         bbox={"boxstyle": "round,pad=0.3", "fc": SURFACE, "ec": STATUS["MISS"], "lw": 1.2})
        for ext in ("png", "svg"):
            fig.savefig(out_dir / f"{name}.{ext}", dpi=200, bbox_inches="tight", pad_inches=0.04)
        plt.close(fig)
        result[name] = "smoke" if smoke else "rendered"
        print(f"wrote {out_dir / name}.png and .svg" + (" (NOT CONFIRMATORY)" if smoke else ""))
    return result


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--results-dir", type=Path, default=op.RESULTS_DIR)
    ap.add_argument("--out-dir", type=Path, default=OUT_DIR)
    ap.add_argument("--report", type=Path, default=OUT_MD.with_suffix(".json"),
                    help="confirmatory_report.py's JSON, for the primaries' Holm verdicts")
    ap.add_argument("--allow-smoke", action="store_true",
                    help="render non-confirmatory inputs too, stamped (layout checks only)")
    a = ap.parse_args(argv)
    holm = load_holm(a.report, a.results_dir)
    print("primary badges: " + (f"Holm verdicts from {a.report}" if holm
                                else "the drivers' 95% verdicts (no report for this results dir)"))
    result = render_all(a.results_dir, a.out_dir, holm, a.allow_smoke)
    return 1 if "failed" in result.values() else 0


if __name__ == "__main__":
    sys.exit(main())
