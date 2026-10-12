"""Shared pieces of the confirmatory drivers exp16-exp20 (PREREGISTRATION P2, P3, P5, P6, P7).

Why one private module: all five need the same resumable process pool, the
same command line (a --confirmatory run vs a --seeds/--out smoke), the §2
rules operating_point.py does not already carry (paired ratio intervals, TOST,
combining a hypothesis's cells), the tape-band grid, the paper's maker
re-implemented on the event clock, a per-class P&L reduction of one run, and
P1's BCS-unit market for the predictions stated in BCS units (H3a/b, H5a/b).
Everything at the operating point comes from operating_point.py; nothing in
§5 is retyped here.

The paper's maker on the event clock (`paper_policy`). agents/bcs_market_maker.py
widens its half-spread x(1 + sensitivity) on each fill that was adverse against
the true V, decays it back toward base once per requote, and thins resting size
by the same factor. StrategicMaker runs exactly that mechanism (MakerPolicy's
adverse_* / spread_decay / thin_liquidity fields), cancelling and requoting on
every observed update. Parameters are the paper's calibrated ones: sensitivity
0.015 (run_calibrated.CALIBRATED_ADVERSE_SENSITIVITY), decay 0.1 per tick,
cap 10x base, size floor 1. The pre-registration maps one tick to 100 ms on
the event clock (P10, and H2d's "300 ms (the tick model's 3 ticks)"), so the
per-tick decay becomes 1 - 0.9^(10 ms / 100 ms) per 10-ms update. The tick
model's latency acted on observing AND on posting, so a latency here is one
way (md = order), as build_market takes it. Base half-spread: the operating
point's (BCS zero-profit, default_policy). The tick maker's inventory skew is
not carried over: the lead's spec names widening, decay and thinning only, and
a 0.2-tick-per-unit skew on a 6,180-lot quote would dominate everything else.
"""
from __future__ import annotations

import argparse
import math
import os
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

import numpy as np

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import operating_point as op  # noqa: E402  (puts build/, agents/, ... on sys.path)

import bcs_engine as be  # noqa: E402
from bootstrap import bootstrap_ci  # noqa: E402
from event_scheduler import EventScheduler  # noqa: E402

SMOKE_STATUS = "smoke — a sizing run of the confirmatory mode on a few seeds; not a test"
DEFAULT_WORKERS = 8
FIRST_SEED = 1                       # seeds 1..n, as facts_event_model and exp15 number them

# --- the paper's maker on the event clock -------------------------------------
PAPER_TICK_NS = 100_000_000          # P10 / H2d: one tick model tick = 100 ms
PAPER_LATENCY_NS = 3 * PAPER_TICK_NS  # the paper's 3-tick maker, one way
PAPER_SENSITIVITY = 0.015
PAPER_DECAY_PER_TICK = 0.1
PAPER_CAP_MULT = 10.0


def paper_policy(qty: int = op.QUOTE_LOTS, **overrides):
    """bcs_market_maker.BCSMarketMaker's mechanism as a MakerPolicy (module doc)."""
    per_update = 1.0 - (1.0 - PAPER_DECAY_PER_TICK) ** (op.UPDATE_INTERVAL_NS / PAPER_TICK_NS)
    kw = dict(adverse_sensitivity=PAPER_SENSITIVITY, spread_decay=per_update,
              spread_cap_mult=PAPER_CAP_MULT, thin_liquidity=True, min_quote_qty=1)
    return op.default_policy(qty, **(kw | overrides))


# --- the command line and the resumable pool ----------------------------------

def parse_args(name: str, doc: str, argv=None) -> argparse.Namespace:
    """--confirmatory: n = 100 seeds into results/experiments/<name>.json with
    CONFIRMATORY_STATUS. Otherwise a smoke: --seeds n into --out, smoke status.
    Either resumes from its checkpoint (one JSONL line per finished run)."""
    ap = argparse.ArgumentParser(description=doc.split("\n")[0])
    ap.add_argument("--confirmatory", action="store_true")
    ap.add_argument("--seeds", type=int, default=2, help="smoke only: seeds per cell")
    ap.add_argument("--out", type=Path, help="smoke only: summary JSON path")
    ap.add_argument("--workers", type=int,
                    default=int(os.environ.get("WORKERS", DEFAULT_WORKERS)))
    ap.add_argument("--out-dir", type=Path, default=None,
                    help="confirmatory output directory (run_confirmatory.sh passes it)")
    a = ap.parse_args(argv)
    if a.confirmatory:
        a.n_seeds, a.status = op.N_SEEDS, op.CONFIRMATORY_STATUS
        a.out, a.checkpoint = op.confirmatory_paths(name, a.out_dir)
    else:
        if a.out is None:
            ap.error("a smoke run needs --out (or pass --confirmatory)")
        a.n_seeds, a.status = a.seeds, SMOKE_STATUS
        a.checkpoint = a.out.with_suffix(".checkpoint.jsonl")
    a.seed_list = list(range(FIRST_SEED, FIRST_SEED + a.n_seeds))
    return a


def run_pending(tasks: dict, fn, checkpoint: Path, workers: int, label: str) -> dict:
    """Run fn(*args) for every {key: args} not yet in the checkpoint; return {key: row}.

    One pool for the whole call (heaviest-first order is the caller's). Each row
    is logged the moment it finishes, so a killed run resumes where it stopped.
    """
    log = op.CellLog(checkpoint)
    todo = [k for k in tasks if k not in log]
    t0 = time.time()
    print(f"[{label}] {len(tasks)} runs, {len(tasks) - len(todo)} checkpointed, "
          f"{len(todo)} to run on {workers} workers", flush=True)

    def done(i, key, row):
        log.put(key, row)
        if i % 100 == 0 or i == len(todo):
            print(f"[{label}] {i}/{len(todo)} runs, {time.time() - t0:.0f} s", flush=True)

    if workers == 1:
        for i, k in enumerate(todo, 1):
            done(i, k, fn(*tasks[k]))
    elif todo:
        with ProcessPoolExecutor(max_workers=workers) as pool:
            futures = {pool.submit(fn, *tasks[k]): k for k in todo}
            for i, fut in enumerate(as_completed(futures), 1):
                done(i, futures[fut], fut.result())
    return {k: log.get(k) for k in tasks}


def call(fn, *args):
    """run_pending's worker: tasks are (function, *args) tuples."""
    return fn(*args)


def key(*parts) -> str:
    return "|".join(str(p) for p in parts)


# --- §2's statistics ------------------------------------------------------------

def boot(values, ci: float = 0.95) -> dict:
    """§2: 95% percentile bootstrap of the mean, 2,000 resamples, seed 0."""
    return bootstrap_ci(list(values), n_bootstrap=op.N_BOOTSTRAP, ci=ci, seed=op.BOOT_SEED)


def ratio_ci(num, den, ci: float = 0.95) -> dict:
    """mean(num) / mean(den), seeds resampled jointly so a CRN pairing survives."""
    num, den = np.asarray(num, float), np.asarray(den, float)
    rng = np.random.default_rng(op.BOOT_SEED)
    idx = rng.integers(0, len(num), size=(op.N_BOOTSTRAP, len(num)))
    with np.errstate(divide="ignore", invalid="ignore"):
        boots = num[idx].mean(1) / den[idx].mean(1)
    boots = boots[np.isfinite(boots)]          # resamples whose denominator is 0
    a = (1 - ci) / 2
    lo, hi = (np.percentile(boots, [100 * a, 100 * (1 - a)]) if boots.size
              else (math.nan, math.nan))
    return {"mean": float(num.mean() / den.mean()) if den.mean() else math.nan,
            "lower": float(lo), "upper": float(hi), "n": len(num)}


def judged(ci: dict, theory: float, lo: float, hi: float) -> dict:
    """An interval with its band and §2's band verdict."""
    return {**ci, "theory": theory, "band": [lo, hi],
            "verdict": op.band_verdict(ci["lower"], ci["upper"], lo, hi)}


def tost(ci90: dict, ci95: dict, lo: float, hi: float) -> str:
    """§2 equivalence by TOST at alpha = 0.05: HIT when the 90% interval lies in
    [lo, hi]; MISS when the 95% interval is disjoint from it; else INCONCLUSIVE."""
    if lo <= ci90["lower"] and ci90["upper"] <= hi:
        return "HIT"
    if ci95["upper"] < lo or ci95["lower"] > hi:
        return "MISS"
    return "INCONCLUSIVE"


def combine(verdicts) -> str:
    """A hypothesis over several cells: MISS if any cell misses, HIT if all hit."""
    v = list(verdicts)
    if "MISS" in v:
        return "MISS"
    return "HIT" if v and all(x == "HIT" for x in v) else "INCONCLUSIVE"


# --- the tape band (§2, §4) ------------------------------------------------------

def band_cells(bands=None, ks=op.K_GRID) -> list:
    """(band, ratio, k) for every tape-band cell; "paper" carries the verdict (§4)."""
    return [(band, r, k) for band, ratios in (bands or op.BANDS).items()
            for r in ratios for k in ks]


def maker_gains(deltas) -> tuple[bool, dict]:
    """§2/§4.1 cell rule: maker-gains when the paired delta's 95% interval is above 0."""
    ci = boot(deltas)
    return ci["lower"] > 0, ci


def band_counts(cells: dict, base: list, fees=None) -> dict:
    """Maker-gains count per band. cells: {(band, ratio, k): rows by seed}; base:
    the matched no-HFT rows in the same seed order. Deltas are maker P&L net of
    `fees` (the operating point's by default), with minus without HFTs."""
    fees = fees or op.FEES
    out: dict = {}
    for (band, ratio, k), rows in cells.items():
        deltas = [net(r, "maker", fees) - net(b, "maker", fees) for r, b in zip(rows, base)]
        gains, ci = maker_gains(deltas)
        b = out.setdefault(band, {"count": 0, "n_cells": 0, "cells": []})
        b["count"] += int(gains)
        b["n_cells"] += 1
        b["cells"].append({"ratio": ratio, "k": k, "maker_gains": gains, "delta_usd": ci})
    return out


# --- one run at the operating point ----------------------------------------------

CLASSES = ("maker", "sniper", "noise", "informed")
INFORMED = 400                      # facts_event_model's id block, after op.NOISE


def class_of(pid: int) -> str:
    if pid >= INFORMED:
        return "informed"
    if pid >= op.NOISE:
        return "noise"
    return "sniper" if pid >= op.SNIPER else "maker"


def simulate(mk, seed: int, scheduler_cls=EventScheduler, snapshot_ms: int = op.SNAPSHOT_MS):
    """operating_point.simulate with a choice of scheduler; returns (result, scheduler)."""
    h = be.EngineHarness()
    h.add_symbol(1)
    h.start(disable_circuit_breaker=True)
    try:
        sched = scheduler_cls(h, 1, seed=seed, snapshot_every_ns=snapshot_ms * 1_000_000,
                              race_jitter_ns=mk.race_jitter_ns)
        return sched.run(mk.agents, mk.path), sched
    finally:
        h.stop()


def legs(res) -> dict:
    """Per class: trading edge against the true V at execution (USD) and passive /
    aggressive notional (USD), so fees under any schedule are a re-pricing, not a
    re-run. Plus the realised mean quoted half-spread (ticks, two-sided snapshots)."""
    out = {f"{c}_{k}": 0.0 for c in CLASSES for k in ("edge", "passive", "aggressive")}
    sniper_fills = 0
    for tr in res.trades:
        b, s = class_of(tr.buyer_id), class_of(tr.seller_id)
        e = (res.path.value_at(tr.t_ns) - tr.price) * tr.quantity / be.PRICE_PRECISION
        out[f"{b}_edge"] += e
        out[f"{s}_edge"] -= e
        taker, maker = (b, s) if tr.aggressor_buy else (s, b)
        notional = tr.price * tr.quantity / be.PRICE_PRECISION
        out[f"{taker}_aggressive"] += notional
        out[f"{maker}_passive"] += notional
        sniper_fills += "sniper" in (b, s)
    two = [(x.best_ask - x.best_bid) / 2 for x in res.snapshots
           if x.best_bid > 0 and x.best_ask > x.best_bid]
    out["half_spread_ticks"] = float(np.mean(two)) if two else math.nan
    out["sniper_fills"] = sniper_fills
    out["n_jumps"] = len(res.races)
    return out


def maker_run(seed: int, policy, one_way_ns: int, k: int, ratio: float, fees=op.FEES,
              horizon_s: float = op.HORIZON_S) -> dict:
    """One single-maker run at the operating point, reduced to `legs`. k = 0 is
    the matched no-HFT baseline (same path, investors and scheduler seed)."""
    mk = op.build_market(seed, maker_latencies_ns=(int(one_way_ns),), k=k, snipe_ratio=ratio,
                         fees=fees, policies=[policy], horizon_s=horizon_s)
    return legs(simulate(mk, seed)[0])


def net(row: dict, cls: str, fees=op.FEES) -> float:
    """A class's P&L net of fees under `fees` (accounting.build_ledger's rule), USD."""
    return row[f"{cls}_edge"] - 1e-4 * (fees.maker_bps * row[f"{cls}_passive"]
                                        + fees.taker_bps * row[f"{cls}_aggressive"])


def maker_won(race, maker_ids, sniper_ids):
    """Did a maker's message reach the engine before every sniper IOC of this race?

    None when no sniper fired (no race). A maker's on_signal burst opens with
    its cancels, so its first arrival is its cancel whenever it had a quote.
    """
    for a in race.arrivals:
        if a.participant_id in sniper_ids:
            return False
        if a.participant_id in maker_ids:
            return any(x.participant_id in sniper_ids for x in race.arrivals) or None
    return None


# --- P1's BCS-unit market (H3a/b and H5a/b are stated in these units) -------------

class _EveryUpdate:
    """A path whose every update is news (is_jump), so JumpsOnlyPath keeps and
    reflects all of them: the random-walk twin of P1's jump path."""

    def __init__(self, path):
        self.v0, self.duration_ns = path.v0, path.duration_ns
        self._u = [type(u)(u.t_ns, u.value, True) for u in path.updates()]

    def updates(self):
        return self._u


def rw_sigma(lam_jump: float, b: float, updates_per_s: float) -> float:
    """§5: sigma_u from lam_rw * sigma_u^2 = lam_j * 2 b^2 (matched quadratic variation)."""
    return math.sqrt(lam_jump * 2 * b * b / updates_per_s)


def bcs_run(seed: int, half_spread: float, n_firms: int, path_kind: str = "laplace",
            f_t: float = 0.0, f_m: float = 0.0, duration_s: float = 600.0,
            rw_updates_per_s: float = 100.0, lam_jump: float = 10.0) -> dict:
    """One run of P1's market (exp9: one LP, N - 1 snipers, one-unit Poisson
    investors, 10 us everywhere, b = 1,000 ticks, lam_i = 10/s) on Laplace jumps
    or on the matched random walk, with per-unit fees in ticks: a taker pays
    f_t, the maker f_m (negative: a rebate). Fees are per unit, not bps, because
    P5's closed form is derived for per-unit fees; snipers fire only when the
    stale quote is worth more than f_t. Edges in ticks, rates per second."""
    import exp9_bcs_replication as x9
    from event_sniper import EventSniper
    from fundamental_paths import JumpDiffusionPath

    seeds = x9.derive_seeds(seed)
    dur = int(round(duration_s * 1e9))
    if path_kind == "laplace":
        raw = JumpDiffusionPath(x9.V0, 0.0, dur, dur, seed=seeds["path"],
                                jump_intensity_per_s=lam_jump, jump_scale=x9.SPEC_B)
    elif path_kind == "rw":
        raw = _EveryUpdate(JumpDiffusionPath(
            x9.V0, rw_sigma(lam_jump, x9.SPEC_B, rw_updates_per_s),
            int(round(1e9 / rw_updates_per_s)), dur, seed=seeds["path"]))
    else:
        raise ValueError(f"path_kind must be 'laplace' or 'rw', got {path_kind!r}")
    path = x9.JumpsOnlyPath(raw, x9.REFLECT_TICKS)
    probe = x9.QuoteProbe()
    investor = x9.PoissonInvestor(x9.INVESTOR_ID, x9.SPEC_LAM_INVEST, x9.LATENCY_NS,
                                  seeds["investor"])
    snipers = [EventSniper(x9.FIRST_SNIPER_ID + j, qty=1, threshold=f_t,
                           md_latency_ns=x9.LATENCY_NS, order_latency_ns=x9.LATENCY_NS)
               for j in range(n_firms - 1)]
    agents = [x9.BCSLiquidityProvider(x9.LP_ID, half_spread, x9.V0, x9.LATENCY_NS),
              investor, probe, *snipers]
    h = be.EngineHarness()
    h.add_symbol(1)
    h.start(disable_circuit_breaker=True)
    try:
        res = EventScheduler(h, 1, seed=seeds["ties"], snapshot_every_ns=dur).run(agents, path)
    finally:
        h.stop()
    row = x9.measure(res, probe, investor, {s.participant_id for s in snipers}, duration_s)
    lp_qty = row["sniper_fills"] + row["investor_fills"]   # the LP is every trade's maker
    row.update(
        half_spread=float(half_spread), n_firms=n_firms, path_kind=path_kind, f_t=f_t, f_m=f_m,
        snipes_per_jump=row["n_sniper_trades"] / max(1, row["n_jumps"]),
        sniper_net=row["sniper_edge"] - f_t * row["sniper_fills"],
        lp_net=row["lp_edge"] - f_m * lp_qty,
        sniper_net_rate=(row["sniper_edge"] - f_t * row["sniper_fills"]) / duration_s,
        lp_net_rate=(row["lp_edge"] - f_m * lp_qty) / duration_s)
    return row


def write_report(path: Path, report: dict) -> None:
    import json
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report, indent=1, sort_keys=True, default=_jsonable) + "\n")
    print(f"wrote {path}", flush=True)


def _jsonable(x):
    if isinstance(x, (np.floating, np.integer, np.bool_)):
        return x.item()
    raise TypeError(f"not JSON serialisable: {type(x)}")
