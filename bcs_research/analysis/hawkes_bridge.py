#!/usr/bin/env python3
"""Experiment 5 — Hawkes bridge: does endogenous excitation rise before gaps?

Tests whether the Hawkes branching ratio n(t) of the trade-arrival process is
elevated in the windows that PRECEDE an endogenous liquidity-gap event, relative
to normal windows. In simulation the ground truth is known (the gap detector
already filters to V-flat, endogenous gaps), so a positive result grounds the
real-data interpretation of a rising n(t) (Filimonov & Sornette 2012).

Pipeline per simulation (operating point, longer run to accumulate gap events):
  1. run the verified-engine harness, take the trade tape and the gap events;
  2. de-tie intra-tick trade timestamps and slide a fixed-count window
     (`window` trades, `stride` step) along the tape, MLE-fitting a univariate
     Hawkes process in each window -> a branching ratio n at the window's end;
  3. label each window pre-gap (a gap starts within `lead` after the window end)
     or normal;
  4. Mann-Whitney U test (one-sided, pre > normal) across all windows, with the
     rank-biserial effect size.

Methodological note: the plan specified a paired Wilcoxon signed-rank test, but
pre-gap and normal windows are INDEPENDENT samples of unequal size (the paired
test would require truncating to equal length and assumes pairing that does not
exist). The Mann-Whitney U / Wilcoxon rank-sum test is the correct test for two
independent groups and is used here.

Run: bcs_research/.venv/bin/python bcs_research/analysis/hawkes_bridge.py
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_ROOT = _HERE.parent
for _d in ("build", "agents", "simulation", "metrics", "experiments", "hawkes"):
    _p = str(_ROOT / _d)
    if _p not in sys.path:
        sys.path.insert(0, _p)

import numpy as np                                       # noqa: E402
from scipy.stats import mannwhitneyu                     # noqa: E402

import bcs_engine as be                                  # noqa: E402
from fundamental_value import FundamentalValueProcess    # noqa: E402
from noise_trader import NoiseTrader                     # noqa: E402
from bcs_market_maker import BCSMarketMaker              # noqa: E402
from hft_agent import HFTAgent                           # noqa: E402
from scheduler import LatencyScheduler                   # noqa: E402
from liquidity_gap_detector import detect_liquidity_gaps  # noqa: E402
from calibrator import calibrate_univariate              # noqa: E402

from exp1_primary import CFG, MM_ID, SYMBOL              # noqa: E402

WINDOW_TRADES = 500
STRIDE_TRADES = 100
LEAD_TICKS = 500
DURATION_TICKS = 50_000
N_SIMS = 5            # pilot default; full run uses 50
BRANCHING_MAX = 1.0   # reject explosive/degenerate (non-stationary) Hawkes fits


# --- pure helpers (unit tested) ----------------------------------------------

def detie_trade_times_us(trade_t, dt_us):
    """Spread trades that share a tick timestamp evenly across [t, t+dt_us).

    The engine stamps every fill within one tick with the same `t` (microseconds
    on the dt_us grid); a Hawkes MLE needs strictly increasing times. Ties are
    spread by within-tick arrival order so the sequence is strictly increasing
    while staying inside the originating tick. Distinct timestamps are untouched.
    """
    ts = np.asarray(trade_t, dtype=float).ravel()
    if ts.size == 0:
        return ts
    out = ts.copy()
    i = 0
    n = ts.size
    while i < n:
        j = i
        while j < n and ts[j] == ts[i]:
            j += 1
        m = j - i
        if m > 1:
            out[i:j] = ts[i] + (np.arange(m) / m) * dt_us
        i = j
    return out


def classify_pre_gap(branching, gap_starts_us, lead_us):
    """Split (end_t_us, n) windows into pre-gap and normal lists.

    A window is pre-gap iff some gap starts strictly after its end time and
    within `lead_us` of it (the window precedes the gap).
    """
    gaps = sorted(gap_starts_us)
    pre, normal = [], []
    for t_us, n in branching:
        is_pre = any(t_us < g <= t_us + lead_us for g in gaps)
        (pre if is_pre else normal).append(n)
    return pre, normal


def rank_biserial(group1, group2):
    """Rank-biserial correlation in [-1, 1]; >0 means group1 stochastically larger."""
    g1 = np.asarray(group1, dtype=float).ravel()
    g2 = np.asarray(group2, dtype=float).ravel()
    if g1.size == 0 or g2.size == 0:
        return float("nan")
    greater = float(np.sum(g1[:, None] > g2[None, :]))
    equal = float(np.sum(g1[:, None] == g2[None, :]))
    u1 = greater + 0.5 * equal
    return float(2.0 * u1 / (g1.size * g2.size) - 1.0)


# --- simulation + rolling calibration ----------------------------------------

def _simulate_tape(seed, cfg, with_hft=True):
    """Mirror of exp1._run agent setup, returning the raw SimResult (tape)."""
    v0 = 100 * be.PRICE_PRECISION
    h = be.EngineHarness()
    h.add_symbol(SYMBOL)
    h.start()
    fund = FundamentalValueProcess(v0=v0, sigma=cfg["sigma"], dt_us=cfg["dt_us"], seed=seed)
    mm = BCSMarketMaker(MM_ID, base_half_spread=cfg["base_half_spread"],
                        quote_qty=cfg["quote_qty"], latency_us=cfg["mm_latency_us"],
                        inventory_skew=cfg["mm_inventory_skew"],
                        adverse_sensitivity=cfg["adverse_sensitivity"],
                        spread_decay=cfg["spread_decay"],
                        spread_cap_mult=cfg["spread_cap_mult"],
                        min_quote_qty=cfg["min_quote_qty"])
    noise = [NoiseTrader(100 + i, lambda_per_tick=cfg["lambda_per_tick"],
                         qty=cfg["noise_qty"], seed=seed * 1000 + i)
             for i in range(cfg["n_noise"])]
    hfts = []
    if with_hft:
        hfts = [HFTAgent(200 + j, latency_us=cfg["hft_latency_us"],
                         race_noise_us=cfg["hft_race_noise_us"],
                         transaction_cost_bps=cfg["hft_cost_bps"],
                         qty=cfg["hft_qty"], seed=seed * 2000 + j)
                for j in range(cfg["n_hft"])]
    sched = LatencyScheduler(h, SYMBOL, cfg["dt_us"])
    result = sched.run([mm] + noise + hfts, fund, cfg["duration_us"])
    h.stop()
    return result


def rolling_branching_ratios(result, dt_us, window=WINDOW_TRADES, stride=STRIDE_TRADES):
    """Slide a fixed-count window along the trade tape; MLE a Hawkes n per window.

    Returns [(window_end_t_us, branching_ratio)] for converged fits only.
    """
    raw_us = [tr["t"] for tr in result.trades]
    if len(raw_us) < window:
        return []
    times_us = detie_trade_times_us(raw_us, dt_us)
    times_s = times_us / 1e6
    out = []
    for end in range(window, len(times_s) + 1, stride):
        w = times_s[end - window:end]
        fit = calibrate_univariate(w, T=w[-1])
        n_hat = fit.params.branching_ratio
        # Keep only stationary fits: a branching ratio >= 1 is explosive and in
        # practice signals a degenerate MLE (beta driven to the positivity floor,
        # so alpha/beta blows up). Drop these rather than let them corrupt means.
        if fit.converged and 0.0 < n_hat < BRANCHING_MAX:
            out.append((float(times_us[end - 1]), float(n_hat)))
    return out


def _mann_whitney(pre, normal):
    if len(pre) < 3 or len(normal) < 3:
        return {"U": None, "p_value": None, "rank_biserial": None,
                "pre_mean": (float(np.mean(pre)) if pre else None),
                "normal_mean": (float(np.mean(normal)) if normal else None),
                "n_pre": len(pre), "n_normal": len(normal),
                "note": "insufficient samples for the test"}
    u, p = mannwhitneyu(pre, normal, alternative="greater")
    return {"U": float(u), "p_value": float(p),
            "rank_biserial": rank_biserial(pre, normal),
            "pre_mean": float(np.mean(pre)), "normal_mean": float(np.mean(normal)),
            "n_pre": len(pre), "n_normal": len(normal)}


def run_bridge(n_sims=N_SIMS, duration_ticks=DURATION_TICKS, window=WINDOW_TRADES,
               stride=STRIDE_TRADES, lead_ticks=LEAD_TICKS, cfg=None, with_hft=True):
    cfg = {**CFG, **(cfg or {}), "duration_us": duration_ticks * CFG["dt_us"]}
    dt_us, sigma = cfg["dt_us"], cfg["sigma"]
    lead_us = lead_ticks * dt_us

    pre_all, normal_all, per_sim, total_gaps = [], [], [], 0
    for seed in range(1, n_sims + 1):
        result = _simulate_tape(seed, cfg, with_hft=with_hft)
        gaps = detect_liquidity_gaps(result.snapshots, dt_us, sigma)
        branching = rolling_branching_ratios(result, dt_us, window, stride)
        pre, normal = classify_pre_gap(branching, [g["start_t"] for g in gaps], lead_us)
        pre_all += pre
        normal_all += normal
        total_gaps += len(gaps)
        per_sim.append({
            "seed": seed, "n_trades": len(result.trades), "n_gaps": len(gaps),
            "n_windows": len(branching), "n_pre": len(pre), "n_normal": len(normal),
            "pre_mean": (float(np.mean(pre)) if pre else None),
            "normal_mean": (float(np.mean(normal)) if normal else None),
        })

    test = _mann_whitney(pre_all, normal_all)
    report = {
        "config": {"n_sims": n_sims, "duration_ticks": duration_ticks,
                   "window_trades": window, "stride_trades": stride,
                   "lead_ticks": lead_ticks, "with_hft": with_hft,
                   "operating_point": {k: cfg[k] for k in
                                       ("n_hft", "mm_latency_us", "adverse_sensitivity",
                                        "spread_decay", "sigma")}},
        "total_gap_events": total_gaps,
        "mann_whitney": test,
        "per_sim": per_sim,
    }

    out_dir = _ROOT / "results" / "experiments"
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "exp5_hawkes_bridge.json").write_text(json.dumps(report, indent=2, sort_keys=True))
    _print_summary(report)
    return report


def _print_summary(report: dict) -> None:
    t = report["mann_whitney"]
    c = report["config"]
    print(f"Experiment 5 — Hawkes bridge ({c['n_sims']} sims x {c['duration_ticks']} ticks, "
          f"window={c['window_trades']} trades, stride={c['stride_trades']}, lead={c['lead_ticks']} ticks)")
    print(f"  total endogenous gap events : {report['total_gap_events']}")
    print(f"  windows: {t['n_pre']} pre-gap / {t['n_normal']} normal")
    if t["U"] is None:
        print(f"  TEST SKIPPED: {t.get('note')}")
        return
    print(f"  branching ratio n(t): pre-gap {t['pre_mean']:.4f}  vs  normal {t['normal_mean']:.4f}")
    print(f"  Mann-Whitney U={t['U']:.1f}  p(one-sided, pre>normal)={t['p_value']:.4g}  "
          f"rank-biserial={t['rank_biserial']:.3f}")
    verdict = ("n(t) elevated before gaps (p<0.05)" if t["p_value"] < 0.05
               else "no significant elevation")
    print(f"  VERDICT: {verdict}")


def main():
    return run_bridge()


if __name__ == "__main__":
    main()
