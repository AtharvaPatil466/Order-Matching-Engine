#!/usr/bin/env python3
"""Experiment 5 (redesigned) — Hawkes endogeneity with the pilot's three fixes.

The pilot (analysis/hawkes_bridge.py, kept intact) was directionally positive
but not significant, and §4.5 of the paper names its design tensions. This
redesign implements the §5.3 corrections, simulation side:

1. TIME-based windows (WINDOW_TICKS wide, STRIDE_TICKS step) instead of
   500-trade windows that straddled multiple gaps. Windows CONTAINING a gap
   start are dropped outright — the pilot let them pollute "normal" — and the
   remainder are labelled pre-gap (a gap starts within LEAD_TICKS after the
   window ends) or normal.
2. QUOTE-update event process (per-side best-quote changes) fitted alongside
   the trade-arrival process. Gaps are mechanically a quote-thinning
   phenomenon, so the quote side is the natural place to look for
   self-excitation; the pilot's trade-arrival process is retained for
   comparison.
3. PLACEBO control: the same sims with large exogenous fundamental jumps at
   KNOWN times, and the same labelling machinery pointed at the shock times.
   Shocks are unpredictable from inside the market, so a sound pipeline must
   find NO pre-shock elevation; a "significant" placebo would mean the
   pre-gap result is a labelling artifact rather than endogeneity.

Still out of scope (per §5.3): the real-data leg — BTC books never empty, so
a depth-depletion gap definition is needed before the bridge can run on the
AlphaForge tape.

Run: bcs_research/.venv/bin/python bcs_research/analysis/exp5_redesigned.py
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_ROOT = _HERE.parent
for _d in ("build", "agents", "simulation", "metrics", "experiments", "hawkes",
           "analysis"):
    _p = str(_ROOT / _d)
    if _p not in sys.path:
        sys.path.insert(0, _p)

import numpy as np                                        # noqa: E402

import bcs_engine as be                                   # noqa: E402
from fundamental_value import FundamentalValueProcess     # noqa: E402
from liquidity_gap_detector import detect_liquidity_gaps  # noqa: E402
from calibrator import calibrate_univariate               # noqa: E402
from hawkes_bridge import (_mann_whitney, _simulate_tape,  # noqa: E402
                           detie_trade_times_us)
from exp1_primary import CFG                              # noqa: E402

WINDOW_TICKS = 200      # < mean gap spacing (~300 ticks at the operating point)
STRIDE_TICKS = 50
LEAD_TICKS = 100
DURATION_TICKS = 50_000
N_SIMS = 10             # per arm
SHOCK_EVERY_TICKS = 2_500
SHOCK_SIZE_SIGMAS = 50  # jump = 50 * sigma = 10 full base spreads
BRANCHING_MAX = 1.0     # stationarity filter, as in the pilot


class ShockedFundamental(FundamentalValueProcess):
    """Random walk plus deterministic jumps at known times (placebo arm).

    Jumps alternate sign so V does not trend away from the book. Applied
    inside step() so observe() latency semantics stay consistent.
    """

    def __init__(self, v0, sigma, dt_us=1000, seed=0,
                 shock_times_us=(), shock_size=0.0):
        super().__init__(v0, sigma, dt_us=dt_us, seed=seed)
        self.shock_times_us = sorted(int(s) for s in shock_times_us)
        self.shock_size = float(shock_size)
        self._next_shock = 0
        self._sign = 1

    def step(self, t):
        if (self._next_shock < len(self.shock_times_us)
                and int(t) >= self.shock_times_us[self._next_shock]):
            self.current_v += self._sign * self.shock_size
            self._sign = -self._sign
            self._next_shock += 1
        return super().step(t)


# --- pure helpers (unit tested) ----------------------------------------------

def quote_event_times_us(snapshots, dt_us):
    """Per-side best-quote change times, de-tied within the tick.

    One event per side per tick in which that side's best price changed
    (including appearing/vanishing). Both sides changing in one tick yield two
    events, spread inside the tick by detie so the MLE sees strict ordering.
    """
    raw = []
    prev_b = prev_a = None
    for s in snapshots:
        if prev_b is not None and s["best_bid"] != prev_b:
            raw.append(s["t"])
        if prev_a is not None and s["best_ask"] != prev_a:
            raw.append(s["t"])
        prev_b, prev_a = s["best_bid"], s["best_ask"]
    return detie_trade_times_us(sorted(raw), dt_us)


def rolling_time_windows(times_us, t_end_us, window_us, stride_us):
    """Fit a Hawkes n in fixed-TIME windows; [(window_end_us, n)] for kept fits."""
    times_us = np.asarray(times_us, dtype=float)
    times_s = times_us / 1e6
    out = []
    for end_us in range(int(window_us), int(t_end_us) + 1, int(stride_us)):
        lo = np.searchsorted(times_us, end_us - window_us, side="left")
        hi = np.searchsorted(times_us, end_us, side="left")
        w = times_s[lo:hi]
        if w.size == 0:
            continue
        fit = calibrate_univariate(w, T=end_us / 1e6)
        n_hat = fit.params.branching_ratio
        if fit.converged and 0.0 < n_hat < BRANCHING_MAX:
            out.append((float(end_us), float(n_hat)))
    return out


def label_windows(windows, marks_us, window_us, lead_us):
    """(pre, normal, n_dropped): drop windows CONTAINING a mark, then label.

    pre    — a mark falls in (end, end + lead_us]
    normal — no mark in (end - window_us, end + lead_us]
    A window whose span (end - window_us, end] contains a mark is dropped:
    it neither precedes the event nor is clean background.
    """
    marks = np.asarray(sorted(marks_us), dtype=float)
    pre, normal = [], []
    dropped = 0
    for end_us, n in windows:
        inside = bool(marks.size) and bool(
            np.any((marks > end_us - window_us) & (marks <= end_us)))
        if inside:
            dropped += 1
            continue
        is_pre = bool(marks.size) and bool(
            np.any((marks > end_us) & (marks <= end_us + lead_us)))
        (pre if is_pre else normal).append(n)
    return pre, normal, dropped


# --- arms ---------------------------------------------------------------------

def _processes(result, dt_us):
    """The two event processes fitted per sim: trade arrivals and quote updates."""
    trades = detie_trade_times_us([tr["t"] for tr in result.trades], dt_us)
    quotes = quote_event_times_us(result.snapshots, dt_us)
    return {"trades": trades, "quotes": quotes}


def _run_arm(n_sims, cfg, marks_from, fund_factory=None):
    """One arm: simulate, window both processes, label against `marks_from`.

    marks_from(result, sim_index) -> event-mark times (gap starts or shock
    times). Returns pooled labelled samples per process + per-sim stats.
    """
    dt_us = cfg["dt_us"]
    window_us, stride_us = WINDOW_TICKS * dt_us, STRIDE_TICKS * dt_us
    lead_us = LEAD_TICKS * dt_us
    pooled = {p: {"pre": [], "normal": []} for p in ("trades", "quotes")}
    per_sim = []
    for i, seed in enumerate(range(1, n_sims + 1)):
        fund = fund_factory(seed, cfg) if fund_factory else None
        result = _simulate_tape(seed, cfg, with_hft=True, fund=fund)
        marks = marks_from(result, i)
        stats = {"seed": seed, "n_marks": len(marks)}
        for name, times in _processes(result, dt_us).items():
            windows = rolling_time_windows(times, cfg["duration_us"],
                                           window_us, stride_us)
            pre, normal, dropped = label_windows(windows, marks, window_us, lead_us)
            pooled[name]["pre"] += pre
            pooled[name]["normal"] += normal
            stats[name] = {"n_events": len(times), "n_windows": len(windows),
                           "n_pre": len(pre), "n_normal": len(normal),
                           "n_dropped": dropped}
        per_sim.append(stats)
    tests = {name: _mann_whitney(g["pre"], g["normal"])
             for name, g in pooled.items()}
    return tests, per_sim


def run_redesigned(n_sims=N_SIMS, duration_ticks=DURATION_TICKS, cfg=None):
    cfg = {**CFG, **(cfg or {}), "duration_us": duration_ticks * CFG["dt_us"]}
    dt_us, sigma = cfg["dt_us"], cfg["sigma"]

    # Treatment: marks are endogenous gap starts (V-flat filtered by detector).
    def gap_marks(result, _i):
        return [g["start_t"] for g in
                detect_liquidity_gaps(result.snapshots, dt_us, sigma)]

    treatment, treat_sims = _run_arm(n_sims, cfg, gap_marks)

    # Placebo: marks are the KNOWN exogenous shock times. Different seeds so
    # the placebo is not the treatment tape re-labelled.
    shock_times = list(range(SHOCK_EVERY_TICKS * dt_us,
                             cfg["duration_us"], SHOCK_EVERY_TICKS * dt_us))

    def shock_fund(seed, c):
        return ShockedFundamental(100 * be.PRICE_PRECISION, c["sigma"],
                                  dt_us=c["dt_us"], seed=10_000 + seed,
                                  shock_times_us=shock_times,
                                  shock_size=SHOCK_SIZE_SIGMAS * c["sigma"])

    placebo, plac_sims = _run_arm(n_sims, cfg, lambda r, i: shock_times,
                                  fund_factory=shock_fund)

    report = {
        "config": {"n_sims_per_arm": n_sims, "duration_ticks": duration_ticks,
                   "window_ticks": WINDOW_TICKS, "stride_ticks": STRIDE_TICKS,
                   "lead_ticks": LEAD_TICKS,
                   "shock_every_ticks": SHOCK_EVERY_TICKS,
                   "shock_size_sigmas": SHOCK_SIZE_SIGMAS,
                   "operating_point": {k: cfg[k] for k in
                                       ("n_hft", "mm_latency_us", "adverse_sensitivity",
                                        "spread_decay", "sigma")}},
        "treatment": treatment, "treatment_per_sim": treat_sims,
        "placebo": placebo, "placebo_per_sim": plac_sims,
    }
    out = _ROOT / "results" / "experiments" / "exp5_redesigned.json"
    out.write_text(json.dumps(report, indent=2, sort_keys=True))
    _print_summary(report)
    return report


def _print_summary(report):
    c = report["config"]
    print(f"Experiment 5 (redesigned) — {c['n_sims_per_arm']} sims/arm x "
          f"{c['duration_ticks']} ticks, window={c['window_ticks']} ticks, "
          f"lead={c['lead_ticks']} ticks")
    for arm in ("treatment", "placebo"):
        print(f"  {arm}:")
        for proc, t in report[arm].items():
            if t["U"] is None:
                print(f"    {proc:>7}: SKIPPED ({t.get('note')})")
                continue
            print(f"    {proc:>7}: pre {t['pre_mean']:.4f} vs normal "
                  f"{t['normal_mean']:.4f}  p={t['p_value']:.4g}  "
                  f"rb={t['rank_biserial']:.3f}  "
                  f"(n={t['n_pre']}/{t['n_normal']})")


if __name__ == "__main__":
    run_redesigned()
