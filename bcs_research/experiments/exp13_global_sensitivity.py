#!/usr/bin/env python3
"""Experiment 13 — global sensitivity: which hand-set parameters drive incidence (PILOT).

Why. Bootstrap CIs at chosen cells shrink with n however arbitrary the cell
is. A reviewer asked instead for random draws over ALL hand-set parameters at
once and a variance decomposition of the outcomes: which parameters drive
incidence. This is pre-registered as P11 (paper/PREREGISTRATION.md): Saltelli
sampling, Sobol first- and total-order indices, bootstrap intervals.

Market. The event model at the tape-calibrated operating point of
experiments/facts_event_model.py (CFG: 0.10 USD tick, 0.001 BTC lot,
jump_fit_v2 path, reservation noise traders with the tape's size law, the
tape's median top depth), with every hand-set parameter drawn from the ranges
below. M makers split that depth and quote post-only (StrategicMaker
post_only, ENDO lane), so equally fast makers never cross each other's stale
quotes; a refused or emptied quote is re-sent at once (requote_after_fill),
as a real post-only maker re-sends, rather than leaving the side empty until
the next update (with M >= 2 that rule left a side empty on most updates).
Each is the paper's adverse-selection maker (widen on an adverse fill against
true V, decay per observed update). k EventSnipers snipe stale quotes
with a threshold equal to their taker fee. The slow informed investor of the
facts config is left out: it is none of P11's parameters and its 6,182-lot
parent orders add inventory-carry variance that would swamp the indices.

Factors and ranges (P11's where P11 gives one; "x[a, b]" multiplies the tape
operating point, log-uniform):
  jump_rate       x[0.25, 4] of 0.200/s                       P11 (lambda_j)
  jump_scale_bp   x[0.25, 4] of b = 0.705 bp                  P11 (b)
  sniper_lat_ns   x[0.1, 10] of 10 us one way (md + order),   P11 (latency level,
                  every maker kept `lag` behind                lag fixed)
  lag_over_eps    U[0, 2], jitter eps = 50 us on every racer  P11 (lag/eps)
  noise_rate      x[0.25, 4] of the arrival rate that gives   P11 (lambda_noise)
                  the tape's 16.28 crossings/s at base
  noise_eps_bp    x[0.25, 4] of 0.519 bp                      P11 silent: its
                  convention for scale parameters; the base is the path's 1-s
                  volatility (the facts config's stated choice)
  half_spread_ticks  log-U[0.5, 8] ticks                      P11 silent: below
                  half a tick a rounded quote can sit on the wrong side of V
                  (every noise fill then "adverse"); 8 ticks gives the same
                  16x span as P11's multiplicative ranges. The BCS h* at the
                  tape is 0.54 ticks, at the bottom of the range.
  adverse_sensitivity x[0.25, 4] of 0.015 per adverse fill   P11; base is the
                  paper's calibrated value (run_calibrated.py)
  decay_per_s     x[0.25, 4] of 105.4/s                       P11; the paper's
                  0.1 per 1-ms tick as a rate (-ln 0.9 / 1 ms), applied per
                  10-ms update as 1 - exp(-rate * 10 ms). A log-uniform range
                  on the rate is the same range on the time constant.
  snipe_ratio     log-U[0.003, 1] of total depth              P11 (snipe/quote)
  fee_over_b      U[0, 0.3]: taker fee F = fee_over_b * b,    P11 (F); all of F on
                  maker fee 0                                  the taker (P11
                  gives only the total; the split is P5's question)
  n_snipers       U{1..21}                                    P11 (k)
  n_makers        U{1..5}, depth split evenly                 P11 (M)
  seed            run seed                                    the aleatoric
                  factor: its index is the share of variance that is run noise
Not varied: c_speed (it enters no outcome below, only waste = N c, so its
index is mechanically zero, as P11 notes); total depth, held at the tape's
median 6,182 lots (calibrated, not hand-set; rent is linear in it, so a range
would only add a scale term, and the calibration files hold no depth
quantiles to set one from); the 10-ms update cadence and diffusive sigma
(jump_fit_v2's fit).

Outcomes, USD per second (1 tick x 1 lot = 1e-4 USD), each run paired with a
no-sniper run on the same path, noise and scheduler seeds (CRN):
  sniper_rent   snipers' gross trading P&L, every fill marked at true V at
                execution (exp10's edge_pnl: the inventory-carry-free twin of
                an end-of-run V mark);
  maker_delta   makers' net P&L (edge less fees) minus the no-sniper run's;
  noise_delta   noise traders' net P&L minus the no-sniper run's: the toll;
  gap_rate      IOC arrivals that empty one side of a two-sided book, per s.
                The tick model's §3.3 gap is a snapshot-grid episode; on the
                event clock a quote is refilled within the maker's latency,
                so the grid would miss nearly every gap. Counting at the
                arrival that causes it is the event-clock analogue.

Sample size. Saltelli needs n (d + 2) runs, here n x 17 pairs. The pilot
uses n = PILOT_N; the confirmatory n is chosen from the pilot's index CIs
(the half-width scales as 1/sqrt(n); see `recommend_n`).

Status: PILOT. The operating point is not signed off (PREREGISTRATION §5), so
this is not the confirmatory test of P11; output goes to results/pilot/.

Run (background): bcs_research/.venv/bin/python bcs_research/experiments/exp13_global_sensitivity.py [n] [horizon_s] [workers]
"""
from __future__ import annotations

import json
import math
import os
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
for _d in ("build", "agents", "simulation", "metrics", "analysis", "calibration", "experiments"):
    _p = str(_ROOT / _d)
    if _p not in sys.path:
        sys.path.insert(0, _p)

import numpy as np  # noqa: E402

import bcs_engine as be  # noqa: E402
import facts_event_model as facts  # noqa: E402
import sensitivity as sa  # noqa: E402
from bootstrap import bootstrap_ci  # noqa: E402
from bcs_theory import laplace_half_spread  # noqa: E402
from event_noise import ReservationNoiseTrader  # noqa: E402
from event_scheduler import EventScheduler  # noqa: E402
from event_sniper import EventSniper  # noqa: E402
from exp10_speed_entry import edge_pnl  # noqa: E402
from fundamental_paths import JumpDiffusionPath  # noqa: E402
from strategic_maker import MakerPolicy, StrategicMaker  # noqa: E402

STATUS = "pilot — pre-sign-off operating point; not the confirmatory test of P11"
NS_PER_S = 1_000_000_000
TAPE = facts.CFG
TICK_BP = TAPE["tick_bp"]
V0 = TAPE["path"]["v0"]
UPDATE_S = TAPE["path"]["update_interval_ms"] / 1e3
DEPTH_LOTS = TAPE["makers"]["depth_lots_total"]
MAKER0, SNIPER0, NOISE = 100, 200, 300              # participant id blocks
JITTER_NS = 50_000          # ~ ABO 2022's median race length (46 us)
BASE_SNIPER_NS = TAPE["snipers"]["md_latency_ns"] + TAPE["snipers"]["order_latency_ns"]
BASE_SENSITIVITY = 0.015    # run_calibrated.CALIBRATED_ADVERSE_SENSITIVITY
BASE_DECAY_PER_S = -math.log(1 - 0.1) / 1e-3      # paper: 0.1 per 1-ms tick
_MU, _B = TAPE["path"]["jump_intensity_per_s"], TAPE["path"]["jump_scale_bp"]
_EPS0 = TAPE["noise"]["eps_bp"]
H_STAR_TICKS = laplace_half_spread(TAPE["noise"]["trade_rate_per_s"], _MU, _B) / TICK_BP
# Arrivals that cross 16.28/s at the BCS spread, the facts config's rule.
BASE_ARRIVALS = (TAPE["noise"]["trade_rate_per_s"]
                 / facts._crossing_probability(H_STAR_TICKS * TICK_BP, _EPS0))

PARAMS = (
    sa.Param("jump_rate", _MU / 4, _MU * 4, "loguniform"),
    sa.Param("jump_scale_bp", _B / 4, _B * 4, "loguniform"),
    sa.Param("sniper_lat_ns", BASE_SNIPER_NS / 10, BASE_SNIPER_NS * 10, "loguniform"),
    sa.Param("lag_over_eps", 0.0, 2.0),
    sa.Param("noise_rate", BASE_ARRIVALS / 4, BASE_ARRIVALS * 4, "loguniform"),
    sa.Param("noise_eps_bp", _EPS0 / 4, _EPS0 * 4, "loguniform"),
    sa.Param("half_spread_ticks", 0.5, 8.0, "loguniform"),
    sa.Param("adverse_sensitivity", BASE_SENSITIVITY / 4, BASE_SENSITIVITY * 4, "loguniform"),
    sa.Param("decay_per_s", BASE_DECAY_PER_S / 4, BASE_DECAY_PER_S * 4, "loguniform"),
    sa.Param("snipe_ratio", 0.003, 1.0, "loguniform"),
    sa.Param("fee_over_b", 0.0, 0.3),
    sa.Param("n_snipers", 1, 21, "integer"),
    sa.Param("n_makers", 1, 5, "integer"),
    sa.Param("seed", kind="seed"),
)
NAMES = tuple(p.name for p in PARAMS)
OUTCOMES = ("sniper_rent", "maker_delta", "noise_delta", "gap_rate")
# The tape operating point in these coordinates (Experiment 14's fixed values).
BASE_POINT = {
    "jump_rate": _MU, "jump_scale_bp": _B, "sniper_lat_ns": BASE_SNIPER_NS,
    # The facts config's 90-us head start over the snipers: lag > eps, so
    # the maker never wins a race.
    "lag_over_eps": 1.8, "noise_rate": BASE_ARRIVALS, "noise_eps_bp": _EPS0,
    "half_spread_ticks": H_STAR_TICKS, "adverse_sensitivity": BASE_SENSITIVITY,
    "decay_per_s": BASE_DECAY_PER_S, "n_snipers": TAPE["snipers"]["n"], "n_makers": 1,
}
PILOT_N, PILOT_HORIZON_S, DESIGN_SEED = 64, 60.0, 13
PILOT_OUT = _ROOT / "results" / "pilot" / "exp13_global_sensitivity.json"
_IOC = be.OrderType.IOC


class _GapHarness:
    """The engine harness, counting IOC arrivals that leave a two-sided book one-sided.

    Delegates everything; only submit_order is wrapped. The scheduler drives
    the harness synchronously, so the book read after submit_order is the book
    that arrival left behind.
    """

    def __init__(self, harness):
        self._h = harness
        self.gaps = 0

    def __getattr__(self, name):
        return getattr(self._h, name)

    def _two_sided(self, sym):
        return self._h.best_bid(sym) > 0 and self._h.best_ask(sym) > 0

    def submit_order(self, sym, *args):
        watch = args[5] == _IOC and self._two_sided(sym)
        out = self._h.submit_order(sym, *args)
        if watch and not self._two_sided(sym):
            self.gaps += 1
        return out


def _ticks(bp: float) -> float:
    return bp / TICK_BP          # V0 = 1e4 / tick_bp, so 1 bp of V0 is 1 / tick_bp ticks


def build(p: dict, horizon_s: float, with_snipers: bool):
    """(agents, path, race jitter) for one parameter point. Seeds: path p['seed'],
    noise [seed, 1]; the scheduler takes p['seed'] too."""
    seed = p["seed"]
    path = JumpDiffusionPath.from_calibration(
        TAPE["path"] | {"jump_intensity_per_s": p["jump_rate"], "jump_scale_bp": p["jump_scale_bp"]},
        int(horizon_s * NS_PER_S), seed=seed)
    jitter_ns = p.get("jitter_ns", JITTER_NS)          # confirmatory: the operating point's eps
    depth = p.get("depth_lots", DEPTH_LOTS)
    lat = int(round(p["sniper_lat_ns"]))
    lag = int(round(p["lag_over_eps"] * jitter_ns))
    s_md, s_ord = lat // 2, lat - lat // 2
    m = p["n_makers"]
    policy = MakerPolicy(half_spread=p["half_spread_ticks"], qty=max(1, round(depth / m)),
                         adverse_sensitivity=p["adverse_sensitivity"],
                         spread_decay=1 - math.exp(-p["decay_per_s"] * UPDATE_S),
                         post_only=True, requote_after_fill=True)
    agents = [StrategicMaker(MAKER0 + i, policy, path=path, md_latency_ns=s_md + lag // 2,
                             order_latency_ns=s_ord + lag - lag // 2) for i in range(m)]
    if with_snipers:
        fee_ticks = _ticks(fee_table(p)["sniper"][1])     # threshold = own taker fee
        qty = max(1, round(p["snipe_ratio"] * depth))
        agents += [EventSniper(SNIPER0 + j, qty=qty, threshold=fee_ticks, md_latency_ns=s_md,
                               order_latency_ns=s_ord) for j in range(p["n_snipers"])]
    n = TAPE["noise"]
    agents.append(ReservationNoiseTrader(
        NOISE, arrival_rate_per_s=p["noise_rate"], eps_scale=_ticks(p["noise_eps_bp"]), v0=V0,
        qty=n["median_lots"], size_sigma_ln=n["size_sigma_ln"], seed=[seed, 1]))
    jitter = {a.participant_id: jitter_ns for a in agents if a.participant_id < NOISE}
    return agents, path, jitter


CLASSES = ("maker", "sniper", "noise")


def fee_table(p: dict) -> dict:
    """{class: (maker_bps, taker_bps)}. Exp 13: the taker fee F = fee_over_b * b
    for every taker, no maker fee. A point may carry its own table (Exp 14)."""
    if "fee_table" in p:
        return p["fee_table"]
    f = p["fee_over_b"] * p["jump_scale_bp"]
    return {c: (0.0, f) for c in CLASSES}


def _class_of(pid: int) -> str:
    return "noise" if pid >= NOISE else ("sniper" if pid >= SNIPER0 else "maker")


def simulate(p: dict, horizon_s: float, with_snipers: bool) -> dict:
    """One run, reduced to what any fee schedule needs: per-class edge (USD,
    marked at V at execution) and passive / aggressive notional (USD), plus the
    gap count and diagnostics. Fees enter the run only through the snipers'
    threshold, so a no-sniper run can be re-priced under any schedule."""
    agents, path, jitter = build(p, horizon_s, with_snipers)
    h = be.EngineHarness()
    h.add_symbol(1)
    h.start(disable_circuit_breaker=True)
    gh = _GapHarness(h)
    try:
        res = EventScheduler(gh, 1, seed=p["seed"], snapshot_every_ns=NS_PER_S,
                             race_jitter_ns=jitter).run(agents, path)
    finally:
        h.stop()
    edge = edge_pnl(res.trades, path)
    out = {f"{c}_{k}": 0.0 for c in CLASSES for k in ("edge", "maker_notional", "taker_notional")}
    for pid, e in edge.items():
        out[f"{_class_of(pid)}_edge"] += e
    sniper_fills = 0
    for tr in res.trades:
        buyer, seller = _class_of(tr.buyer_id), _class_of(tr.seller_id)
        taker, maker = (buyer, seller) if tr.aggressor_buy else (seller, buyer)
        notional = tr.price * tr.quantity / be.PRICE_PRECISION
        out[f"{taker}_taker_notional"] += notional
        out[f"{maker}_maker_notional"] += notional
        sniper_fills += "sniper" in (buyer, seller)
    return out | {"gaps": gh.gaps, "sniper_fills": sniper_fills,
                  "maker_rejects": sum(getattr(a, "rejects", 0) for a in agents),
                  "edge_residual": sum(edge.values())}


def _fees(run: dict, table: dict) -> dict:
    """Fees per class (USD), as accounting.build_ledger books them: maker_bps on
    passive notional, taker_bps on aggressive notional."""
    return {c: 1e-4 * (table[c][0] * run[f"{c}_maker_notional"]
                       + table[c][1] * run[f"{c}_taker_notional"]) for c in CLASSES}


def outcomes_from(w: dict, o: dict, table: dict, horizon_s: float) -> dict:
    """The outcomes from a with-sniper run `w` and its matched no-sniper run `o`."""
    fw, fo = _fees(w, table), _fees(o, table)
    net = lambda r, f, c: r[f"{c}_edge"] - f[c]  # noqa: E731
    per_s = 1.0 / horizon_s
    row = {
        "sniper_rent": w["sniper_edge"] * per_s,
        "maker_delta": (net(w, fw, "maker") - net(o, fo, "maker")) * per_s,
        "noise_delta": (net(w, fw, "noise") - net(o, fo, "noise")) * per_s,
        "gap_rate": w["gaps"] * per_s,
        # Extras: levels, the no-sniper arm, diagnostics.
        "sniper_net": net(w, fw, "sniper") * per_s,
        "exchange_delta": (sum(fw.values()) - sum(fo.values())) * per_s,
        "maker_net_base": net(o, fo, "maker") * per_s,
        "noise_net_base": net(o, fo, "noise") * per_s,
        "gap_rate_base": o["gaps"] * per_s,
        "sniper_fills": w["sniper_fills"],
        "maker_rejects": w["maker_rejects"],
    }
    # Net P&L plus exchange revenue is zero in each arm, so the deltas close:
    # snipers' net = -(maker + noise + exchange deltas). Kept as a check.
    row["identity_residual"] = (row["sniper_net"] + row["maker_delta"] + row["noise_delta"]
                                + row["exchange_delta"])
    row["edge_residual"] = max(abs(w["edge_residual"]), abs(o["edge_residual"]))
    return row


def outcomes(p: dict, horizon_s: float) -> dict:
    """The outcomes for one point, against its matched no-sniper run (CRN)."""
    return outcomes_from(simulate(p, horizon_s, True), simulate(p, horizon_s, False),
                         fee_table(p), horizon_s)


def _task(args):
    p, horizon_s = args
    return outcomes(p, horizon_s)


def run_points(points, horizon_s, workers=None) -> list[dict]:
    """Outcomes for every point, in order; a process pool unless workers == 1."""
    tasks = [(p, horizon_s) for p in points]
    if workers == 1:
        return [_task(t) for t in tasks]
    with ProcessPoolExecutor(max_workers=workers or max(1, (os.cpu_count() or 2) - 2)) as ex:
        return list(ex.map(_task, tasks, chunksize=1))


def design_points(n: int, seed: int = DESIGN_SEED):
    """Points for A, B and every AB_i, in that order, and the unit matrices."""
    A, B = sa.saltelli_design(len(PARAMS), n, seed)
    mats = [A, B] + [sa.ab_matrix(A, B, i) for i in range(len(PARAMS))]
    return [pt for U in mats for pt in sa.values(PARAMS, U)], A, B


def analyse(rows: list[dict], n: int, n_boot: int = 2000) -> dict:
    """Sobol table per outcome from rows ordered as design_points returns them."""
    d = len(PARAMS)
    out = {}
    for key in OUTCOMES:
        y = np.array([r[key] for r in rows]).reshape(d + 2, n)
        out[key] = sa.sobol_with_ci(y[0], y[1], y[2:], NAMES, n_boot=n_boot)
    return out


def recommend_n(tables: dict, n: int, target_half_width: float = 0.05) -> dict:
    """n at which the widest total-order CI among factors with ST above the
    seed's would shrink to +-target (half-width ~ 1/sqrt(n)), rounded up to a
    power of 2, per outcome."""
    rec = {}
    for key, t in tables.items():
        idx = t["indices"]
        floor = idx["seed"]["ST"]
        widths = [(r["ST_hi"] - r["ST_lo"]) / 2 for name, r in idx.items()
                  if name != "seed" and r["ST"] > floor] or [0.0]
        need = n * (max(widths) / target_half_width) ** 2
        rec[key] = {"widest_half_width": max(widths), "n_needed": int(2 ** math.ceil(math.log2(max(need, 2))))}
    return rec


def table(tables: dict) -> str:
    lines = []
    for key, t in tables.items():
        lines.append(f"\n{key} (var {t['variance']:.4g})  S1 [95% CI]   ST [95% CI]")
        ranked = sorted(t["indices"].items(), key=lambda kv: -kv[1]["ST"])
        for name, r in ranked:
            lines.append(f"  {name:20s} {r['S1']:6.3f} [{r['S1_lo']:6.3f},{r['S1_hi']:6.3f}]"
                         f"  {r['ST']:6.3f} [{r['ST_lo']:6.3f},{r['ST_hi']:6.3f}]")
    return "\n".join(lines)


def _summary(rows):
    keys = OUTCOMES + ("sniper_net", "exchange_delta", "gap_rate_base", "sniper_fills",
                       "maker_rejects")
    return {k: {q: float(np.quantile([r[k] for r in rows], q)) for q in (0.1, 0.5, 0.9)}
            | {"mean": float(np.mean([r[k] for r in rows]))} for k in keys}


def main(n=PILOT_N, horizon_s=PILOT_HORIZON_S, out_path=PILOT_OUT, workers=None) -> dict:
    t0 = time.time()
    points, A, B = design_points(n)
    print(f"{len(points)} points x 2 arms, horizon {horizon_s}s", flush=True)
    rows = run_points(points, horizon_s, workers)
    tables = analyse(rows, n)
    report = {
        "experiment": "exp13_global_sensitivity", "status": STATUS,
        "n_base": n, "d": len(PARAMS), "runs": 2 * len(points), "horizon_s": horizon_s,
        "design_seed": DESIGN_SEED, "jitter_ns": JITTER_NS, "depth_lots": DEPTH_LOTS,
        "params": [vars(p) for p in PARAMS], "base_point": BASE_POINT,
        "units": "USD per second (1 tick x 1 lot = 1e-4 USD); gap_rate per second",
        "estimators": "S1 Saltelli 2010, ST Jansen 1999; 95% percentile bootstrap over design "
                      "rows, 2000 resamples, seed 0",
        "deviations_from_p11": [
            "factors added beyond P11's list, per the reviewer's 'all hand-set parameters': "
            "noise_eps_bp, half_spread_ticks; c_speed omitted (mechanically zero on these "
            "outcomes); a seed factor measures run noise",
            "F carried entirely by the taker (P11 gives the total only)",
            "gap_rate is the event-clock analogue (IOC empties a side), not the §3.3 grid count",
        ],
        "sobol": tables,
        "recommended_confirmatory_n": recommend_n(tables, n),
        "outcome_quantiles": _summary(rows),
        "max_identity_residual": max(abs(r["identity_residual"]) for r in rows),
        "max_edge_residual": max(r["edge_residual"] for r in rows),
        "rows": rows,
        "elapsed_s": round(time.time() - t0, 1),
    }
    out_path = Path(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(report, indent=1) + "\n")
    print(table(tables))
    print("recommended n:", json.dumps(report["recommended_confirmatory_n"]))
    print(f"wrote {out_path} in {report['elapsed_s']}s")
    return report


# --- confirmatory mode: P11 at the operating point ------------------------------
#
# The pilot's design (factors, ranges, outcomes, estimators) moved onto §5's
# operating point: the sniper latency level is x[0.1, 10] of the snipers' 100 us,
# lag/eps runs over [0, 2] of eps = 80 us, the depth is the 6,180-lot quote and
# every racer draws the 80 us jitter. Everything else is P11's range or the
# pilot's documented choice (see `deviations_from_p11` in the report).
#
# Sample size: N_base = 2,048, not P11's written 512. P11's verdicts are
# interval tests on indices against thresholds 0.05-0.5 apart (H11a: latency
# S1 < 0.05 with its interval; H11b: sum(ST - S1) >= 0.2; H11c: +-10% of rent);
# the pilot's index intervals were about +-0.1 at n = 64, so at 512 they are
# about +-0.035 and H11a's "< 0.05" stays INCONCLUSIVE for any true index above
# ~0.015. 2,048 halves that. Changing a written sample size after sign-off needs
# a §4 row, so the report also carries the verdicts at N = 512: a scrambled
# Sobol' sequence is extensible, so the first 512 rows of the 2,048-row design
# ARE the 512-row design (tested), and that verdict costs nothing.
#   H11a  sniper_rent: S1(sniper_lat_ns) interval below 0.05, and S1(jump_rate)
#         + S1(jump_scale_bp) at least 0.5 (joint bootstrap of the sum); MISS
#         if the latency S1 is >= 0.1 or the sum < 0.35.
#   H11b  maker_delta: adverse_sensitivity and snipe_ratio in the top three
#         total-order indices (point estimates; the seed factor is run noise,
#         not one of P11's parameters, so it is ranked out), and
#         sum(ST - S1) >= 0.2 over P11's parameters; MISS if either is outside
#         the top three or the sum < 0.1.
#   H11c  (recast, §4) toll = -noise_delta over the A and B rows (independent
#         prior draws): its 95% interval inside +-10% of mean sniper rent.

CONFIRM_N = 2048
PREREG_N = 512


def confirm_params():
    import operating_point as op
    base = op.SNIPER_LATENCY_NS
    return tuple(sa.Param("sniper_lat_ns", base / 10, base * 10, "loguniform")
                 if q.name == "sniper_lat_ns" else
                 sa.Param("noise_rate", op.INVESTOR_ARRIVAL_PER_S / 4, op.INVESTOR_ARRIVAL_PER_S * 4,
                          "loguniform") if q.name == "noise_rate" else q for q in PARAMS)


def confirm_points(n: int):
    import operating_point as op
    params = confirm_params()
    A, B = sa.saltelli_design(len(params), n, DESIGN_SEED)
    mats = [A, B] + [sa.ab_matrix(A, B, i) for i in range(len(params))]
    extra = {"jitter_ns": op.JITTER_NS, "depth_lots": op.QUOTE_LOTS}
    return [pt | extra for U in mats for pt in sa.values(params, U)]


def _blocks(rows, key, n_full, n):
    """(d + 2, n) outcome matrix from rows ordered as confirm_points(n_full), first n rows."""
    return np.array([r[key] for r in rows]).reshape(len(PARAMS) + 2, n_full)[:, :n]


def _joint_ci(y, fn, n_boot=2000, seed=0):
    """Point value and 95% bootstrap interval of fn(S1, ST), resampling design rows."""
    s1, st = sa._indices(y[0], y[1], y[2:])
    n = y.shape[1]
    idx = np.random.default_rng(seed).integers(0, n, size=(n_boot, n))
    vals = []
    for c in np.array_split(idx, -(-n_boot // 200)):
        b1, bt = sa._indices(y[0][c], y[1][c], y[2:][:, c])
        vals.append(fn(b1, bt))
    vals = np.concatenate(vals)
    lo, hi = np.percentile(vals, [2.5, 97.5])
    return {"mean": float(fn(s1[:, None], st[:, None])[0]), "lower": float(lo), "upper": float(hi)}


def p11_verdicts(rows, n_full, n) -> dict:
    import operating_point as op
    ix = {name: i for i, name in enumerate(NAMES)}
    params = [i for name, i in ix.items() if name != "seed"]
    rent = _blocks(rows, "sniper_rent", n_full, n)
    lat = _joint_ci(rent, lambda s1, st: s1[ix["sniper_lat_ns"]])
    jb = _joint_ci(rent, lambda s1, st: s1[ix["jump_rate"]] + s1[ix["jump_scale_bp"]])
    if lat["lower"] >= 0.1 or jb["upper"] < 0.35:
        h11a = "MISS"
    else:
        h11a = "HIT" if lat["upper"] < 0.05 and jb["lower"] >= 0.5 else "INCONCLUSIVE"
    md = _blocks(rows, "maker_delta", n_full, n)
    s1, st = sa._indices(md[0], md[1], md[2:])
    top3 = [NAMES[i] for i in sorted(params, key=lambda i: -st[i])[:3]]
    inter = _joint_ci(md, lambda b1, bt: (bt[params] - b1[params]).sum(0))
    named_in = {"adverse_sensitivity", "snipe_ratio"} <= set(top3)
    if not named_in or inter["upper"] < 0.1:
        h11b = "MISS"
    else:
        h11b = "HIT" if inter["lower"] >= 0.2 else "INCONCLUSIVE"
    ab_rows = rows[:2 * n_full]
    use = [r for blk in (ab_rows[:n_full][:n], ab_rows[n_full:][:n]) for r in blk]
    toll = bootstrap_ci([-r["noise_delta"] for r in use], seed=0)
    mean_rent = float(np.mean([r["sniper_rent"] for r in use]))
    h11c = op.band_verdict(toll["lower"], toll["upper"], -0.1 * mean_rent, 0.1 * mean_rent)
    return {"n_base": n,
            "H11a": {"verdict": h11a, "S1_sniper_latency": lat, "S1_jump_rate_plus_b": jb},
            "H11b": {"verdict": h11b, "top3_total_order": top3,
                     "total_order": {NAMES[i]: float(st[i]) for i in params},
                     "interactions_sum_ST_minus_S1": inter},
            "H11c": {"verdict": h11c, "toll_usd_per_s": toll, "mean_sniper_rent": mean_rent,
                     "band": [-0.1 * mean_rent, 0.1 * mean_rent], "rows": "A and B"}}


def _confirm_task(p, horizon_s):
    return outcomes(p, horizon_s)


def confirmatory(argv) -> dict:
    import argparse
    import operating_point as op
    ap = argparse.ArgumentParser(description="exp13 confirmatory (P11)")
    ap.add_argument("--confirmatory", action="store_true")
    ap.add_argument("--n", type=int, default=CONFIRM_N, help="smoke only: a smaller power of 2")
    ap.add_argument("--horizon-s", type=float, default=op.HORIZON_S)
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--out-dir", type=Path, default=None)
    a = ap.parse_args(argv)
    out, ck = op.confirmatory_paths("exp13_global_sensitivity", a.out_dir)
    log = op.CellLog(ck)
    t0 = time.time()
    points = confirm_points(a.n)
    # The N = 512 design's rows run first, so a batch stopped early still has P11's
    # written sample size complete.
    order = sorted(range(len(points)), key=lambda i: (i % a.n >= PREREG_N, i))
    got = op.run_cached(_confirm_task, {str(i): (points[i], a.horizon_s) for i in order},
                        log, a.workers)
    rows = [got[str(i)] for i in range(len(points))]
    full = p11_verdicts(rows, a.n, a.n)
    prereg = p11_verdicts(rows, a.n, min(PREREG_N, a.n))
    status = op.CONFIRMATORY_STATUS if a.n >= CONFIRM_N and a.horizon_s >= op.HORIZON_S else \
        f"smoke — NOT confirmatory: N = {a.n} x {a.horizon_s:g} s against {CONFIRM_N} x {op.HORIZON_S} s"
    report = {
        "experiment": "exp13_global_sensitivity (P11), confirmatory", "status": status,
        "n_base": a.n, "d": len(PARAMS), "runs": 2 * len(points), "horizon_s": a.horizon_s,
        "design_seed": DESIGN_SEED, "jitter_ns": op.JITTER_NS, "depth_lots": op.QUOTE_LOTS,
        "params": [vars(q) for q in confirm_params()],
        "p11": full, "p11_at_prereg_n512": prereg,
        "verdicts_read_on": f"N = {a.n} (lead's instruction); P11 writes N_base = 512, whose "
                            "verdicts are p11_at_prereg_n512 — the change needs a §4 row",
        "deviations_from_p11": [
            "factors added beyond P11's list: noise_eps_bp, half_spread_ticks; c_speed omitted "
            "(mechanically zero on these outcomes); a seed factor measures run noise",
            "F carried entirely by the taker (P11 gives the total only)",
            "gap_rate is the event-clock analogue (IOC empties a side), not the §3.3 grid count",
            "N_base 2048 instead of 512",
        ],
        "holm": "95% verdicts; Holm across the 11 primaries is applied at write-up",
        "sobol": analyse(rows, a.n),
        "max_identity_residual": max(abs(r["identity_residual"]) for r in rows),
        "elapsed_s": round(time.time() - t0, 1),
    }
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=1) + "\n")
    print(f"[exp13] N={a.n}: H11a {full['H11a']['verdict']}, H11b {full['H11b']['verdict']}, "
          f"H11c {full['H11c']['verdict']} (N=512: {prereg['H11a']['verdict']}, "
          f"{prereg['H11b']['verdict']}, {prereg['H11c']['verdict']}) -> {out}", flush=True)
    return report


if __name__ == "__main__":
    a = sys.argv[1:]
    if "--confirmatory" in a:
        confirmatory(a)
        sys.exit(0)
    main(int(a[0]) if a else PILOT_N, float(a[1]) if len(a) > 1 else PILOT_HORIZON_S,
         workers=int(a[2]) if len(a) > 2 else None)
