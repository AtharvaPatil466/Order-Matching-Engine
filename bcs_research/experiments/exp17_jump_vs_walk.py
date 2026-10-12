#!/usr/bin/env python3
"""Experiment 17 — jump-diffusion vs random-walk fundamental (PREREGISTRATION P3).

Why this exists. The tick model's fundamental was a Gaussian random walk; BCS's
rent comes from jumps exceeding the half-spread (Prop 5: the prize depends on
the jump law). P3 says that at matched quadratic variation the two laws give
different, predictable sniping economics. Three predictions, as §6 states them:

  H3a (primary)  BCS LP at a fixed half-spread h, equal latency: the ratio of
                 sniper rent rates (Laplace jumps / random walk) at matched
                 variance rate lam_j * 2b^2 = lam_rw * sigma_u^2 equals
                 [lam_j b e^(-h/b)] / [lam_rw 2 (sigma_u phi(h/sigma_u)
                 - h (1 - Phi(h/sigma_u)))] within +-10%; b = 1,000 ticks,
                 lam_j = 10/s, h = 567, and §5's cadence of 100 updates/s
                 (prediction 1.30). Bootstrap ratio interval.
  H3b            Mean profit per successful snipe is b (+-5%) at every LP spread
                 of the sweep under Laplace jumps, and falls with h under the
                 random walk. Miss: the Laplace slope on h has an interval
                 excluding 0, or the random-walk slope is >= 0. OLS slope of the
                 per-h means with a seed-clustered bootstrap.
  H3c            For the paper's maker, the tape-band maker-gains count is
                 strictly lower under the jump-diffusion than under a random
                 walk of matched variance. Miss: count under jumps >= count
                 under the walk.

H3a/H3b are stated in BCS units, so they run in P1's market (paper/
bcs_replication_spec.md, via _confirm_common.bcs_run): one LP and one sniper
(N = 2; the rent ratio does not depend on N, since both laws lose the same
(N-1)/N of races), one-unit Poisson investors, 10 us everywhere, 600-s runs as
P1's spec sizes them, n = 100. The random walk publishes every 10 ms with
sigma_u = sqrt(lam_j 2 b^2 / 100) = 447 ticks; both paths are reflected inside
the engine's price window exactly as P1's (a sign flip keeps |move| and its
timing). "The sweep" of H3b is P1's own h grid, h* x {0.85, ..., 1.15} with
h* = 567.1; H3a reads its middle point, h = 567.

H3c runs at the operating point (it counts tape-band cells): the paper's maker
(_confirm_common.paper_policy) at its 300-ms one-way latency, which P3 does not
restate; the jump-diffusion arm is §5's path and the random walk replaces its
jumps by diffusion of the same total variance rate, sigma^2 + lam_j 2 b^2, on
the same 10-ms grid (the diffusion is drawn first, so the walk's steps are the
jump-diffusion's own draws rescaled: common random numbers). The random-walk
arm's snipers face the 1.7-bp taker fee as their threshold, against steps of
about 3 ticks; the summary reports its sniper fills so a count that is zero by
construction is visible as such.

Run (from bcs_research/):
    python experiments/exp17_jump_vs_walk.py --confirmatory
    python experiments/exp17_jump_vs_walk.py --seeds 2 --out /tmp/exp17.json   # smoke
"""
from __future__ import annotations

import math
import sys
import time
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import _confirm_common as cc  # noqa: E402
import operating_point as op  # noqa: E402

import numpy as np  # noqa: E402
from scipy import stats  # noqa: E402

from bcs_theory import laplace_half_spread  # noqa: E402
from exp9_bcs_replication import GRID_FACTORS  # noqa: E402
from fundamental_paths import JumpDiffusionPath  # noqa: E402
from strategic_maker import StrategicMaker  # noqa: E402

NAME = "exp17_jump_vs_walk"
B, LAM_J, LAM_I = 1000.0, 10.0, 10.0       # H3a's BCS units
RW_RATE = 100.0                            # §5: random-walk updates per second
N_FIRMS = 2
BCS_DURATION_S = 600.0                     # P1's run length
H_STAR = laplace_half_spread(LAM_I, LAM_J, B)
H_GRID = tuple(int(round(H_STAR * f)) for f in GRID_FACTORS)
H3A_H = 567
RATIO_BAND = 0.10
SNIPE_BAND = 0.05
PATHS = ("laplace", "rw")
_P = op.PATH
RW_OP_PATH = {**_P, "jump_intensity_per_s": 0.0,
              "sigma_bp_per_sqrt_s": math.sqrt(_P["sigma_bp_per_sqrt_s"] ** 2
                                               + _P["jump_intensity_per_s"] * 2
                                               * _P["jump_scale_bp"] ** 2)}


# --- theory --------------------------------------------------------------------

def rw_prize(h: float, sigma: float) -> float:
    """E[(|X| - h)^+] for X ~ N(0, sigma^2): 2 (sigma phi(h/sigma) - h (1 - Phi(h/sigma)))."""
    z = h / sigma
    return 2 * (sigma * stats.norm.pdf(z) - h * stats.norm.sf(z))


def h3a_theory(h: float = H3A_H, b: float = B, lam_j: float = LAM_J,
               lam_rw: float = RW_RATE) -> float:
    """H3a's ratio of rent rates, Laplace jumps over the matched random walk."""
    return lam_j * b * math.exp(-h / b) / (lam_rw * rw_prize(h, cc.rw_sigma(lam_j, b, lam_rw)))


def rw_profit_per_snipe(h: float, sigma: float) -> float:
    """E[|X| - h | |X| > h] for the walk: what H3b's random-walk slope should trace."""
    z = h / sigma
    return sigma * stats.norm.pdf(z) / stats.norm.sf(z) - h


# --- runs ----------------------------------------------------------------------

def bcs_task(path_kind: str, h: int, seed: int, duration_s: float = BCS_DURATION_S) -> dict:
    return cc.bcs_run(seed, h, N_FIRMS, path_kind, duration_s=duration_s,
                      rw_updates_per_s=RW_RATE, lam_jump=LAM_J)


def h3c_run(path_kind: str, k: int, ratio: float, seed: int,
            horizon_s: float = op.HORIZON_S) -> dict:
    """The paper's maker on §5's path ("laplace") or on the matched walk ("rw")."""
    mk = op.build_market(seed, maker_latencies_ns=(cc.PAPER_LATENCY_NS,), k=k,
                         snipe_ratio=ratio, policies=[cc.paper_policy()], horizon_s=horizon_s)
    if path_kind == "rw":
        path = JumpDiffusionPath.from_calibration(RW_OP_PATH, int(horizon_s * 1e9), seed=seed)
        makers = [StrategicMaker(m.participant_id, m.policy, path=path,
                                 md_latency_ns=m.md_latency_ns,
                                 order_latency_ns=m.order_latency_ns) for m in mk.makers]
        mk = mk._replace(agents=makers + mk.snipers + [mk.noise], path=path, makers=makers)
    return cc.legs(cc.simulate(mk, seed)[0])


def tasks(seeds, horizon_s: float = op.HORIZON_S, bcs_duration_s: float = BCS_DURATION_S) -> dict:
    t = {}
    for band, ratio, k in sorted(cc.band_cells(), key=lambda c: -c[2]):
        for p in PATHS:
            for s in seeds:
                t[cc.key("h3c", p, band, ratio, k, s)] = (h3c_run, p, k, ratio, s, horizon_s)
    for p in PATHS:
        for s in seeds:
            t[cc.key("h3c", p, "base", s)] = (h3c_run, p, 0, op.PAPER_BAND[0], s, horizon_s)
        for h in H_GRID:
            for s in seeds:
                t[cc.key("bcs", p, h, s)] = (bcs_task, p, h, s, bcs_duration_s)
    return t


# --- verdicts (pure) -------------------------------------------------------------

def slope_ci(hs, edge, fills) -> dict:
    """OLS slope on h of the per-h mean profit per snipe (sum edge / sum fills),
    seeds resampled jointly (clustered). edge, fills: arrays len(hs) x seeds."""
    hs = np.asarray(hs, float)
    edge, fills = np.asarray(edge, float), np.asarray(fills, float)

    def slope(cols):
        return float(np.polyfit(hs, edge[:, cols].sum(1) / fills[:, cols].sum(1), 1)[0])

    n = edge.shape[1]
    rng = np.random.default_rng(op.BOOT_SEED)
    boots = [slope(rng.integers(0, n, n)) for _ in range(op.N_BOOTSTRAP)]
    return {"slope": slope(np.arange(n)), "lower": float(np.percentile(boots, 2.5)),
            "upper": float(np.percentile(boots, 97.5)), "n": n}


def h3a(rate_laplace, rate_rw) -> dict:
    th = h3a_theory()
    return cc.judged(cc.ratio_ci(rate_laplace, rate_rw), th, th * (1 - RATIO_BAND),
                     th * (1 + RATIO_BAND))


def h3b(per_h: dict, slopes: dict) -> dict:
    """per_h: {h: Laplace per-snipe interval}; slopes: {"laplace": ci, "rw": ci}."""
    levels = {str(h): cc.judged(ci, B, B * (1 - SNIPE_BAND), B * (1 + SNIPE_BAND))
              for h, ci in per_h.items()}
    lap, rw = slopes["laplace"], slopes["rw"]
    lap_flat = lap["lower"] <= 0 <= lap["upper"]
    if not lap_flat or rw["lower"] >= 0:
        verdict = "MISS"
    elif rw["upper"] < 0 and all(v["verdict"] == "HIT" for v in levels.values()):
        verdict = "HIT"
    else:
        verdict = "INCONCLUSIVE"
    return {"laplace_per_snipe": levels, "slopes": slopes, "verdict": verdict}


def h3c_verdict(count_jumps: int, count_walk: int) -> str:
    """Counts are fixed by the cell rule, so there is no middle ground."""
    return "HIT" if count_jumps < count_walk else "MISS"


# --- summary ----------------------------------------------------------------------

def summarize(rows: dict, seeds) -> dict:
    def bcs(p, h, f):
        return [f(rows[cc.key("bcs", p, h, s)]) for s in seeds]

    out = {"H3a": {**h3a(bcs("laplace", H3A_H, lambda r: r["sniper_rate"]),
                         bcs("rw", H3A_H, lambda r: r["sniper_rate"])), "primary": True,
                   "h": H3A_H, "rw_updates_per_s": RW_RATE,
                   "sigma_u": cc.rw_sigma(LAM_J, B, RW_RATE)}}
    per_h = {h: cc.ratio_ci(bcs("laplace", h, lambda r: r["sniper_edge"]),
                            bcs("laplace", h, lambda r: r["sniper_fills"])) for h in H_GRID}
    slopes = {p: slope_ci(H_GRID, [bcs(p, h, lambda r: r["sniper_edge"]) for h in H_GRID],
                          [bcs(p, h, lambda r: r["sniper_fills"]) for h in H_GRID])
              for p in PATHS}
    rw_theory = {str(h): rw_profit_per_snipe(h, cc.rw_sigma(LAM_J, B, RW_RATE)) for h in H_GRID}
    rw_measured = {str(h): cc.ratio_ci(bcs("rw", h, lambda r: r["sniper_edge"]),
                                       bcs("rw", h, lambda r: r["sniper_fills"])) for h in H_GRID}
    out["H3b"] = {**h3b(per_h, slopes), "rw_per_snipe": rw_measured,
                  "rw_per_snipe_theory": rw_theory}
    counts, fills = {}, {}
    for p in PATHS:
        base = [rows[cc.key("h3c", p, "base", s)] for s in seeds]
        cells = {(b, r, k): [rows[cc.key("h3c", p, b, r, k, s)] for s in seeds]
                 for b, r, k in cc.band_cells()}
        counts[p] = cc.band_counts(cells, base)
        fills[p] = sum(r["sniper_fills"] for rs in cells.values() for r in rs)
    by_band = {band: {"count_jump_diffusion": counts["laplace"][band]["count"],
                      "count_random_walk": counts["rw"][band]["count"],
                      "verdict": h3c_verdict(counts["laplace"][band]["count"],
                                             counts["rw"][band]["count"])}
               for band in op.BANDS}
    out["H3c"] = {"verdict": by_band["paper"]["verdict"], "verdict_band": "paper",
                  "by_band": by_band, "cells": counts, "sniper_fills_total": fills,
                  "rw_sigma_bp_per_sqrt_s": RW_OP_PATH["sigma_bp_per_sqrt_s"]}
    return out


def main(argv=None) -> dict:
    a = cc.parse_args(NAME, __doc__, argv)
    t0 = time.time()
    rows = cc.run_pending(tasks(a.seed_list), cc.call, a.checkpoint, a.workers, NAME)
    report = {"experiment": NAME, "prediction": "P3", "status": a.status,
              "seeds": a.seed_list, "n_runs": len(rows),
              "config": {"bcs_units": {"b": B, "lam_jump": LAM_J, "lam_invest": LAM_I,
                                       "n_firms": N_FIRMS, "duration_s": BCS_DURATION_S,
                                       "h_grid": H_GRID, "h3a_h": H3A_H,
                                       "rw_updates_per_s": RW_RATE},
                         "h3c": {"horizon_s": op.HORIZON_S, "maker_one_way_ns":
                                 cc.PAPER_LATENCY_NS, "paper_policy": repr(cc.paper_policy()),
                                 "rw_path": RW_OP_PATH, "bands": op.BANDS, "k_grid": op.K_GRID}},
              "units": "H3a/H3b ticks (rates per s); H3c maker P&L USD per run, net of fees",
              **summarize(rows, a.seed_list), "wall_s": round(time.time() - t0, 1)}
    cc.write_report(a.out, report)
    print({h: report[h]["verdict"] for h in ("H3a", "H3b", "H3c")}, flush=True)
    return report


if __name__ == "__main__":
    main()
