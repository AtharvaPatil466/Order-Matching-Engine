#!/usr/bin/env python3
"""Experiment 18 — fee schedules: zero, taker-heavy, maker-rebate (PREREGISTRATION P5).

Why this exists. The paper's incidence results are gross of fees, yet the
venue's fees are the same order as the rents (paper §5). P5 derives, from BCS
eqs. 1-3 with per-unit fees, that an indifference-setting LP depends only on
its net half-spread h' = h - f_m and the total fee F = f_t + f_m:
lam_i h' = lam_j e^(-(h'+F)/b) [b + F (N-1)/N]. Three predictions, as §6 states:

  H5a (primary)  Holding the LP's quotes fixed, a taker fee f_t scales snipes
                 per jump by e^(-f_t/b) and leaves net profit per successful
                 snipe at b, each within +-5%.
  H5b            With the LP's spread re-solved under each schedule by P1's
                 procedure, the all-in investor half-spread h + f_t is equal
                 across schedules with the same F (TOST +-3%) and equals
                 567.1, 648.8, 811.0 ticks for F = 0, 100, 300 at N = 2 and
                 567.1, 658.8, 837.0 at N = 5 (+-3%).
  H5c            The paper's mechanical maker does not re-price: maker-rebate
                 and taker-heavy schedules each give at least as many tape-band
                 maker-gains cells as zero fees, and sniper rent falls by at
                 least the factor e^(-f_t/b). Miss: fewer maker-gains cells
                 under either schedule than under zero fees.

H5a/H5b are in BCS units (lam_i = lam_j = 10/s, b = 1,000 ticks), so they run
in P1's market (_confirm_common.bcs_run: 600-s runs, n = 100) with per-unit
fees in ticks, the units the closed form is derived in. H5a: N = 2, h = 567
(h* at zero fees), f_t in {100, 300} against f_t = 0, f_m = 0. H5b: N in
{2, 5}; at F = 0 the zero schedule; at F in {100, 300} two splits of equal F
shaped like accounting.py's presets: taker-heavy (f_m, f_t) = (2F/7, 5F/7)
(TAKER_HEAVY is 2 / 5 bp) and maker-rebate (-F/2, 3F/2) (MAKER_REBATE is
-1 / 3 bp). P1's procedure: a seven-point h grid around the predicted quote
h' + f_m (factors 0.85 ... 1.15), per seed the root of D(h) = LP net rate -
mean sniper net rate (exp9.grid_root), all-in = root + f_t.

H5c counts tape-band cells at the operating point with accounting.py's three
presets (ZERO, TAKER_HEAVY, MAKER_REBATE); the snipers' threshold is the
schedule's taker fee, and the no-HFT baseline, which no fee can change, runs
once and is re-priced under each schedule. The maker is the paper's at its
300-ms one-way latency (P5 does not restate it). Rent is the snipers' P&L net
of fees; f_t / b in the factor is the taker fee over the path's Laplace scale,
both in bp. "Falls by at least the factor" is read per paper-band cell as the
95% upper bound of rent(schedule) / rent(zero) at or below e^(-f_t/b).

Run (from bcs_research/):
    python experiments/exp18_fee_schedules.py --confirmatory
    python experiments/exp18_fee_schedules.py --seeds 2 --out /tmp/exp18.json   # smoke
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

from scipy import optimize  # noqa: E402

from accounting import MAKER_REBATE, TAKER_HEAVY, ZERO  # noqa: E402
from exp9_bcs_replication import GRID_FACTORS, root_estimate  # noqa: E402

NAME = "exp18_fee_schedules"
B, LAM = 1000.0, 10.0                      # BCS units: b, lam_i = lam_j
BCS_DURATION_S = 600.0
H5A_H, H5A_N, H5A_FT = 567, 2, (100.0, 300.0)
H5B_F, H5B_N = (0.0, 100.0, 300.0), (2, 5)
H5B_TABLE = {(0.0, 2): 567.1, (100.0, 2): 648.8, (300.0, 2): 811.0,
             (0.0, 5): 567.1, (100.0, 5): 658.8, (300.0, 5): 837.0}
SPLITS = {"taker_heavy": (2 / 7, 5 / 7), "maker_rebate": (-1 / 2, 3 / 2)}   # (f_m, f_t) / F
H5A_BAND, H5B_BAND = 0.05, 0.03
SCHEDULES = {"zero": ZERO, "taker_heavy": TAKER_HEAVY, "maker_rebate": MAKER_REBATE}


# --- theory --------------------------------------------------------------------

def net_half_spread(F: float, n_firms: int, b: float = B, lam_i: float = LAM,
                    lam_j: float = LAM) -> float:
    """h' solving lam_i h' = lam_j e^(-(h'+F)/b) [b + F (N-1)/N] (P5, derived)."""
    def gap(h):
        return lam_i * h - lam_j * math.exp(-(h + F) / b) * (b + F * (n_firms - 1) / n_firms)
    return optimize.brentq(gap, 0.0, lam_j * (b + F) / lam_i)


def schedules_at(F: float) -> dict:
    """{name: (f_m, f_t)} of H5b's schedules with total fee F."""
    if F == 0:
        return {"zero": (0.0, 0.0)}
    return {name: (m * F, t * F) for name, (m, t) in SPLITS.items()}


# --- runs ----------------------------------------------------------------------

def bcs_task(h: int, n_firms: int, f_t: float, f_m: float, seed: int,
             duration_s: float = BCS_DURATION_S) -> dict:
    return cc.bcs_run(seed, h, n_firms, "laplace", f_t=f_t, f_m=f_m, duration_s=duration_s,
                      lam_jump=LAM)


def h5b_grid(F: float, n_firms: int, f_m: float) -> list:
    return [int(round((net_half_spread(F, n_firms) + f_m) * g)) for g in GRID_FACTORS]


def h5c_run(schedule: str, k: int, ratio: float, seed: int,
            horizon_s: float = op.HORIZON_S) -> dict:
    return cc.maker_run(seed, cc.paper_policy(), cc.PAPER_LATENCY_NS, k, ratio,
                        fees=SCHEDULES[schedule], horizon_s=horizon_s)


def tasks(seeds, horizon_s: float = op.HORIZON_S, bcs_duration_s: float = BCS_DURATION_S) -> dict:
    t = {}
    for band, ratio, k in sorted(cc.band_cells(), key=lambda c: -c[2]):
        for sched in SCHEDULES:
            for s in seeds:
                t[cc.key("h5c", sched, band, ratio, k, s)] = (h5c_run, sched, k, ratio, s, horizon_s)
    for s in seeds:
        t[cc.key("h5c", "base", s)] = (h5c_run, "zero", 0, op.PAPER_BAND[0], s, horizon_s)
    for F in H5B_F:
        for n in H5B_N:
            for f_m, f_t in schedules_at(F).values():
                for h in h5b_grid(F, n, f_m):
                    for s in seeds:
                        t[cc.key("bcs", h, n, f_t, f_m, s)] = (bcs_task, h, n, f_t, f_m, s,
                                                               bcs_duration_s)
    for f_t in (0.0, *H5A_FT):
        for s in seeds:
            t[cc.key("bcs", H5A_H, H5A_N, f_t, 0.0, s)] = (bcs_task, H5A_H, H5A_N, f_t, 0.0, s,
                                                          bcs_duration_s)
    return t


# --- verdicts (pure) ---------------------------------------------------------------

def h5a(snipes: dict, net: dict, fills: dict) -> dict:
    """snipes / net / fills: {f_t: per-seed snipes per jump / sniper net / sniper fills}."""
    out = {}
    for f_t in H5A_FT:
        th = math.exp(-f_t / B)
        out[f"snipes_ratio_f{f_t:g}"] = cc.judged(cc.ratio_ci(snipes[f_t], snipes[0.0]), th,
                                                  th * (1 - H5A_BAND), th * (1 + H5A_BAND))
        out[f"net_per_snipe_f{f_t:g}"] = cc.judged(cc.ratio_ci(net[f_t], fills[f_t]), B,
                                                   B * (1 - H5A_BAND), B * (1 + H5A_BAND))
    return {"tests": out, "verdict": cc.combine(v["verdict"] for v in out.values())}


def h5b(all_in: dict) -> dict:
    """all_in: {(F, N, schedule): per-seed all-in half-spreads}, seeds aligned."""
    levels, equal = {}, {}
    for (F, n, name), vals in sorted(all_in.items()):
        th = net_half_spread(F, n) + F
        levels[f"F={F:g},N={n},{name}"] = {**cc.judged(cc.boot(vals), th, th * (1 - H5B_BAND),
                                                       th * (1 + H5B_BAND)),
                                           "tabulated": H5B_TABLE[(F, n)]}
    for F in H5B_F:
        if F == 0:
            continue
        for n in H5B_N:
            diff = [a - b for a, b in zip(all_in[(F, n, "taker_heavy")],
                                          all_in[(F, n, "maker_rebate")])]
            m = H5B_BAND * (net_half_spread(F, n) + F)
            c95, c90 = cc.boot(diff), cc.boot(diff, ci=0.90)
            equal[f"F={F:g},N={n}"] = {**c95, "ci90": [c90["lower"], c90["upper"]],
                                       "margin": [-m, m], "verdict": cc.tost(c90, c95, -m, m)}
    verdicts = [v["verdict"] for v in levels.values()] + [v["verdict"] for v in equal.values()]
    return {"levels": levels, "split_equivalence": equal, "verdict": cc.combine(verdicts)}


def h5c_verdict(counts: dict, rent_ok: bool) -> str:
    """counts: {schedule: maker-gains cells}; rent_ok: rent fell by the factor everywhere."""
    if min(counts["taker_heavy"], counts["maker_rebate"]) < counts["zero"]:
        return "MISS"
    return "HIT" if rent_ok else "INCONCLUSIVE"


def rent_factor(schedule: str) -> float:
    return math.exp(-SCHEDULES[schedule].taker_bps / op.PATH["jump_scale_bp"])


# --- summary --------------------------------------------------------------------------

def summarize(rows: dict, seeds) -> dict:
    def bcs(h, n, f_t, f_m, f):
        return [f(rows[cc.key("bcs", h, n, f_t, f_m, s)]) for s in seeds]

    col = {name: {f_t: bcs(H5A_H, H5A_N, f_t, 0.0, lambda r, name=name: r[name])
                  for f_t in (0.0, *H5A_FT)}
           for name in ("snipes_per_jump", "sniper_net", "sniper_fills")}
    out = {"H5a": {**h5a(col["snipes_per_jump"], col["sniper_net"], col["sniper_fills"]),
                   "primary": True}}
    all_in, roots = {}, {}
    for F in H5B_F:
        for n in H5B_N:
            for name, (f_m, f_t) in schedules_at(F).items():
                hs = h5b_grid(F, n, f_m)
                curves = [[rows[cc.key("bcs", h, n, f_t, f_m, s)] for h in hs] for s in seeds]
                d = [[r["lp_net_rate"] - r["sniper_net_rate"] / (n - 1) for r in c] for c in curves]
                est = root_estimate(hs, d)
                roots[f"F={F:g},N={n},{name}"] = {k: est[k] for k in ("mean", "lower", "upper",
                                                                      "n_extrapolated",
                                                                      "n_multi_crossing")}
                all_in[(F, n, name)] = [x + f_t for x in est["per_seed"]]
    out["H5b"] = {**h5b(all_in), "quoted_half_spread_roots": roots}
    base = [rows[cc.key("h5c", "base", s)] for s in seeds]
    counts, rent = {}, {}
    for sched, fees in SCHEDULES.items():
        cells = {(b, r, k): [rows[cc.key("h5c", sched, b, r, k, s)] for s in seeds]
                 for b, r, k in cc.band_cells()}
        counts[sched] = cc.band_counts(cells, base, fees)
        rent[sched] = {c: [cc.net(x, "sniper", fees) for x in rs] for c, rs in cells.items()}
    by_band = {}
    for band in op.BANDS:
        checks = {}
        for sched in ("taker_heavy", "maker_rebate"):
            for (b, r, k), vals in rent[sched].items():
                if b != band:
                    continue
                ci = cc.ratio_ci(vals, rent["zero"][(b, r, k)])
                checks[f"{sched},ratio={r},k={k}"] = {**ci, "factor": rent_factor(sched),
                                                      "ok": ci["upper"] <= rent_factor(sched)}
        n = {s: counts[s][band]["count"] for s in SCHEDULES}
        by_band[band] = {"counts": n, "rent_checks": checks,
                         "rent_ok": all(c["ok"] for c in checks.values()),
                         "verdict": h5c_verdict(n, all(c["ok"] for c in checks.values()))}
    out["H5c"] = {"verdict": by_band["paper"]["verdict"], "verdict_band": "paper",
                  "by_band": by_band, "cells": counts}
    return out


def main(argv=None) -> dict:
    a = cc.parse_args(NAME, __doc__, argv)
    t0 = time.time()
    rows = cc.run_pending(tasks(a.seed_list), cc.call, a.checkpoint, a.workers, NAME)
    report = {"experiment": NAME, "prediction": "P5", "status": a.status,
              "seeds": a.seed_list, "n_runs": len(rows),
              "config": {"bcs_units": {"b": B, "lam_invest": LAM, "lam_jump": LAM,
                                       "duration_s": BCS_DURATION_S, "h5a": {
                                           "h": H5A_H, "n_firms": H5A_N, "f_t": H5A_FT},
                                       "h5b": {"F": H5B_F, "n_firms": H5B_N,
                                               "splits_over_F": SPLITS,
                                               "grid_factors": GRID_FACTORS}},
                         "h5c": {"schedules_bps": {k: vars(v) for k, v in SCHEDULES.items()},
                                 "maker_one_way_ns": cc.PAPER_LATENCY_NS,
                                 "paper_policy": repr(cc.paper_policy()),
                                 "horizon_s": op.HORIZON_S, "bands": op.BANDS,
                                 "k_grid": op.K_GRID}},
              "units": "H5a/H5b ticks; H5c USD per run, net of each schedule's fees",
              **summarize(rows, a.seed_list), "wall_s": round(time.time() - t0, 1)}
    cc.write_report(a.out, report)
    print({h: report[h]["verdict"] for h in ("H5a", "H5b", "H5c")}, flush=True)
    return report


if __name__ == "__main__":
    main()
