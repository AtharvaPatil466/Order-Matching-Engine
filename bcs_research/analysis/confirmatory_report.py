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

Every HIT condition (confirmatory_primaries.py) is an interval lying inside a
band or beyond a bound, and the percentile-bootstrap (same resamples, seed 0)
and Wilson intervals are nested in a at a fixed resample count, so across
Holm's adjusted levels (all drawn with the same 20,000 resamples, see A2) the
set of a where a primary HITs is an up-set and p_k is well defined; for a
conjunction of cells it is the largest cell p (an intersection-union test,
valid without a further adjustment). Holm compares the j-th smallest p to
alpha / (m - j + 1), m = 11, and only those 11 thresholds can change a
decision, so p_k is computed as its ceiling on that grid (`grid_p_value`):
exact for Holm's decisions, and it needs the rule evaluated at no more than 11
levels. A primary that is missing, refused (non-confirmatory status) or not a
HIT at the drivers' own 95% enters with p = 1 (with nested intervals a
stricter-level HIT implies the 95% one; the rule keeps that true when the two
levels use different resample counts), so a partial batch is never more
lenient than the full one (Holm is monotone in the others' p-values). The
family size stays 11 whatever has finished.

MISS is NOT adjusted (ambiguity A1 below): the Holm verdict is MISS if the
driver's own 95% rule says MISS, HIT if Holm rejects, INCONCLUSIVE otherwise.
The intervals the report prints for a primary are at its Holm threshold.

Each primary's rule, as its driver computes it, and its HIT conditions at
level 1 - a are written down in confirmatory_primaries.py, which holds the
eleven evaluators; this module holds Holm, loading, the report and the CLI.

Every recomputation reproduces the driver's own 95% verdict at a = 0.05
(tests/test_confirmatory_report.py); a mismatch is reported as a failure.

Ambiguities for the lead to log in §4 (none is silently resolved):
  A1  Holm adjusts HIT claims only; MISS stays at the drivers' 95%. Adjusting
      MISS too would widen the miss intervals and turn misses into
      INCONCLUSIVE (flattering the program, against §1's intent), and P6's
      miss clause "deadweight interval including zero" is a failure-to-reject
      clause that a wider interval makes EASIER, so a symmetric adjustment
      would turn a 95% HIT into a Holm MISS.
  A2  Bootstrap resolution. §2 fixes 2,000 resamples, which at a = 0.05/11
      leaves about 4.5 resamples per tail. At every Holm-ADJUSTED level the
      bootstrap intervals (P3-P9, P11; Wilson needs none) use 20,000
      resamples, same seed 0, so a tail rests on about 45 (lead's decision,
      2026-10-16). At a = 0.05 itself, the plain 95% recomputation and Holm's
      last threshold, they use the drivers' 2,000, so they reproduce the
      drivers' intervals exactly.
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

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT / "experiments") not in sys.path:
    sys.path.insert(0, str(_ROOT / "experiments"))

import operating_point as op  # noqa: E402  (puts build/, agents/, metrics/, ... on sys.path)
import exp9_bcs_replication as x9  # noqa: E402
# The evaluators, and the helpers callers and tests reach as confirmatory_report.<name>.
from confirmatory_primaries import (  # noqa: E402,F401
    ALPHA, PRIMARIES, Primary, _boot, _n_boot, _pct, _ratio_ci, _z, bootstrap_ci, p11_h11a)

FAMILY = 11
OUT_MD = _ROOT / "paper" / "confirmatory_results.md"
# run_confirmatory.sh's drivers, in its order; the last two are extras, not of record.
DRIVERS = ("exp9_bcs_replication", "exp10_speed_entry", "exp11_batch_semantics",
           "exp17_jump_vs_walk", "exp19_noise_heterogeneity", "exp18_fee_schedules",
           "exp20_maker_strategies", "exp16_race_incidence", "exp12_endogenous_makers",
           "exp13_global_sensitivity", "exp15_heldout_who_pays")
EXTRAS = ("exp13_global_sensitivity_n2048", "exp14_incidence_priors")


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
        hit_open = d.confirmatory and at95["verdict"] == "HIT"
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
        if at is not None and e["state"] == "ok":    # outside the family, no Holm level
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
        if not (ok and p11["state"] == "ok"):
            row["H11a_at_p11_holm_level"] = "— (not confirmatory)"
            out.append(row)
            continue
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
        if "parts_holm" not in e:                     # only counted primaries have a Holm level
            continue
        lines += [f"### {e['pid']} — {e['claim']} ({e['driver']})", "",
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
