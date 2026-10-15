#!/usr/bin/env python3
"""The confirmatory analysis pass: every prediction's verdict, the 11 primaries Holm-adjusted.

Why this exists. PREREGISTRATION §2: "The 11 primaries are one family,
Holm-adjusted at alpha = 0.05. Secondaries use unadjusted 95% intervals." The
drivers judge every prediction at a fixed 95% and leave Holm to this pass
(§4.1, P11). This module reads each driver's summary JSON and its checkpoint,
recomputes each primary at its Holm-adjusted level from the checkpointed rows
(calling the drivers' own pure functions, with the level passed where they
take one), and writes `paper/confirmatory_results.md` plus a JSON twin. It
never edits or re-runs a driver: the batch must use the committed code.

How Holm is applied. A primary's verdict is a rule on intervals ("HIT if the
95% interval lies inside the band"). Replace 95% by 1 - a and the HIT
conditions define a test at level a; its Holm-compatible p-value is

    p_k = inf { a : the HIT conditions hold with (1 - a) intervals }.

Every HIT condition below is an interval lying inside a band or beyond a
bound, and the percentile-bootstrap (same 2,000 resamples, seed 0) and Wilson
intervals are nested in a, so the set of a where a primary HITs is an up-set
and p_k is well defined; for a conjunction of cells it is the largest cell p
(an intersection-union test, valid without a further adjustment). Holm
compares the j-th smallest p to alpha / (m - j + 1), m = 11, and only those 11
thresholds can change a decision, so p_k is computed as its ceiling on that
grid (`grid_p_value`): exact for Holm's decisions, and it needs the rule
evaluated at no more than 11 levels. A primary that is missing, refused
(non-confirmatory status) or a MISS at 95% enters with p = 1, so a partial
batch is never more lenient than the full one (Holm is monotone in the others'
p-values). The family size stays 11 whatever has finished.

MISS is NOT adjusted (ambiguity A1 below): the Holm verdict is MISS if the
driver's own 95% rule says MISS, HIT if Holm rejects, INCONCLUSIVE otherwise.
The intervals the report prints for a primary are at its Holm threshold.

Each primary, as its driver computes it, and its HIT conditions at level 1 - a:

  P1  exp9 `p1_verdict`, "primary_race_loss". Per cell (jump ratio x N, the
      final cells: those x9._reruns flags use 2n seeds), the race-loss share
      pooled over seeds at h*, Wilson (scipy binomtest, method "wilson"),
      band (N - 1)/N +- 0.02. HIT: every cell's (1 - a) Wilson interval inside
      its band. The h-hat slope enters P1 only as a MISS clause ("h-hat
      trending in N"), so it is read from the driver at 95%.
  P2  exp16 `h2a`. 16 cells k x lag/eps: maker cancel-first share, pooled
      Wilson (op.wilson with z = Phi^-1(1 - a/2)), band
      race_win_probability +- 0.02. HIT: all 16 inside.
  P3  exp17 `h3a`. One paired ratio of sniper rent rates, Laplace / random
      walk at h = 567: cc.ratio_ci(ci = 1 - a), band 1.30 +- 10%.
  P4  exp10 `p4_verdicts` H4a at c = R/5.5. N* comes from P4's Setup, the
      driver's entry rule ("enter while the next entrant's interval lies above
      c"), at the driver's 95%: it defines the market's equilibrium, not the
      test, and an estimand must not move with the family size (A3). HIT:
      N* = 5 and the waste share n c / rent, from the (1 - a) rent interval,
      inside 0.909 +- 0.05. MISS clauses: N* != 5 with the 5th/6th entrants'
      intervals excluding 5, or waste disjoint from the band.
  P5  exp18 `h5a`. Four paired ratios (snipes per jump vs e^(-f_t/b), net
      profit per snipe vs b, f_t in {100, 300}) via cc.ratio_ci(ci = 1 - a),
      bands +- 5%. HIT: all four inside.
  P6  exp19 H6a. Per v-bar arm on the paper band: participation-over-
      predicted ratio (the driver's joint seed bootstrap, percentiles at a/2)
      inside [0.9, 1.1], and the deadweight interval (cc.boot, ci = 1 - a)
      above zero; x19.h6a_verdict judges. HIT: both arms.
  P7  exp20 `h7a`. 16 cells: per-jump loss ratio cancelling / non-cancelling
      (the driver's joint seed bootstrap at a/2), band 1 - race_win_probability
      +- 0.03. HIT: all 16 inside.
  P8  exp12 `h8a`. The Nash profiles are the solver's (its best-response tests
      define the equilibrium, A3). On the report seeds: each M-1 -> M paired
      spread difference with (1 - a) upper bound <= 0, M = 5's spread interval
      inside +-20% of the N = k + 1 breakeven, and M = 1 on the grid's upper
      edge (deterministic). HIT: all. §4.1 notes the M = 5 clause cannot HIT
      on the tick grid, so P8 cannot HIT at any level.
  P9  exp11 H9a. Per tau in {1, 10, 100} ms: batch-minus-continuous sniping
      loss with (1 - a) upper bound < 0 and the batch loss's upper bound
      <= 1.2 x footnote 34's bound. HIT: all three taus.
  P10 exp15 `classify`, paper band. The configuration is the unit; its per-
      maker mean deltas are the driver's (no level enters them). HIT: the
      Wilson (z at a) lower bounds of the hit rate and of the share with
      Spearman < 0 both >= 0.80. MISS clauses: hit-rate upper < 0.60 or the
      closely spaced upper < 0.50.
  P11 exp13 H11a at the N = 512 of record. Joint design-row bootstrap of
      S1(sniper latency) and S1(lambda_j) + S1(b) (the driver's _joint_ci,
      percentiles at a/2). HIT: latency upper < 0.05 and the sum's lower
      >= 0.5. The N = 2,048 extension is reported beside, not of record.

Every recomputation reproduces the driver's own 95% verdict at a = 0.05
(tests/test_confirmatory_report.py); a mismatch is reported as a failure.

Ambiguities for the lead to log in §4 (none is silently resolved):
  A1  Holm adjusts HIT claims only; MISS stays at the drivers' 95%. Adjusting
      MISS too would widen the miss intervals and turn misses into
      INCONCLUSIVE (flattering the program, against §1's intent), and P6's
      miss clause "deadweight interval including zero" is a failure-to-reject
      clause that a wider interval makes EASIER, so a symmetric adjustment
      would turn a 95% HIT into a Holm MISS.
  A2  Bootstrap resolution: §2 fixes 2,000 resamples, kept here, so at
      a = 0.05/11 each percentile bound rests on about 4.5 resamples per tail.
  A3  Procedure intervals stay at 95%: P4's entry rule (Setup) and P8's
      best-response confirmations define the estimand, so they are not
      Holm-adjusted; only the intervals that test a prediction are.
  A4  Conjunctions over cells (P1, P2, P5, P6, P7, P8, P9, P11) are judged as
      the drivers judge them, with no within-primary adjustment: HIT needs
      every cell (intersection-union, valid at a), MISS fires on any cell.
  A5  P10's miss line names the tick-model clauses too; they belong to the
      tick-model secondary bullet, which the driver reports beside, so the
      primary is the event-model rule. P10b is a secondary (§4 calls it one),
      outside the family.
  A6  P1's batch arm, h-hat, h0-hat and profit-per-snipe bullets are
      secondaries; P1's primary is the race-loss share (exp9's docstring).

Run (from bcs_research/), after the batch:
    .venv/bin/python analysis/confirmatory_report.py
    .venv/bin/python analysis/confirmatory_report.py --results-dir DIR --out /tmp/report.md
It refuses to write anything when no driver summary carries the confirmatory
status, and labels every non-confirmatory driver loudly when some do.
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, NamedTuple

import numpy as np
from scipy.stats import binomtest, norm

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT / "experiments") not in sys.path:
    sys.path.insert(0, str(_ROOT / "experiments"))

import operating_point as op  # noqa: E402  (puts build/, agents/, metrics/, ... on sys.path)
import _confirm_common as cc  # noqa: E402
import exp9_bcs_replication as x9  # noqa: E402
import exp10_speed_entry as x10  # noqa: E402
import exp11_batch_semantics as x11  # noqa: E402
import exp12_endogenous_makers as x12  # noqa: E402
import exp13_global_sensitivity as x13  # noqa: E402
import exp15_heldout_who_pays as x15  # noqa: E402
import exp16_race_incidence as x16  # noqa: E402
import exp17_jump_vs_walk as x17  # noqa: E402
import exp18_fee_schedules as x18  # noqa: E402
import exp19_noise_heterogeneity as x19  # noqa: E402
import exp20_maker_strategies as x20  # noqa: E402
from bcs_theory import half_spread, laplace_jump, race_loss_probability  # noqa: E402
from bootstrap import bootstrap_ci  # noqa: E402

ALPHA = 0.05
FAMILY = 11
OUT_MD = _ROOT / "paper" / "confirmatory_results.md"
# run_confirmatory.sh's drivers, in its order; the last two are extras, not of record.
DRIVERS = ("exp9_bcs_replication", "exp10_speed_entry", "exp11_batch_semantics",
           "exp17_jump_vs_walk", "exp19_noise_heterogeneity", "exp18_fee_schedules",
           "exp20_maker_strategies", "exp16_race_incidence", "exp12_endogenous_makers",
           "exp13_global_sensitivity", "exp15_heldout_who_pays")
EXTRAS = ("exp13_global_sensitivity_n2048", "exp14_incidence_priors")
H4A_N_STAR, H4A_WASTE_BAND = 5, (0.909 - 0.05, 0.909 + 0.05)   # as exp10.p4_verdicts


# --- Holm ---------------------------------------------------------------------------

def holm(p_values: dict, alpha: float = ALPHA, m: int | None = None) -> dict:
    """Holm's step-down: the j-th smallest p against alpha / (m - j + 1), stopping at the
    first failure. m defaults to len(p_values); a larger m counts absent tests as p = 1."""
    m = len(p_values) if m is None else m
    if m < len(p_values):
        raise ValueError(f"family size {m} below the {len(p_values)} p-values given")
    out, still = {}, True
    for j, name in enumerate(sorted(p_values, key=lambda k: (p_values[k], k))):
        threshold = alpha / (m - j)
        still = still and p_values[name] <= threshold
        out[name] = {"rank": j + 1, "threshold": threshold, "rejected": still}
    return out


def holm_thresholds(m: int = FAMILY, alpha: float = ALPHA) -> list:
    """Holm's thresholds, ascending: alpha/m, alpha/(m-1), ..., alpha."""
    return [alpha / (m - j) for j in range(m)]


def grid_p_value(hit_at: Callable[[float], bool], thresholds) -> float:
    """The smallest threshold at which the HIT conditions hold (the p-value's ceiling on
    Holm's grid), 1.0 if none. hit_at must be monotone: true at a means true above a."""
    return next((a for a in sorted(thresholds) if hit_at(a)), 1.0)


# --- intervals at a level ---------------------------------------------------------------
# The drivers' bootstraps at 1 - a. Percentiles are 100 a/2 and 100 - 100 a/2, which at
# a = 0.05 are exactly the drivers' 2.5 and 97.5; functions that take ci= get 1 - a.

def _pct(boots, alpha: float) -> tuple:
    q = 100 * alpha / 2
    return float(np.percentile(boots, q)), float(np.percentile(boots, 100 - q))


def _seed_boot(stat, n: int, alpha: float, finite_only: bool = False) -> dict:
    """The drivers' joint seed bootstrap loop (2,000 resamples, seed 0) at level 1 - a."""
    rng = np.random.default_rng(op.BOOT_SEED)
    boots = np.array([stat(rng.integers(0, n, n)) for _ in range(op.N_BOOTSTRAP)])
    if finite_only:
        boots = boots[np.isfinite(boots)]
    lo, hi = _pct(boots, alpha) if boots.size else (math.nan, math.nan)
    return {"mean": float(stat(np.arange(n))), "lower": lo, "upper": hi, "n": n}


def _z(alpha: float) -> float:
    return float(norm.ppf(1 - alpha / 2))


def _part(label: str, ci: dict, verdict: str, band=None, rule: str | None = None) -> dict:
    est = ci.get("mean", ci.get("rate"))
    return {"label": label, "estimate": est, "lower": ci.get("lower"), "upper": ci.get("upper"),
            "band": band, "rule": rule, "verdict": verdict}


def _judged(label: str, j: dict) -> dict:
    return _part(label, j, j["verdict"], j["band"])


def _all(parts: list) -> dict:
    return {"verdict": cc.combine(p["verdict"] for p in parts), "parts": parts}


# --- the eleven primaries, each at level 1 - alpha -------------------------------------
# evaluate(summary, rows, alpha) -> {"verdict": the full rule at 1 - alpha, "parts": [...]}.
# rows: the driver's checkpoint, {key: row}.

def p1_race_loss(summary: dict, rows: dict, alpha: float) -> dict:
    rerun = {tuple(c) for c in x9._reruns(summary["first_pass_40_seeds"])}
    n = len(summary["seeds"])
    duration = summary["config"]["duration_s"]
    parts = []
    for c in summary["final"]["cells"]:
        seeds = range(2 * n if (c["ratio"], c["n_firms"]) in rerun else n)
        at = [rows[json.dumps([c["lam_jump"], c["n_firms"], c["grid_star"][3], s, duration, 0])]
              for s in seeds]
        k, m = sum(r["n_sniped"] for r in at), sum(r["n_snipeable"] for r in at)
        w = binomtest(k, m).proportion_ci(confidence_level=1 - alpha, method="wilson")
        th = race_loss_probability(c["n_firms"])
        band = [th - x9.SHARE_BAND, th + x9.SHARE_BAND]
        parts.append(_part(f"ratio={c['ratio']:g},N={c['n_firms']}",
                           {"mean": k / m, "lower": float(w.low), "upper": float(w.high)},
                           op.band_verdict(float(w.low), float(w.high), *band), band))
    slopes = [_part(f"h-hat slope, ratio={r} (95%, MISS clause only)", s,
                    "MISS" if s["verdict"] == "MISS" else "not a miss", s["band"])
              for r, s in summary["final"]["h_hat_slope_on_race_loss"].items()]
    race = cc.combine(p["verdict"] for p in parts)
    verdict = "MISS" if any(s["verdict"] == "MISS" for s in slopes) else race
    return {"verdict": verdict, "parts": parts + slopes}


def p2_h2a(summary: dict, rows: dict, alpha: float) -> dict:
    seeds, z, parts = summary["seeds"], _z(alpha), []
    for k in x16.K_RACE:
        for lf in x16.LAG_FRACS:
            rs = [rows[cc.key("race", k, lf, s)] for s in seeds]
            th = x16.race_win_probability(lf * op.JITTER_NS, op.JITTER_NS, k)
            w = op.wilson(sum(r["maker_wins"] for r in rs), sum(r["races"] for r in rs), z=z)
            parts.append(_judged(f"k={k},lag/eps={lf}",
                                 cc.judged(w, th, th - x16.SHARE_BAND, th + x16.SHARE_BAND)))
    return _all(parts)


def p3_h3a(summary: dict, rows: dict, alpha: float) -> dict:
    rate = {p: [rows[cc.key("bcs", p, x17.H3A_H, s)]["sniper_rate"] for s in summary["seeds"]]
            for p in x17.PATHS}
    th = x17.h3a_theory()
    j = cc.judged(cc.ratio_ci(rate["laplace"], rate["rw"], ci=1 - alpha), th,
                  th * (1 - x17.RATIO_BAND), th * (1 + x17.RATIO_BAND))
    return _all([_judged("rent rate ratio, Laplace / random walk", j)])


def p4_h4a(summary: dict, rows: dict, alpha: float) -> dict:
    seeds, c = summary["seeds"], x10.P4_COSTS[5.5]

    def col(n_firms, field):
        return [rows[json.dumps([n_firms - 1, False, s])][field] for s in seeds]

    per = {n: col(n, "per_sniper") for n in x10.FIRMS_GRID}
    n_star = x10.entry_stop(per, c, 2)["n_star"]         # Setup's rule, the driver's 95% (A3)
    ci5, ci6 = (bootstrap_ci(per[j], ci=1 - alpha, seed=0) for j in (5, 6))
    share = None
    if n_star >= 2:
        r = bootstrap_ci(col(n_star, "rent"), ci=1 - alpha, seed=0)
        share = {"mean": n_star * c / r["mean"], "lower": n_star * c / r["upper"],
                 "upper": n_star * c / r["lower"]}
    waste = op.band_verdict(share["lower"], share["upper"], *H4A_WASTE_BAND) if share else "MISS"
    excluded = ci5["upper"] < c or ci6["lower"] > c
    if (n_star != H4A_N_STAR and excluded) or waste == "MISS":
        verdict = "MISS"
    else:
        verdict = "HIT" if n_star == H4A_N_STAR and waste == "HIT" else "INCONCLUSIVE"
    parts = [_part("entry stops at N* (Setup rule, 95%)", {"mean": n_star},
                   "HIT" if n_star == H4A_N_STAR else "not 5", rule="N* = 5"),
             _part("5th entrant profit vs c", ci5, "excludes 5" if ci5["upper"] < c else "-",
                   rule=f"c = {c:.4g}"),
             _part("6th entrant profit vs c", ci6, "excludes 5" if ci6["lower"] > c else "-",
                   rule=f"c = {c:.4g}"),
             _part("waste share N* c / rent", share or {}, waste, list(H4A_WASTE_BAND))]
    return {"verdict": verdict, "parts": parts}


def p5_h5a(summary: dict, rows: dict, alpha: float) -> dict:
    def col(f_t, field):
        return [rows[cc.key("bcs", x18.H5A_H, x18.H5A_N, f_t, 0.0, s)][field]
                for s in summary["seeds"]]

    parts, band, b = [], x18.H5A_BAND, x18.B
    for f_t in x18.H5A_FT:
        th = math.exp(-f_t / b)
        snipes = cc.ratio_ci(col(f_t, "snipes_per_jump"), col(0.0, "snipes_per_jump"), ci=1 - alpha)
        net = cc.ratio_ci(col(f_t, "sniper_net"), col(f_t, "sniper_fills"), ci=1 - alpha)
        parts.append(_judged(f"snipes per jump ratio, f_t={f_t:g}",
                             cc.judged(snipes, th, th * (1 - band), th * (1 + band))))
        parts.append(_judged(f"net profit per snipe, f_t={f_t:g}",
                             cc.judged(net, b, b * (1 - band), b * (1 + band))))
    return _all(parts)


def _participation_ci(part, h, part0, h0, vbar: float, alpha: float) -> dict:
    """x19.participation_ci at level 1 - a (same statistic, same resamples)."""
    part, h = np.asarray(part, float), np.asarray(h, float)
    part0, h0 = np.asarray(part0, float), np.asarray(h0, float)

    def stat(cols):
        measured = part[:, cols].mean() / part0[cols].mean()
        predicted = np.mean(np.exp(-(h[:, cols].mean(1) - h0[cols].mean()) / vbar))
        return measured / predicted

    return _seed_boot(stat, part.shape[1], alpha)


def p6_h6a(summary: dict, rows: dict, alpha: float) -> dict:
    seeds, parts, verdicts = summary["seeds"], [], []
    for e in x19.DESIGN_E:
        vbar = x19.vbar_for(e)
        base = [rows[cc.key("p6", e, "base", s)] for s in seeds]
        cells = [[rows[cc.key("p6", e, "paper", r, k, s)] for s in seeds]
                 for r in op.PAPER_BAND for k in op.K_GRID]
        ratio = _participation_ci([[r["crossings"] / r["arrivals"] for r in c] for c in cells],
                                  [[r["half_spread_ticks"] for r in c] for c in cells],
                                  [r["crossings"] / r["arrivals"] for r in base],
                                  [r["half_spread_ticks"] for r in base], vbar, alpha)
        dwl = cc.boot([np.mean([(b["gains_usd"] - c[i]["gains_usd"]) / b["duration_s"]
                                for c in cells]) for i, b in enumerate(base)], ci=1 - alpha)
        verdicts.append(x19.h6a_verdict(ratio, dwl))
        band = [1 - x19.PART_BAND, 1 + x19.PART_BAND]
        parts.append(_part(f"e={e}: participation / predicted", ratio,
                           op.band_verdict(ratio["lower"], ratio["upper"], *band), band))
        parts.append(_part(f"e={e}: deadweight USD/s", dwl,
                           "HIT" if dwl["lower"] > 0 else "MISS", rule="interval above 0"))
    return {"verdict": cc.combine(verdicts), "parts": parts}


def _per_jump_ratio_ci(loss_a, n_a, loss_b, n_b, alpha: float) -> dict:
    """x20.per_jump_ratio_ci at level 1 - a."""
    arr = [np.asarray(x, float) for x in (loss_a, n_a, loss_b, n_b)]

    def stat(i):
        la, na, lb, nb = (x[i].sum() for x in arr)
        return (la / na) / (lb / nb) if na and nb and lb else math.nan

    return _seed_boot(stat, len(arr[0]), alpha, finite_only=True)


def p7_h7a(summary: dict, rows: dict, alpha: float) -> dict:
    parts = []
    for k in x20.K_RACE:
        for lf in x20.LAG_FRACS:
            a, b = ([rows[cc.key("h7a", kind, k, lf, s)] for s in summary["seeds"]]
                    for kind in ("cancelling", "non_cancelling"))
            ci = _per_jump_ratio_ci([r["loss_usd"] for r in a], [r["snipeable"] for r in a],
                                    [r["loss_usd"] for r in b], [r["snipeable"] for r in b], alpha)
            th = 1 - x20.race_win_probability(lf * op.JITTER_NS, op.JITTER_NS, k)
            parts.append(_judged(f"k={k},lag/eps={lf}",
                                 cc.judged(ci, th, th - x20.H7A_BAND, th + x20.H7A_BAND)))
    return _all(parts)


def _h8a_spreads(summary: dict, rows: dict) -> dict:
    """{M: per-report-seed realised half-spread} of the symmetric F = 0 equilibria."""
    mk, seeds = summary["market"], summary["seeds"]["report"]
    ms = sorted(int(c[len("sym_M"):-len("_F0")]) for c in summary["cells"]
                if c.startswith("sym_M") and c.endswith("_F0"))
    out = {}
    for m in ms:
        profile = [x12.describe(x12.policy_from(d)) for d in summary["cells"][f"sym_M{m}_F0"]["profile"]]
        lat = list(x12._lat((mk["sym_us"],) * m))
        out[m] = [rows[json.dumps([[mk["k"], mk["snipe_ratio"]], profile, lat, s,
                                   summary["horizon_s"]])]["half_spread_ticks"] for s in seeds]
    return out


def p8_h8a(summary: dict, rows: dict, alpha: float) -> dict:
    spreads, ci = _h8a_spreads(summary, rows), 1 - alpha
    ms, parts = sorted(spreads), []
    for a, b in zip(ms, ms[1:]):
        pairs = [y - x for x, y in zip(spreads[a], spreads[b]) if x is not None and y is not None]
        d = bootstrap_ci(pairs, ci=ci) if pairs else None
        parts.append(_part(f"spread M={b} minus M={a}", d or {},
                           x12._step(d) if d else "INCONCLUSIVE", rule="upper <= 0"))
    k = summary["market"]["k"]
    n = k + 1
    breakeven = half_spread(op.INVESTOR_RATE_PER_S, op.PATH["jump_intensity_per_s"] * (n - 1) / n,
                            laplace_jump(op.PATH["jump_scale_bp"])) / op.TICK_BP
    band = [0.8 * breakeven, 1.2 * breakeven]
    vals = [v for v in spreads[max(ms)] if v is not None]
    top = bootstrap_ci(vals, ci=ci) if vals else None
    parts.append(_part(f"M={max(ms)} spread vs breakeven", top or {},
                       op.band_verdict(top["lower"], top["upper"], *band) if top else "MISS", band))
    m1 = x12.policy_from(summary["cells"][f"sym_M{min(ms)}_F0"]["profile"][0])
    edge = m1.quote and m1.half_spread == max(x12.HALF_SPREADS_TICKS)
    verdicts = [p["verdict"] for p in parts]
    verdict = ("MISS" if "MISS" in verdicts else
               "HIT" if all(v == "HIT" for v in verdicts) and edge else "INCONCLUSIVE")
    parts.append(_part(f"M={min(ms)} on the grid's upper edge", {}, "yes" if edge else "no"))
    return {"verdict": verdict, "parts": parts}


def p9_h9a(summary: dict, rows: dict, alpha: float) -> dict:
    def col(arm):
        return [rows[f"{x11._arm_key('fast', 'stale', arm)}#{s}"]["maker_sniping_loss"]
                for s in summary["seeds"]]

    cont, parts, verdicts = col(x11.CONTINUOUS), [], []
    for tau in x11.CONFIRM_TAUS_NS:
        batch = col((tau, "time", "engine"))
        d = bootstrap_ci([a - b for a, b in zip(batch, cont)], ci=1 - alpha, seed=0)
        lvl = bootstrap_ci(batch, ci=1 - alpha, seed=0)
        cap = 1.2 * x11.footnote34_bound(tau)
        below = "HIT" if d["upper"] < 0 else ("MISS" if d["lower"] >= 0 else "INCONCLUSIVE")
        under = "HIT" if lvl["upper"] <= cap else ("MISS" if lvl["lower"] > cap else "INCONCLUSIVE")
        verdicts.append(op.all_hit([below, under]))
        parts.append(_part(f"tau={tau / 1e6:g} ms: batch minus continuous", d, below, rule="upper < 0"))
        parts.append(_part(f"tau={tau / 1e6:g} ms: batch loss vs 1.2 x bound", lvl, under,
                           rule=f"upper <= {cap:.4g}"))
    return {"verdict": op.all_hit(verdicts), "parts": parts}


def p10_event_model(summary: dict, rows: dict, alpha: float) -> dict:
    scored, z = summary["bands"]["paper"]["configs"], _z(alpha)
    hits = [x15.is_hit(c["latencies"], c["deltas"]) for c in scored]
    neg = [x15.spearman(c["latencies"], c["deltas"]) < 0 for c in scored]
    close = [h for h, c in zip(hits, scored) if c["closely_spaced"]]
    overall, rho = x15.wilson(sum(hits), len(hits), z), x15.wilson(sum(neg), len(neg), z)
    close_w = x15.wilson(sum(close), len(close), z)
    if overall["upper"] < x15.MISS_OVERALL or (close and close_w["upper"] < x15.MISS_CLOSE):
        verdict = "MISS"
    elif overall["lower"] >= x15.HIT_BAND and rho["lower"] >= x15.HIT_BAND:
        verdict = "HIT"
    else:
        verdict = "INCONCLUSIVE"
    parts = [_part("hit rate (slowest pays)", overall, "-", rule="lower >= 0.80; MISS upper < 0.60"),
             _part("share with Spearman < 0", rho, "-", rule="lower >= 0.80"),
             _part("closely spaced hit rate", close_w, "-", rule="MISS upper < 0.50")]
    return {"verdict": verdict, "parts": parts}


def _joint_ci(y, fn, alpha: float, n_boot: int = 2000, seed: int = 0) -> dict:
    """x13._joint_ci at level 1 - a (same design-row resamples)."""
    s1, st = x13.sa._indices(y[0], y[1], y[2:])
    n = y.shape[1]
    idx = np.random.default_rng(seed).integers(0, n, size=(n_boot, n))
    vals = []
    for c in np.array_split(idx, -(-n_boot // 200)):
        b1, bt = x13.sa._indices(y[0][c], y[1][c], y[2:][:, c])
        vals.append(fn(b1, bt))
    lo, hi = _pct(np.concatenate(vals), alpha)
    return {"mean": float(fn(s1[:, None], st[:, None])[0]), "lower": lo, "upper": hi}


def p11_h11a(summary: dict, rows: dict, alpha: float) -> dict:
    n = summary["n_base"]
    ordered = [rows[x13.design_key(i, n)] for i in range((len(x13.PARAMS) + 2) * n)]
    rent = x13._blocks(ordered, "sniper_rent", n, n)
    ix = {name: i for i, name in enumerate(x13.NAMES)}
    lat = _joint_ci(rent, lambda s1, st: s1[ix["sniper_lat_ns"]], alpha)
    jb = _joint_ci(rent, lambda s1, st: s1[ix["jump_rate"]] + s1[ix["jump_scale_bp"]], alpha)
    if lat["lower"] >= 0.1 or jb["upper"] < 0.35:
        verdict = "MISS"
    else:
        verdict = "HIT" if lat["upper"] < 0.05 and jb["lower"] >= 0.5 else "INCONCLUSIVE"
    parts = [_part("S1 sniper latency (rent)", lat, "-", rule="upper < 0.05; MISS lower >= 0.1"),
             _part("S1 jump rate + S1 b (rent)", jb, "-", rule="lower >= 0.5; MISS upper < 0.35")]
    return {"verdict": verdict, "parts": parts}


class Primary(NamedTuple):
    pid: str
    claim: str
    driver: str
    evaluate: Callable[[dict, dict, float], dict]
    driver_verdict: Callable[[dict], str]


PRIMARIES = (
    Primary("P1", "race-loss share = (N-1)/N +-0.02", "exp9_bcs_replication", p1_race_loss,
            lambda s: s["p1"]["primary_race_loss"]),
    Primary("P2", "H2a maker race-win share = formula +-0.02", "exp16_race_incidence", p2_h2a,
            lambda s: s["H2a"]["verdict"]),
    Primary("P3", "H3a rent ratio jumps / walk +-10%", "exp17_jump_vs_walk", p3_h3a,
            lambda s: s["H3a"]["verdict"]),
    Primary("P4", "H4a entry stops at N* = 5, waste 0.909 +-0.05", "exp10_speed_entry", p4_h4a,
            lambda s: s["p4"]["H4a"]["verdict"]),
    Primary("P5", "H5a taker fee scales snipes, net per snipe = b", "exp18_fee_schedules", p5_h5a,
            lambda s: s["H5a"]["verdict"]),
    Primary("P6", "H6a participation factor +-10%, deadweight > 0", "exp19_noise_heterogeneity",
            p6_h6a, lambda s: s["H6a"]["verdict"]),
    Primary("P7", "H7a cancelling maker loses 1 - p_win +-0.03", "exp20_maker_strategies", p7_h7a,
            lambda s: s["H7a"]["verdict"]),
    Primary("P8", "H8a Nash spread non-increasing in M, M=5 +-20%", "exp12_endogenous_makers",
            p8_h8a, lambda s: s["p8"]["H8a"]["verdict"]),
    Primary("P9", "H9a batch loss below continuous and <= 1.2 x bound", "exp11_batch_semantics",
            p9_h9a, lambda s: s["p9"]["H9a"]["verdict"]),
    Primary("P10", "slowest maker pays in >= 80% of configurations", "exp15_heldout_who_pays",
            p10_event_model, lambda s: s["bands"]["paper"]["p10"]["verdict"]),
    Primary("P11", "H11a S1(latency) < 0.05, S1(lam_j) + S1(b) >= 0.5", "exp13_global_sensitivity",
            p11_h11a, lambda s: s["p11"]["H11a"]["verdict"]),
)

# Secondaries: unadjusted 95%, read from the drivers (§2). (prediction, claim, driver, node)
SECONDARIES = (
    ("P1", "h-hat within +-3% of h* (all cells)", "exp9_bcs_replication", lambda s: s["p1"]["h_hat_all_cells"]),
    ("P1", "h-hat independent of N (slope)", "exp9_bcs_replication", lambda s: s["p1"]["h_hat_slope"]),
    ("P1", "h0-hat within +-3% of h0(N)", "exp9_bcs_replication", lambda s: s["p1"]["h0_hat_all_cells"]),
    ("P1", "profit per snipe within +-3% of b", "exp9_bcs_replication",
     lambda s: s["p1"]["snipe_profit_all_cells"]),
    ("P1", "batch arm exposure", "exp9_bcs_replication", lambda s: s["p1"]["batch_arm"]),
    ("P2", "H2b rent grows as k/(k+1)", "exp16_race_incidence", lambda s: s["H2b"]),
    ("P2", "H2c gaps(21)/gaps(3) < 2", "exp16_race_incidence", lambda s: s["H2c"]),
    ("P2", "H2d maker-gains counts (paper band)", "exp16_race_incidence", lambda s: s["H2d"]),
    ("P3", "H3b profit per snipe b, walk slope < 0", "exp17_jump_vs_walk", lambda s: s["H3b"]),
    ("P3", "H3c fewer maker-gains cells under jumps", "exp17_jump_vs_walk", lambda s: s["H3c"]),
    ("P4", "H4b waste share and invariance across c", "exp10_speed_entry", lambda s: s["p4"]["H4b"]),
    ("P4", "H4c paper maker: dissipation, toll invariant", "exp10_speed_entry", lambda s: s["p4"]["H4c"]),
    ("P5", "H5b all-in half-spread by F", "exp18_fee_schedules", lambda s: s["H5b"]),
    ("P5", "H5c paper maker non-neutral", "exp18_fee_schedules", lambda s: s["H5c"]),
    ("P6", "H6b slow informed edge not positive", "exp19_noise_heterogeneity", lambda s: s["H6b"]),
    ("P6", "H6c inversion vs elasticity", "exp19_noise_heterogeneity", lambda s: s["H6c"]),
    ("P7", "H7b skew raises loss by E[cosh]", "exp20_maker_strategies", lambda s: s["H7b"]),
    ("P7", "H7c ladder loss e^-(h_j-h_1)/b", "exp20_maker_strategies", lambda s: s["H7c"]),
    ("P7", "H7d A-S maker: no inversion", "exp20_maker_strategies", lambda s: s["H7d"]),
    ("P7", "H7e fading leg pattern", "exp20_maker_strategies", lambda s: s["H7e"]),
    ("P8", "H8b active makers fall in F and k", "exp12_endogenous_makers", lambda s: s["p8"]["H8b"]),
    ("P8", "H8c slowest maker lowest", "exp12_endogenous_makers", lambda s: s["p8"]["H8c"]),
    ("P8", "H8d lagging maker exits", "exp12_endogenous_makers", lambda s: s["p8"]["H8d"]),
    ("P9", "H9b tie-break rules", "exp11_batch_semantics", lambda s: s["p9"]["H9b"]),
    ("P9", "H9c no backfire", "exp11_batch_semantics", lambda s: s["p9"]["H9c"]),
    ("P9", "H9d rationing TOST +-2%", "exp11_batch_semantics", lambda s: s["p9"]["H9d"]),
    ("P10", "P10b queue front (literal: a tie is not right)", "exp15_heldout_who_pays",
     lambda s: s["bands"]["paper"]["p10b"]),
    ("P10", "tick-model secondary", "exp15_heldout_who_pays",
     lambda s: s["bands"]["paper"]["p10_tick_secondary"]),
    ("P11", "H11b maker-delta ranks, interactions", "exp13_global_sensitivity", lambda s: s["p11"]["H11b"]),
    ("P11", "H11c toll within +-10% of rent", "exp13_global_sensitivity", lambda s: s["p11"]["H11c"]),
)

# §4's first amendment: verdicts on the paper band, the corrected tape band beside.
BAND_READINGS = (
    ("P2 H2d", "exp16_race_incidence", lambda s: [s["H2d"]["by_band"][b]["verdict"] for b in op.BANDS]),
    ("P3 H3c", "exp17_jump_vs_walk", lambda s: [s["H3c"]["by_band"][b]["verdict"] for b in op.BANDS]),
    ("P5 H5c", "exp18_fee_schedules", lambda s: [s["H5c"]["by_band"][b]["verdict"] for b in op.BANDS]),
    ("P6 H6a (primary)", "exp19_noise_heterogeneity",
     lambda s: [s["H6a"]["verdict"], s["H6a"]["tape_band_verdict"]]),
    ("P6 H6c", "exp19_noise_heterogeneity", lambda s: [s["H6c"]["verdict"], s["H6c"]["tape_band_verdict"]]),
    ("P7 H7d", "exp20_maker_strategies", lambda s: [s["H7d"]["by_band"][b]["verdict"] for b in op.BANDS]),
    ("P7 H7e", "exp20_maker_strategies", lambda s: [s["H7e"]["by_band"][b]["verdict"] for b in op.BANDS]),
    ("P8 H8c", "exp12_endogenous_makers", lambda s: [s["p8"]["H8c"][b]["verdict"] for b in op.BANDS]),
    ("P10 (primary)", "exp15_heldout_who_pays",
     lambda s: [s["bands"][b]["p10"]["verdict"] for b in op.BANDS]),
    ("P10b where named", "exp15_heldout_who_pays",
     lambda s: [s["bands"][b]["p10b"]["verdict_where_named"] for b in op.BANDS]),
)


# --- loading ------------------------------------------------------------------------------

class Driver(NamedTuple):
    name: str
    summary: dict
    rows: dict
    status: str
    confirmatory: bool


def checkpoint_path(results_dir: Path, name: str) -> Path:
    """Confirmatory layout first (checkpoints/<name>.jsonl), then a smoke run's."""
    base = name.removesuffix("_n2048")            # the extension shares exp13's checkpoint
    path = results_dir / "checkpoints" / f"{base}.jsonl"
    return path if path.exists() else results_dir / f"{name}.checkpoint.jsonl"


def load_driver(results_dir: Path, name: str) -> Driver | None:
    path = Path(results_dir) / f"{name}.json"
    if not path.exists():
        return None
    summary = json.loads(path.read_text())
    status = summary.get("status", "<no status>")
    rows = op.CellLog(checkpoint_path(Path(results_dir), name)).done
    return Driver(name, summary, rows, status, status == op.CONFIRMATORY_STATUS)


def load_all(results_dir: Path) -> dict:
    return {n: d for n in DRIVERS + EXTRAS if (d := load_driver(results_dir, n)) is not None}


# --- assessment -----------------------------------------------------------------------------

def _evaluator(primary: Primary, d: Driver) -> Callable[[float], dict]:
    cache: dict = {}

    def at(alpha: float) -> dict:
        if alpha not in cache:
            cache[alpha] = primary.evaluate(d.summary, d.rows, alpha)
        return cache[alpha]
    return at


def _first_pass(primary: Primary, d: Driver | None) -> dict:
    """95% recomputation, the driver's own verdict, and p (1 when no HIT claim is open).
    A non-confirmatory driver is still recomputed, to be shown, but never enters Holm."""
    base = {"pid": primary.pid, "claim": primary.claim, "driver": primary.driver}
    if d is None:
        return {**base, "state": "missing", "p": 1.0}
    state = "ok" if d.confirmatory else "refused"
    base |= {"status": d.status, "confirmatory": d.confirmatory, "state": state}
    try:
        driver95 = primary.driver_verdict(d.summary)
        at = _evaluator(primary, d)
        at95 = at(ALPHA)
        hit_open = d.confirmatory and at95["verdict"] != "MISS"
        p = grid_p_value(lambda a: at(a)["verdict"] == "HIT", holm_thresholds()) if hit_open else 1.0
    except Exception as exc:                    # surfaced as a failure, never swallowed
        return {**base, "state": "error" if d.confirmatory else state,
                "error": f"{type(exc).__name__}: {exc}", "p": 1.0}
    return {**base, "p": p, "_at": at, "driver_verdict_95": driver95,
            "recomputed_95": at95["verdict"], "parts_95": at95["parts"],
            "matches_driver": at95["verdict"] == driver95}


def _holm_verdict(entry: dict, rejected: bool) -> str:
    if entry["state"] == "missing":
        return "MISSING"
    if entry["state"] == "refused":
        return "REFUSED: not confirmatory"
    if entry["state"] == "error":
        return "ERROR"
    if entry["recomputed_95"] == "MISS":
        return "MISS"
    return "HIT" if rejected else "INCONCLUSIVE"


def _at_holm_level(entry: dict, at, threshold: float) -> dict:
    try:
        held = at(threshold)
    except Exception as exc:                    # shown, and listed under failures
        return {"error": f"at the Holm level: {type(exc).__name__}: {exc}"}
    return {"rule_at_holm_level": held["verdict"], "parts_holm": held["parts"]}


def assess_primaries(drivers: dict) -> list:
    entries = [_first_pass(p, drivers.get(p.driver)) for p in PRIMARIES]
    ranks = holm({e["pid"]: e["p"] for e in entries}, ALPHA, FAMILY)
    out = []
    for e in entries:
        h = ranks[e["pid"]]
        at = e.pop("_at", None)
        e |= {"holm_rank": h["rank"], "holm_threshold": h["threshold"],
              "holm_verdict": _holm_verdict(e, h["rejected"])}
        if at is not None:
            e |= _at_holm_level(e, at, h["threshold"])
        out.append(e)
    return out


def _leaves(node) -> list:
    """Every judged interval (a dict with lower, upper and verdict) under a JSON node."""
    if isinstance(node, dict):
        own = [node] if {"lower", "upper", "verdict"} <= node.keys() else []
        return own + [x for v in node.values() for x in _leaves(v)]
    if isinstance(node, list):
        return [x for v in node for x in _leaves(v)]
    return []


def _secondary_row(pid, claim, driver, get, drivers) -> dict:
    d = drivers.get(driver)
    row = {"pid": pid, "claim": claim, "driver": driver}
    if d is None:
        return row | {"verdict_95": "MISSING", "summary": ""}
    try:
        node = get(d.summary)
    except (KeyError, TypeError) as exc:
        return row | {"verdict_95": "ERROR", "summary": f"{type(exc).__name__}: {exc}"}
    verdict = node if isinstance(node, str) else node.get("verdict")
    leaves = [x for x in _leaves(node) if x is not node] or _leaves(node)
    if len(leaves) == 1:
        text = _interval(leaves[0])
    else:
        text = f"{sum(x['verdict'] == 'HIT' for x in leaves)}/{len(leaves)} judged intervals HIT" \
            if leaves else ""
    label = verdict if d.confirmatory else f"{verdict} (NOT CONFIRMATORY)"
    return row | {"verdict_95": label, "summary": text, "confirmatory": d.confirmatory}


def band_readings(drivers: dict) -> list:
    out = []
    for label, driver, get in BAND_READINGS:
        d = drivers.get(driver)
        try:
            paper, tape = get(d.summary) if d else ("MISSING", "MISSING")
        except (KeyError, TypeError) as exc:
            paper, tape = "ERROR", f"{type(exc).__name__}: {exc}"
        out.append({"prediction": label, "driver": driver, "paper_band": paper, "tape_band": tape,
                    "confirmatory": bool(d and d.confirmatory)})
    return out


def p10b_reading(drivers: dict) -> dict | None:
    d = drivers.get("exp15_heldout_who_pays")
    if d is None:
        return None
    b = d.summary["bands"]["paper"]["p10b"]
    return {"literal": b["verdict"], "hit_rate": b["hit_rate"],
            "where_named": b.get("verdict_where_named"), "hit_rate_where_named": b.get("hit_rate_where_named"),
            "n_no_prediction": b.get("n_no_prediction"), "confirmatory": d.confirmatory}


def p11_readings(drivers: dict, primaries: list) -> list:
    """P11 at N = 512 (of record) and, beside it, the N = 2,048 extension if present."""
    p11 = next(e for e in primaries if e["pid"] == "P11")
    out = []
    for name, role in (("exp13_global_sensitivity", "of record"),
                       ("exp13_global_sensitivity_n2048", "extension, not of record")):
        d = drivers.get(name)
        if d is None:
            continue
        ok = d.status.startswith(op.CONFIRMATORY_STATUS)
        row = {"driver": name, "role": role, "n_base": d.summary.get("n_base"),
               "confirmatory": ok, "status": d.status,
               "verdicts_95": {h: d.summary["p11"][h]["verdict"] for h in ("H11a", "H11b", "H11c")}}
        try:
            row["H11a_at_p11_holm_level"] = p11_h11a(d.summary, d.rows, p11["holm_threshold"])["verdict"]
        except Exception as exc:
            row["H11a_at_p11_holm_level"] = f"ERROR {type(exc).__name__}: {exc}"
        out.append(row)
    return out


def deviations(drivers: dict, primaries: list) -> list:
    """Missing drivers, non-confirmatory files, recomputation failures, non-convergence."""
    out = [f"MISSING: {n}.json (driver not finished or not run)"
           for n in DRIVERS if n not in drivers]
    out += [f"NOT CONFIRMATORY: {n}.json has status '{d.status}' — its verdicts are NOT counted"
            for n, d in drivers.items() if not d.status.startswith(op.CONFIRMATORY_STATUS)]
    out += [f"RECOMPUTATION ERROR: {e['pid']} ({e['driver']}): {e['error']}"
            for e in primaries if "error" in e]
    out += [f"MISMATCH: {e['pid']} recomputed {e['recomputed_95']} at 95% but {e['driver']} "
            f"says {e['driver_verdict_95']}" for e in primaries if e.get("matches_driver") is False]
    if (d := drivers.get("exp12_endogenous_makers")) is not None:
        out += [f"NON-CONVERGENCE (exp12): {x}" for x in d.summary.get("non_convergence", [])]
    if (d := drivers.get("exp9_bcs_replication")) is not None:
        rerun = x9._reruns(d.summary["first_pass_40_seeds"])
        if rerun:
            out.append(f"exp9: cells re-run at 2n seeds per spec §5 (ratio, N): {rerun}")
        out += [f"exp9 sanity: investor fill share below floor in ratio={c['ratio']:g}, "
                f"N={c['n_firms']}" for c in d.summary["final"]["cells"]
                if not c["sanity"]["investor_fill_ok"]]
    if (d := drivers.get("exp10_speed_entry")) is not None:
        p4 = d.summary["p4"]
        out += [f"exp10: entry censored at the grid's top ({h}, c = R/{c['rent_multiple']})"
                for h in ("H4b", "H4c") for c in p4[h]["cells"] if c["entry"]["censored"]]
    return out


def build_report(results_dir: Path) -> dict:
    drivers = load_all(Path(results_dir))
    primaries = assess_primaries(drivers)
    counted = [e for e in primaries if e["state"] == "ok"]
    inconclusive = sum(e["holm_verdict"] == "INCONCLUSIVE" for e in counted)
    return {
        "generated_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "results_dir": str(results_dir), "alpha": ALPHA, "family_size": FAMILY,
        "holm_thresholds": holm_thresholds(),
        "confirmatory_drivers": sorted(n for n, d in drivers.items() if d.confirmatory),
        "n_primaries_confirmatory": len(counted),
        "complete": len(counted) == FAMILY,
        "underpowered": inconclusive > FAMILY / 3,
        "n_primaries_inconclusive": inconclusive,
        "primaries": primaries,
        "secondaries": [_secondary_row(*s, drivers) for s in SECONDARIES],
        "band_readings": band_readings(drivers),
        "p10b": p10b_reading(drivers),
        "p11": p11_readings(drivers, primaries),
        "deviations": deviations(drivers, primaries),
    }


# --- rendering -------------------------------------------------------------------------------

def _num(x) -> str:
    if x is None or (isinstance(x, float) and math.isnan(x)):
        return "—"
    return f"{x:.4g}" if isinstance(x, (int, float)) else str(x)


def _interval(p: dict) -> str:
    if p.get("lower") is None:
        return _num(p.get("estimate", p.get("mean")))
    est = p.get("estimate", p.get("mean", p.get("rate")))
    return f"{_num(est)} [{_num(p['lower'])}, {_num(p['upper'])}]"


def _banner(r: dict) -> list:
    blocks = []
    refused = [d for d in r["deviations"] if d.startswith(("NOT CONFIRMATORY", "MISSING"))]
    if not r["complete"]:
        blocks.append([f"> **PARTIAL BATCH — {r['n_primaries_confirmatory']} of {FAMILY} primaries "
                       "have confirmatory results. Nothing below is the program's final verdict.**"])
    if refused:
        blocks.append(["> **REFUSED / MISSING — these are NOT counted as confirmatory:**"]
                      + [f"> - {d}" for d in refused])
    if r["underpowered"]:
        blocks.append([f"> **UNDERPOWERED (§2): {r['n_primaries_inconclusive']} of {FAMILY} "
                       "primaries are INCONCLUSIVE after Holm, more than a third.**"])
    lines = [x for i, b in enumerate(blocks) for x in ([">"] if i else []) + b]
    return lines + [""] if lines else []


def _primary_table(r: dict) -> list:
    lines = ["| P | claim | 95% (driver) | 95% (recomputed) | Holm rank | Holm α_k | p ≤ | "
             "Holm verdict | parts HIT at α_k |", "|---|---|---|---|---|---|---|---|---|"]
    for e in r["primaries"]:
        parts = [p for p in e.get("parts_holm", []) if p["verdict"] in ("HIT", "MISS", "INCONCLUSIVE")]
        hit = f"{sum(p['verdict'] == 'HIT' for p in parts)}/{len(parts)}" if parts else "—"
        p = "—" if e["p"] >= 1 else _num(e["p"])
        lines.append(f"| {e['pid']} | {e['claim']} | {e.get('driver_verdict_95', '—')} | "
                     f"{e.get('recomputed_95', '—')} | {e['holm_rank']} | {_num(e['holm_threshold'])} | "
                     f"{p} | **{e['holm_verdict']}** | {hit} |")
    return lines + [""]


def _primary_details(r: dict) -> list:
    lines = []
    for e in r["primaries"]:
        if "parts_holm" not in e:
            continue
        shown = "" if e["state"] == "ok" else " — NOT CONFIRMATORY: shown, not counted"
        lines += [f"### {e['pid']} — {e['claim']} ({e['driver']}){shown}", "",
                  f"Holm threshold α_k = {_num(e['holm_threshold'])} (level {_num(1 - e['holm_threshold'])}); "
                  f"the full rule at that level reads {e['rule_at_holm_level']}.", "",
                  "| part | 95% interval | Holm-level interval | band / rule | 95% | Holm level |",
                  "|---|---|---|---|---|---|"]
        for a, b in zip(e["parts_95"], e["parts_holm"]):
            rule = _num(a["band"][0]) + " – " + _num(a["band"][1]) if a["band"] else (a["rule"] or "")
            lines.append(f"| {a['label']} | {_interval(a)} | {_interval(b)} | {rule} | "
                         f"{a['verdict']} | {b['verdict']} |")
        lines.append("")
    return lines


def render_markdown(r: dict) -> str:
    lines = ["# Confirmatory results", "",
             f"Generated {r['generated_utc']} by `analysis/confirmatory_report.py` from "
             f"`{r['results_dir']}`. Do not edit by hand.", ""]
    lines += _banner(r)
    lines += ["## Primaries (one family, Holm at α = 0.05, m = 11)", "",
              "A primary HITs after Holm when Holm's step-down rejects its p, the smallest level at "
              "which its HIT conditions hold (shown as its ceiling on Holm's grid). MISS is the "
              "drivers' unadjusted 95% reading. Method and ambiguities: the module docstring.", ""]
    lines += _primary_table(r)
    lines += ["## Secondaries (unadjusted 95%, §2)", "",
              "| P | claim | driver | verdict (95%) | intervals |", "|---|---|---|---|---|"]
    lines += [f"| {s['pid']} | {s['claim']} | {s['driver']} | {s['verdict_95']} | {s['summary']} |"
              for s in r["secondaries"]]
    lines += ["", "## §4 amendments", "", "### Paper band (of record) and corrected tape band", "",
              "| prediction | paper band | tape band | confirmatory |", "|---|---|---|---|"]
    lines += [f"| {b['prediction']} | {b['paper_band']} | {b['tape_band']} | {b['confirmatory']} |"
              for b in r["band_readings"]]
    if r["p10b"]:
        b = r["p10b"]
        lines += ["", "### P10b (secondary): where it names a payer vs literal", "",
                  f"- where named (of record, §4.1): **{b['where_named']}**, hit rate "
                  f"{_interval(b['hit_rate_where_named'] or {})}",
                  f"- literal (a tie counts as not right): {b['literal']}, hit rate {_interval(b['hit_rate'])}",
                  f"- configurations with no prediction: {b['n_no_prediction']}; confirmatory: {b['confirmatory']}"]
    lines += ["", "### P11: N = 512 of record, N = 2,048 beside", ""]
    lines += [f"- {p['driver']} ({p['role']}, N = {p['n_base']}, confirmatory: {p['confirmatory']}): "
              f"95% {p['verdicts_95']}; H11a at P11's Holm level: {p['H11a_at_p11_holm_level']}"
              for p in r["p11"]] or ["- no exp13 output yet"]
    lines += ["", "## Deviations and failures", ""]
    lines += [f"- {d}" for d in r["deviations"]] or ["- none"]
    lines += ["", "## Primary details", ""] + _primary_details(r)
    return "\n".join(lines) + "\n"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--results-dir", type=Path, default=op.RESULTS_DIR)
    ap.add_argument("--out", type=Path, default=OUT_MD, help="markdown path; the JSON twin sits beside it")
    a = ap.parse_args(argv)
    report = build_report(a.results_dir)
    if not report["confirmatory_drivers"]:
        print(f"REFUSED: no summary in {a.results_dir} carries the confirmatory status "
              f"'{op.CONFIRMATORY_STATUS}'. Nothing written.", file=sys.stderr)
        return 2
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(render_markdown(report))
    a.out.with_suffix(".json").write_text(json.dumps(report, indent=1, default=str) + "\n")
    for line in _banner(report):
        print(line, file=sys.stderr)
    print(f"wrote {a.out} and {a.out.with_suffix('.json')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
