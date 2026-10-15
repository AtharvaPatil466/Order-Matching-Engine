"""The eleven primaries' decision rules, each re-judged at any level 1 - a.

Why this exists. confirmatory_report.py applies Holm across the family; it
needs every primary's rule as a function of the level. Each evaluator here
reads a driver's summary JSON and checkpointed rows and returns the full rule
at level 1 - a with every interval it judged. It calls the drivers' own pure
functions where they take a level (op.wilson's z, bootstrap_ci's ci and
n_bootstrap, the drivers' verdict helpers) and re-derives, line for line, the
few intervals whose level or resample count a driver fixes (cc.ratio_ci and
the drivers' private seed-bootstrap loops). At a = 0.05 every evaluator
reproduces its driver's 95% verdict and intervals exactly
(tests/test_confirmatory_report.py). It lives apart from the report so each
file stays focused; nothing here knows about Holm.

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
      walk at h = 567: cc.ratio_ci re-derived at level 1 - a, band 1.30 +- 10%.
  P4  exp10 `p4_verdicts` H4a at c = R/5.5. N* comes from P4's Setup, the
      driver's entry rule ("enter while the next entrant's interval lies above
      c"), at the driver's 95%: it defines the market's equilibrium, not the
      test, and an estimand must not move with the family size (A3). HIT:
      N* = 5 and the waste share n c / rent, from the (1 - a) rent interval,
      inside 0.909 +- 0.05. MISS clauses: N* != 5 with the 5th/6th entrants'
      intervals excluding 5, or waste disjoint from the band.
  P5  exp18 `h5a`. Four paired ratios (snipes per jump vs e^(-f_t/b), net
      profit per snipe vs b, f_t in {100, 300}) via cc.ratio_ci at 1 - a,
      bands +- 5%. HIT: all four inside.
  P6  exp19 H6a. Per v-bar arm on the paper band: participation-over-
      predicted ratio (the driver's joint seed bootstrap, percentiles at a/2)
      inside [0.9, 1.1], and the deadweight interval (cc.boot's bootstrap at 1 - a)
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

Resamples (the report's ambiguity A2): the drivers' 2,000 at a = 0.05, so the
95% recomputation is exact; 20,000 at every Holm-adjusted level, so a tail at
0.05/11 rests on about 45 resamples rather than 4.5. Same seed 0 throughout.
"""
from __future__ import annotations

import json
import math
import sys
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

ALPHA = 0.05                                   # §2's level: the drivers' own 95%
H4A_N_STAR, H4A_WASTE_BAND = 5, (0.909 - 0.05, 0.909 + 0.05)   # as exp10.p4_verdicts


# --- intervals at a level ---------------------------------------------------------------
# The drivers' bootstraps at 1 - a. Percentiles are 100 a/2 and 100 - 100 a/2, which at
# a = 0.05 are exactly the drivers' 2.5 and 97.5; functions that take ci= get 1 - a.
# Resamples: the drivers' 2,000 at a = 0.05, 20,000 at a Holm-adjusted level (A2).

DRIVER_N_BOOT = op.N_BOOTSTRAP       # §2's 2,000, every driver's
HOLM_N_BOOT = 20_000                 # about 45 resamples per tail at 0.05/11


def _n_boot(alpha: float) -> int:
    return DRIVER_N_BOOT if alpha == ALPHA else HOLM_N_BOOT


def _boot(values, alpha: float) -> dict:
    """cc.boot / bootstrap_ci(seed 0) at level 1 - a with _n_boot(a) resamples."""
    return bootstrap_ci(list(values), n_bootstrap=_n_boot(alpha), ci=1 - alpha, seed=op.BOOT_SEED)


def _ratio_ci(num, den, alpha: float) -> dict:
    """cc.ratio_ci at level 1 - a with _n_boot(a) joint resamples. Its count is fixed at
    2,000, so it is re-derived here, line for line (tested equal to it at a = 0.05)."""
    num, den = np.asarray(num, float), np.asarray(den, float)
    rng = np.random.default_rng(op.BOOT_SEED)
    idx = rng.integers(0, len(num), size=(_n_boot(alpha), len(num)))
    with np.errstate(divide="ignore", invalid="ignore"):
        boots = num[idx].mean(1) / den[idx].mean(1)
    boots = boots[np.isfinite(boots)]
    a = (1 - (1 - alpha)) / 2                  # the driver's arithmetic: ci = 1 - alpha first
    lo, hi = (np.percentile(boots, [100 * a, 100 * (1 - a)]) if boots.size
              else (math.nan, math.nan))
    return {"mean": float(num.mean() / den.mean()) if den.mean() else math.nan,
            "lower": float(lo), "upper": float(hi), "n": len(num)}


def _pct(boots, alpha: float) -> tuple:
    q = 100 * alpha / 2
    return float(np.percentile(boots, q)), float(np.percentile(boots, 100 - q))


def _seed_boot(stat, n: int, alpha: float, finite_only: bool = False) -> dict:
    """The drivers' joint seed bootstrap loop (seed 0) at level 1 - a, _n_boot(a) resamples."""
    rng = np.random.default_rng(op.BOOT_SEED)
    boots = np.array([stat(rng.integers(0, n, n)) for _ in range(_n_boot(alpha))])
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
    j = cc.judged(_ratio_ci(rate["laplace"], rate["rw"], alpha), th,
                  th * (1 - x17.RATIO_BAND), th * (1 + x17.RATIO_BAND))
    return _all([_judged("rent rate ratio, Laplace / random walk", j)])


def p4_h4a(summary: dict, rows: dict, alpha: float) -> dict:
    seeds, c = summary["seeds"], x10.P4_COSTS[5.5]

    def col(n_firms, field):
        return [rows[json.dumps([n_firms - 1, False, s])][field] for s in seeds]

    per = {n: col(n, "per_sniper") for n in x10.FIRMS_GRID}
    n_star = x10.entry_stop(per, c, 2)["n_star"]         # Setup's rule, the driver's 95% (A3)
    ci5, ci6 = (_boot(per[j], alpha) for j in (5, 6))
    share = None
    if n_star >= 2:
        r = _boot(col(n_star, "rent"), alpha)
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
        snipes = _ratio_ci(col(f_t, "snipes_per_jump"), col(0.0, "snipes_per_jump"), alpha)
        net = _ratio_ci(col(f_t, "sniper_net"), col(f_t, "sniper_fills"), alpha)
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
        dwl = _boot([np.mean([(b["gains_usd"] - c[i]["gains_usd"]) / b["duration_s"]
                              for c in cells]) for i, b in enumerate(base)], alpha)
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
    spreads = _h8a_spreads(summary, rows)
    ms, parts = sorted(spreads), []
    for a, b in zip(ms, ms[1:]):
        pairs = [y - x for x, y in zip(spreads[a], spreads[b]) if x is not None and y is not None]
        d = _boot(pairs, alpha) if pairs else None
        parts.append(_part(f"spread M={b} minus M={a}", d or {},
                           x12._step(d) if d else "INCONCLUSIVE", rule="upper <= 0"))
    k = summary["market"]["k"]
    n = k + 1
    breakeven = half_spread(op.INVESTOR_RATE_PER_S, op.PATH["jump_intensity_per_s"] * (n - 1) / n,
                            laplace_jump(op.PATH["jump_scale_bp"])) / op.TICK_BP
    band = [0.8 * breakeven, 1.2 * breakeven]
    vals = [v for v in spreads[max(ms)] if v is not None]
    top = _boot(vals, alpha) if vals else None
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
        d = _boot([a - b for a, b in zip(batch, cont)], alpha)
        lvl = _boot(batch, alpha)
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


def _joint_cis(y, fns, alpha: float, seed: int = 0) -> list:
    """x13._joint_ci at level 1 - a for each statistic in fns, _n_boot(a) design-row
    resamples (its count is fixed). The driver redraws the same seed-0 resamples for each
    statistic, so one pass over them serves all: the same numbers at half the cost."""
    n_boot = _n_boot(alpha)
    s1, st = x13.sa._indices(y[0], y[1], y[2:])
    n = y.shape[1]
    idx = np.random.default_rng(seed).integers(0, n, size=(n_boot, n))
    vals = [[] for _ in fns]
    for c in np.array_split(idx, -(-n_boot // 200)):
        b1, bt = x13.sa._indices(y[0][c], y[1][c], y[2:][:, c])
        for v, fn in zip(vals, fns):
            v.append(fn(b1, bt))
    return [dict(zip(("lower", "upper"), _pct(np.concatenate(v), alpha)),
                 mean=float(fn(s1[:, None], st[:, None])[0])) for v, fn in zip(vals, fns)]


def p11_h11a(summary: dict, rows: dict, alpha: float) -> dict:
    n = summary["n_base"]
    ordered = [rows[x13.design_key(i, n)] for i in range((len(x13.PARAMS) + 2) * n)]
    rent = x13._blocks(ordered, "sniper_rent", n, n)
    ix = {name: i for i, name in enumerate(x13.NAMES)}
    lat, jb = _joint_cis(rent, [lambda s1, st: s1[ix["sniper_lat_ns"]],
                                lambda s1, st: s1[ix["jump_rate"]] + s1[ix["jump_scale_bp"]]], alpha)
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
