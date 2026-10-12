#!/usr/bin/env python3
"""Experiment 12 — endogenous makers: best response, Nash iteration, exit.

Why this exists. The reviewer's first point, the one that "decides whether
there's a paper": every maker in the paper follows a hand-set rule, so its
headlines (the single-maker inversion, who pays) could be artefacts of the
rule rather than of the market. Here the makers CHOOSE their rule. Each maker
picks a MakerPolicy from a declared discrete space to maximise expected P&L
net of a fixed quoting cost F per second, given its rivals' policies; makers
take turns best-responding until no one has a significant profitable
deviation (PREREGISTRATION.md, P8).

The strategy space (`search_space`, all in engine ticks and lots):
  base half-spread   HALF_SPREADS_TICKS
  adverse widening   WIDENING: (sensitivity, decay) pairs; none, slow, fast
  quote size         QTYS
  post-only          on/off, only when there are rivals (alone, a maker's own
                     cancel always lands before its requote, so nothing can
                     be refused and the dimension would double the cost for
                     nothing)
  no-quote           MakerPolicy(quote=False): payoff exactly 0 and F is not
                     charged, so it is the exit option that gives an entry
                     margin.

Payoff. Each maker's fills are marked at the true V at execution (exp10's
edge_pnl: V is a martingale, so this is the expected value without the
inventory-carry noise of an end-of-run mark), less fees at FEES, less F if it
quotes. Money units are USD: one engine price unit is the 0.10 USD tick on one
BTC, a lot is 0.001 BTC, and P&L divides by PRICE_PRECISION = 1e4.

Best response, and why two stages. Every candidate is run on the same screen
seeds (common random numbers: same path, same noise arrivals), and the top
SHORTLIST by mean go on to fresh confirm seeds together with the incumbent.
Picking the maximum of ~80 noisy means and then testing it on the same seeds
would find "significant" deviations among policies that are truly equal (the
winner's curse); fresh seeds remove that. On the confirm seeds each finalist's
paired difference from the best gets a 95% bootstrap CI (metrics/bootstrap);
finalists whose CI straddles 0 are reported as INDIFFERENT to the best, and
the maker only switches when the incumbent is significantly worse than the
best. ponytail: a candidate the screen cuts is never confirmed; widen
SHORTLIST if screen noise is large next to the gaps between candidates.

Nash iteration. Makers best-respond in turn (Gauss-Seidel); a round with no
switch is an equilibrium in the sense above (no significant profitable
deviation on the confirm seeds). A profile seen before is a cycle, reported
with its members rather than iterated. The same seeds serve every call, so
the iteration solves one sampled game and every profile's runs are cached.

Market: the FACTS lane's tape-calibrated config (facts_event_model.CFG) with
its maker replaced by the M strategic makers: three 5+5 us snipers thresholded
at the 1.7 bp taker fee, the reservation-value noise population at the tape's
arrival rate, the slow informed investor, the calibrated jump-diffusion path.

Status: a PILOT. The operating point is not signed off (PREREGISTRATION §5),
so output goes to results/pilot/ labelled as such; this is not the
confirmatory test of P8.

Run: bcs_research/.venv/bin/python bcs_research/experiments/exp12_endogenous_makers.py [--resume]
"""
from __future__ import annotations

import dataclasses
import json
import os
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
for _d in ("build", "simulation", "agents", "metrics", "analysis", "calibration", "experiments"):
    _p = str(_ROOT / _d)
    if _p not in sys.path:
        sys.path.insert(0, _p)

import numpy as np  # noqa: E402

import bcs_engine as be  # noqa: E402
import facts_event_model as facts  # noqa: E402
from accounting import FeeSchedule, build_ledger  # noqa: E402
from bootstrap import bootstrap_ci  # noqa: E402
from event_scheduler import EventScheduler  # noqa: E402
from event_sniper import EventSniper  # noqa: E402
from exp10_speed_entry import edge_pnl  # noqa: E402
from strategic_maker import MakerPolicy, StrategicMaker  # noqa: E402

NS_PER_S = 1_000_000_000
MAKER, SNIPER, NOISE, INFORMED = facts.MAKER, facts.SNIPER, facts.NOISE, facts.INFORMED
# Binance USD-M VIP 9 (the facts lane's snipers): taker 1.7 bp; maker fee 0
# (the venue's top tier lists 0.0000%; stated, not re-checked here). A maker
# whose plain limit crosses a rival pays the taker fee like anyone else.
FEES = FeeSchedule(maker_bps=0.0, taker_bps=facts.VIP9_TAKER_BP)

HALF_SPREADS_TICKS = (0.5, 1, 2, 4, 8, 16, 32, 64)
WIDENING = ((0.0, 0.05), (1.0, 0.01), (1.0, 0.1))   # (adverse_sensitivity, spread_decay)
QTYS = (250, 1_000)   # lots; the tape touch (6,182) lost 0.3 USD/s alone, at 7x the variance
NO_QUOTE = MakerPolicy(quote=False)
# Where every Nash iteration starts: mid-grid, no widening, post-only with rivals.
START = MakerPolicy(half_spread=4, qty=1_000)

LATENCY_US = 50                    # the facts makers' one-way latency (md = order)
ASYM_LATENCY_US = (50, 150, 450)   # maker k is 3^k times slower
COSTS_USD_PER_S = (0.0, 0.1, 0.3, 1.0)
NO_SNIPERS = {**facts.CFG, "snipers": {**facts.CFG["snipers"], "n": 0}}
M_GRID = (1, 2, 3)

SHORTLIST = 6
SCREEN_SEEDS = tuple(range(100, 102))
CONFIRM_SEEDS = tuple(range(200, 212))
REPORT_SEEDS = tuple(range(300, 316))
HORIZON_S = 150.0
MAX_ROUNDS = 3
STATUS = "pilot — pre-sign-off; not the confirmatory test of P8"
PILOT_OUT = _ROOT / "results" / "pilot" / "exp12_endogenous_makers.json"


# --- the strategy space ------------------------------------------------------

def search_space(n_rivals: int) -> tuple:
    """Every policy a maker may choose, no-quote last. Post-only only with rivals."""
    post = (False, True) if n_rivals else (False,)
    return tuple(MakerPolicy(half_spread=h, qty=q, adverse_sensitivity=s, spread_decay=d,
                             post_only=po)
                 for h in HALF_SPREADS_TICKS for s, d in WIDENING for q in QTYS
                 for po in post) + (NO_QUOTE,)


def describe(p: MakerPolicy) -> dict:
    if not p.quote:
        return {"quote": False}
    return {"quote": True, "half_spread_ticks": p.half_spread, "qty": p.qty,
            "adverse_sensitivity": p.adverse_sensitivity, "spread_decay": p.spread_decay,
            "post_only": p.post_only}


def policy_from(d: dict) -> MakerPolicy:
    """describe()'s inverse, to resume from a checkpointed JSON."""
    if not d["quote"]:
        return NO_QUOTE
    return MakerPolicy(half_spread=d["half_spread_ticks"], qty=d["qty"],
                       adverse_sensitivity=d["adverse_sensitivity"],
                       spread_decay=d["spread_decay"], post_only=d["post_only"])


# --- one run -----------------------------------------------------------------

def simulate(cfg: dict, profile: tuple, latencies_ns: tuple, seed: int, horizon_s: float) -> dict:
    """One run of the facts market with its maker replaced by `profile`.

    Seeds as facts.build_market: path `seed`, noise [seed, 1], informed
    [seed, 2], scheduler `seed`; so every profile meets the same path and the
    same noise arrivals (common random numbers).
    """
    agents, path = facts.build_market(cfg, seed, horizon_s)
    others = [a for a in agents if not isinstance(a, StrategicMaker)]
    makers = [StrategicMaker(MAKER + k, p, path=path, md_latency_ns=lat, order_latency_ns=lat)
              for k, (p, lat) in enumerate(zip(profile, latencies_ns))]
    h = be.EngineHarness()
    h.add_symbol(1)
    h.start(disable_circuit_breaker=True)
    try:
        res = EventScheduler(h, 1, seed=seed, snapshot_every_ns=cfg["snapshot_ms"] * 1_000_000
                             ).run(makers + others, path)
    finally:
        h.stop()
    return summarize(res, makers, horizon_s)


def summarize(res, makers, secs: float) -> dict:
    """Per-second payoffs (before F) and the welfare legs of one run."""
    edge = edge_pnl(res.trades, res.path)
    ledger = build_ledger(res.trades, FEES)
    snipers = [a.participant_id for a in res.agents if isinstance(a, EventSniper)]
    maker_ids = [m.participant_id for m in makers]

    def net(pid):
        acc = ledger.accounts.get(pid)
        return edge.get(pid, 0.0) - (acc.fees if acc else 0.0)

    fills = {pid: 0 for pid in maker_ids}
    maker_on_maker = 0
    for tr in res.trades:
        both = (tr.buyer_id in fills) + (tr.seller_id in fills)
        maker_on_maker += both == 2
        for pid in (tr.buyer_id, tr.seller_id):
            if pid in fills:
                fills[pid] += 1
    two_sided = [(s.best_ask - s.best_bid) / 2 for s in res.snapshots
                 if s.best_bid > 0 and s.best_ask > s.best_bid]
    total = sum(net(pid) for pid in edge) + ledger.exchange_revenue
    return {
        "maker_net": [net(pid) / secs for pid in maker_ids],
        "maker_fills": [fills[pid] for pid in maker_ids],
        "maker_rejects": [m.rejects for m in makers],
        "maker_on_maker_fills_per_s": maker_on_maker / secs,
        "noise_cost": -net(NOISE) / secs,             # spread paid + fees
        "noise_edge_cost": -edge.get(NOISE, 0.0) / secs,   # spread paid only: the toll
        "sniper_net": sum(net(p) for p in snipers) / secs,
        "sniper_gross": sum(edge.get(p, 0.0) for p in snipers) / secs,
        "informed_net": net(INFORMED) / secs,
        "exchange": ledger.exchange_revenue / secs,
        "half_spread_ticks": float(np.mean(two_sided)) if two_sided else None,
        "two_sided_share": len(two_sided) / max(1, len(res.snapshots)),
        "identity_residual": total,
    }


def _task(args):
    return simulate(*args)


class Evaluator:
    """Runs (profile, latencies, seed) cells in a process pool, once each."""

    def __init__(self, cfg: dict, horizon_s: float, pool=None):
        self.cfg, self.horizon_s, self.pool = cfg, horizon_s, pool
        self.cache: dict = {}

    def rows(self, jobs: list, seeds) -> list:
        """jobs: [(profile, latencies_ns)] -> per job, the rows for `seeds` in order."""
        todo = list(dict.fromkeys((p, lat, s) for p, lat in jobs for s in seeds
                                  if (p, lat, s) not in self.cache))
        args = [(self.cfg, p, lat, s, self.horizon_s) for p, lat, s in todo]
        out = self.pool.map(_task, args, chunksize=1) if self.pool else map(_task, args)
        self.cache.update(zip(todo, out))
        return [[self.cache[(p, lat, s)] for s in seeds] for p, lat in jobs]


# --- best response (pure: works on any sampler) -------------------------------

def paired_ci(a, b) -> dict:
    """95% bootstrap CI of mean(a - b), paired by seed."""
    return bootstrap_ci(np.asarray(a, float) - np.asarray(b, float))


def best_response(sample, candidates, incumbent, screen_seeds, confirm_seeds,
                  shortlist=SHORTLIST) -> dict:
    """The best candidate against fixed rivals, and whether it beats the incumbent.

    sample(candidates, seeds) -> one array of per-seed payoffs per candidate,
    on common random numbers. Returns the chosen policy (the incumbent unless
    it is significantly worse than the best), the confirm-stage table and the
    candidates indifferent to the best (paired CI straddling 0).
    """
    screen = sample(list(candidates), screen_seeds)
    order = np.argsort([-np.mean(x) for x in screen], kind="stable")
    finalists = [candidates[i] for i in order[:shortlist]]
    if incumbent not in finalists:
        finalists.append(incumbent)
    confirm = dict(zip(finalists, sample(finalists, confirm_seeds)))
    best = max(finalists, key=lambda c: (np.mean(confirm[c]), c == incumbent))
    table = [{"policy": c, "mean": float(np.mean(confirm[c])),
              "diff_vs_best": paired_ci(confirm[c], confirm[best])} for c in finalists]
    indifferent = [r["policy"] for r in table
                   if r["policy"] != best and r["diff_vs_best"]["upper"] >= 0]
    gap = next(r["diff_vs_best"] for r in table if r["policy"] == incumbent)
    deviates = best != incumbent and gap["upper"] < 0
    return {"policy": best if deviates else incumbent, "best": best, "deviates": deviates,
            "incumbent_gap": gap, "indifferent": indifferent, "table": table,
            "screen_means": {candidates[i]: float(np.mean(screen[i])) for i in order}}


# --- Nash iteration (pure) -------------------------------------------------------

def nash_iteration(br, initial: tuple, max_rounds: int = MAX_ROUNDS) -> dict:
    """Iterated best response, makers in turn, until a round with no switch.

    br(i, profile) -> best_response's dict for maker i. A switch that returns
    to a profile already visited is a cycle: reported, not iterated.
    """
    profile = tuple(initial)
    history, log = [profile], []
    for rnd in range(max_rounds):
        moved = False
        for i in range(len(profile)):
            r = br(i, profile)
            log.append({"round": rnd, "maker": i, "deviates": r["deviates"],
                        "policy": r["policy"], "incumbent_gap": r["incumbent_gap"],
                        "indifferent": r["indifferent"]})
            if not r["deviates"]:
                continue
            moved = True
            profile = profile[:i] + (r["policy"],) + profile[i + 1:]
            if profile in history:
                return {"status": "cycle", "profile": profile, "log": log,
                        "cycle": history[history.index(profile):]}
            history.append(profile)
        if not moved:
            return {"status": "equilibrium", "profile": profile, "log": log, "rounds": rnd + 1}
    return {"status": "max_rounds", "profile": profile, "log": log}


# --- the experiment ---------------------------------------------------------------

def maker_sampler(ev: Evaluator, i: int, profile: tuple, latencies_ns: tuple, cost: float):
    """Maker i's payoffs net of F, rivals fixed; no-quote is 0 without a run."""
    def sample(cands, seeds):
        quoting = [c for c in cands if c.quote]
        jobs = [(profile[:i] + (c,) + profile[i + 1:], latencies_ns) for c in quoting]
        got = dict(zip(quoting, ev.rows(jobs, seeds)))
        return [np.array([r["maker_net"][i] - cost for r in got[c]]) if c.quote
                else np.zeros(len(seeds)) for c in cands]
    return sample


def solve(ev: Evaluator, latencies_ns: tuple, cost: float, initial: tuple,
          screen_seeds=SCREEN_SEEDS, confirm_seeds=CONFIRM_SEEDS) -> dict:
    space = search_space(len(latencies_ns) - 1)

    def br(i, profile):
        return best_response(maker_sampler(ev, i, profile, latencies_ns, cost), space,
                             profile[i], screen_seeds, confirm_seeds)
    return nash_iteration(br, initial)


def report_profile(ev: Evaluator, profile: tuple, latencies_ns: tuple, cost: float,
                   seeds=REPORT_SEEDS) -> dict:
    """The equilibrium on fresh seeds: payoffs, spreads and who bears the tax."""
    rows = ev.rows([(profile, latencies_ns)], seeds)[0]

    def ci(f):
        vals = [f(r) for r in rows]
        vals = [v for v in vals if v is not None]
        return bootstrap_ci(vals) if vals else None

    makers = []
    for k, p in enumerate(profile):
        fills = [r["maker_fills"][k] / max(1, sum(r["maker_fills"])) for r in rows]
        makers.append({"policy": describe(p), "latency_us": latencies_ns[k] / 1_000,
                       "net_after_F": ci(lambda r: r["maker_net"][k] - cost * p.quote),
                       "fill_share": float(np.mean(fills)),
                       "rejects_per_s": float(np.mean([r["maker_rejects"][k] for r in rows]))
                       / ev.horizon_s})
    return {"active_makers": sum(p.quote for p in profile), "makers": makers,
            "half_spread_ticks": ci(lambda r: r["half_spread_ticks"]),
            "two_sided_share": ci(lambda r: r["two_sided_share"]),
            "maker_total_net_before_F": ci(lambda r: sum(r["maker_net"])),
            "noise_cost": ci(lambda r: r["noise_cost"]),
            "noise_edge_cost": ci(lambda r: r["noise_edge_cost"]),
            "sniper_net": ci(lambda r: r["sniper_net"]),
            "informed_net": ci(lambda r: r["informed_net"]),
            "exchange": ci(lambda r: r["exchange"]),
            "maker_on_maker_fills_per_s": ci(lambda r: r["maker_on_maker_fills_per_s"]),
            "max_identity_residual": max(abs(r["identity_residual"]) for r in rows),
            "maker_net_per_seed": [[r["maker_net"][k] for r in rows] for k in range(len(profile))]}


def _jsonable(x):
    if isinstance(x, MakerPolicy):
        return describe(x)
    if isinstance(x, dict):
        return {(json.dumps(describe(k)) if isinstance(k, MakerPolicy) else str(k)): _jsonable(v)
                for k, v in x.items()}
    if isinstance(x, (list, tuple)):
        return [_jsonable(v) for v in x]
    if isinstance(x, (np.floating, np.integer, np.bool_)):
        return x.item()
    return x


def _cell(ev, label, latencies_us, cost, initial, t0):
    lat = tuple(int(u * 1_000) for u in latencies_us)
    nash = solve(ev, lat, cost, initial)
    out = {"label": label, "latencies_us": list(latencies_us), "cost_usd_per_s": cost,
           "nash": nash, "equilibrium": report_profile(ev, nash["profile"], lat, cost),
           "elapsed_s": round(time.time() - t0, 1), "cached_runs": len(ev.cache)}
    print(f"{label}: {nash['status']} -> {[describe(p) for p in nash['profile']]} "
          f"({out['elapsed_s']} s, {len(ev.cache)} runs)", flush=True)
    return out


def main(out_path=PILOT_OUT, workers=None, resume=False) -> dict:
    """Runs every cell, checkpointing the JSON after each. resume=True keeps the
    cells an interrupted run already wrote and runs only the rest (the run
    cache is not saved, so a resumed cell re-simulates what it needs)."""
    t0 = time.time()
    out_path = Path(out_path)
    cells = json.loads(out_path.read_text())["cells"] if resume and out_path.exists() else []
    done = {c["label"]: c for c in cells}
    inversion = {}
    workers = workers or max(1, (os.cpu_count() or 2) - 1)

    def checkpoint():
        report = {"experiment": "exp12_endogenous_makers", "status": STATUS,
                  "config": facts.CFG, "fees": vars(FEES),
                  "space": {"half_spreads_ticks": HALF_SPREADS_TICKS, "widening": WIDENING,
                            "qtys": QTYS, "post_only": "with rivals only", "no_quote": True},
                  "start": describe(START), "horizon_s": HORIZON_S,
                  "seeds": {"screen": SCREEN_SEEDS, "confirm": CONFIRM_SEEDS,
                            "report": REPORT_SEEDS},
                  "shortlist": SHORTLIST, "units": "USD per second",
                  "cells": cells, "elapsed_s": round(time.time() - t0, 1),
                  "single_maker_inversion": {
                      "measure": "M=1 best-response maker net, with minus without snipers, "
                                 "paired by report seed (USD/s)", **inversion}}
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps(_jsonable(report), indent=1) + "\n")

    with ProcessPoolExecutor(max_workers=workers) as pool:
        ev = Evaluator(facts.CFG, HORIZON_S, pool)

        def cell(label, latencies_us, cost, initial, on=ev):
            """The cell's equilibrium profile, run now unless resumed. A resumed
            cell's report is re-run, so every cell's report comes from this code."""
            if label in done:
                profile = tuple(policy_from(d) for d in done[label]["nash"]["profile"])
                lat = tuple(int(u * 1_000) for u in latencies_us)
                done[label]["equilibrium"] = report_profile(on, profile, lat, cost)
            else:
                cells.append(_cell(on, label, latencies_us, cost, initial, t0))
                profile = cells[-1]["nash"]["profile"]
            checkpoint()
            return profile

        sym = {}
        for m in M_GRID:   # symmetric latencies, F = 0: spreads against M
            start = dataclasses.replace(START, post_only=m > 1)
            sym[m] = cell(f"sym_M{m}_F0", (LATENCY_US,) * m, 0.0, (start,) * m)
        for cost in COSTS_USD_PER_S[1:]:   # exit: three potential makers, F > 0
            cell(f"sym_M3_F{cost}", (LATENCY_US,) * 3, cost, sym[3])
        for m in (2, 3):   # asymmetric latencies, F = 0
            cell(f"asym_M{m}_F0", ASYM_LATENCY_US[:m], 0.0, sym[m])
        # The single-maker inversion with a maker that re-optimises: its best
        # response without snipers, paired by report seed against sym_M1_F0.
        alone = Evaluator(NO_SNIPERS, HORIZON_S, pool)
        cell("sym_M1_F0_no_snipers", (LATENCY_US,), 0.0, (START,), on=alone)
        by = {c["label"]: c["equilibrium"]["maker_net_per_seed"][0] for c in cells}
        inversion.update(paired_ci(by["sym_M1_F0"], by["sym_M1_F0_no_snipers"]))
        checkpoint()
    print(f"wrote {out_path} in {time.time() - t0:.0f} s")
    return {"cells": cells}


# --- confirmatory mode: P8 at the operating point ---------------------------------
#
# The pilot's strategy space, solver and checks, moved onto §5's operating
# point (operating_point.build_market: 100 us snipers with 80 us jitter, the
# investors, VIP 9 fees; no informed investor, §5 lists none). P8 states M and
# little else, so the base cell is k = 3 (the operating point's) with one snipe
# clearing the quote (ratio 1: BCS's N = k + 1 breakeven H8a compares with
# assumes it), makers at 50 us each way (lag 0) or the pilot's 50/150/450 us.
# Solver: every run lasts §5's 30 minutes. One screen seed (30 simulated
# minutes per candidate, six times the pilot's screen) and six confirm seeds
# per finalist; the equilibrium is reported on 100 fresh seeds (§2's n).
# MAX_ROUNDS 8; a cycle or the round cap is reported as non-convergence.
# WIDENING's decays (0.01, 0.05, 0.1) are per OBSERVED UPDATE, i.e. per 10 ms,
# by StrategicMaker's definition: they are strategy-grid points the makers
# choose among, not the tick model's per-tick rate, so no tick conversion
# applies (the paper's 0.1 per 100-ms tick would be 0.0105 per update, inside
# the grid's span).
# Every run is checkpointed, and the solver is deterministic given its runs,
# so a restart replays the finished solves from the checkpoint without
# simulating.
#   H8a  M in {1, 2, 3, 5}, F = 0: the realised quoted half-spread on the report
#        seeds, paired across M; each step M-1 -> M HIT if the paired interval's
#        upper bound is <= 0, MISS if its lower bound is > 0. M = 5 inside +-20%
#        of half_spread(lambda_i, lambda_j (N-1)/N, J), N = k + 1. M = 1's best
#        response on the half-spread grid's upper edge (not in P8's miss line:
#        off the edge is INCONCLUSIVE).
#   H8b  three potential makers; F in {0, 0.1, 0.3, 1.0} USD/s at k = 3 and k in
#        {1, 3, 8} at F = 0.1 (P8 states no grids: §5 defaults to the pilot's
#        costs; the k grid is P2's {1, 3, 8} minus 21). Active makers must not
#        rise in F or in k; each active maker's gross P&L interval must meet
#        [F, F + increment], the increment being the fall in mean active-maker
#        P&L when one more maker quotes (the first active maker's policy).
#   H8c  the asymmetric equilibria (M = 2: 50/150 us; M = 3: 50/150/450 us) held
#        fixed and evaluated in every tape-band cell, both bands (§4): per cell
#        the slowest maker is "lowest" if every paired interval of its delta
#        minus a rival's lies below 0, "not lowest" if any lies above 0. HIT if
#        >= 90% of cells are lowest, MISS if > 10% are not lowest.
#   H8d  ratio-1 cells with k in {3, 5, 6, 7, 8, 13, 21}: the M = 2 asymmetric game
#        (the 150 us maker lags every sniper by 200 us > eps) solved per k; the
#        slow maker counts if it chooses no-quote or takes < 5% of maker fills.
#        HIT if >= 2/3 of cells count, MISS if < 1/2.

CONFIRM_SCREEN, CONFIRM_CONFIRM = (100,), tuple(range(200, 206))
CONFIRM_REPORT = tuple(range(300, 400))
CONFIRM_MAX_ROUNDS = 8
CONFIRM_K, CONFIRM_RATIO = 3, 1.0
M_CONFIRM = (1, 2, 3, 5)
H8B_COSTS, H8B_KS, H8B_K_COST = (0.1, 0.3, 1.0), (1, 8), 0.1
H8D_KS = (3, 5, 6, 7, 8, 13, 21)
SYM_US, ASYM_US = 50, (50, 150, 450)


def simulate_op(market: tuple, profile: tuple, latencies_ns: tuple, seed: int,
                horizon_s: float) -> dict:
    """One operating-point run; market = (k, snipe ratio)."""
    import operating_point as op
    k, ratio = market
    mk = op.build_market(seed, maker_latencies_ns=latencies_ns, k=k, snipe_ratio=ratio,
                         policies=list(profile), horizon_s=horizon_s)
    res = op.simulate(seed, market=mk)
    return summarize(res, mk.makers, horizon_s)


class OpEvaluator:
    """Evaluator's interface on the operating point, every run checkpointed."""

    def __init__(self, market: tuple, horizon_s: float, log, pool=None):
        self.market, self.horizon_s, self.log, self.pool = market, horizon_s, log, pool
        self.cache = log.done

    def key(self, profile, lat, seed) -> str:
        return json.dumps([list(self.market), [describe(p) for p in profile], list(lat), seed,
                           self.horizon_s])

    def rows(self, jobs: list, seeds) -> list:
        from concurrent.futures import as_completed
        want = {self.key(p, lat, s): (p, lat, s) for p, lat in jobs for s in seeds}
        todo = {k: v for k, v in want.items() if k not in self.log}
        if self.pool is None:
            for k, (p, lat, s) in todo.items():
                self.log.put(k, simulate_op(self.market, p, lat, s, self.horizon_s))
        else:
            futs = {self.pool.submit(simulate_op, self.market, p, lat, s, self.horizon_s): k
                    for k, (p, lat, s) in todo.items()}
            for f in as_completed(futs):
                self.log.put(futs[f], f.result())
        return [[self.log.get(self.key(p, lat, s)) for s in seeds] for p, lat in jobs]


def solve_op(ev, latencies_ns, cost, initial) -> dict:
    space = search_space(len(latencies_ns) - 1)

    def br(i, profile):
        return best_response(maker_sampler(ev, i, profile, latencies_ns, cost), space,
                             profile[i], CONFIRM_SCREEN, CONFIRM_CONFIRM)
    return nash_iteration(br, initial, CONFIRM_MAX_ROUNDS)


def _lat(us) -> tuple:
    return tuple(int(u * 1_000) for u in us)


def _step(diff: dict) -> str:
    return "HIT" if diff["upper"] <= 0 else ("MISS" if diff["lower"] > 0 else "INCONCLUSIVE")


def h8a(sym: dict, k: int) -> dict:
    """sym: {M: (nash dict, report rows)}; rows carry half_spread_ticks per report seed."""
    import operating_point as op
    from bcs_theory import half_spread, laplace_jump
    spreads = {m: [r["half_spread_ticks"] for r in rows] for m, (_, rows) in sym.items()}
    steps = []
    ms = sorted(spreads)
    for a, b in zip(ms, ms[1:]):
        pairs = [(y - x) for x, y in zip(spreads[a], spreads[b]) if x is not None and y is not None]
        d = bootstrap_ci(pairs) if pairs else None
        steps.append({"from_M": a, "to_M": b, "diff": d, "verdict": _step(d) if d else "INCONCLUSIVE"})
    lam_i, lam_j, b = op.INVESTOR_RATE_PER_S, op.PATH["jump_intensity_per_s"], op.PATH["jump_scale_bp"]
    n = k + 1
    breakeven = half_spread(lam_i, lam_j * (n - 1) / n, laplace_jump(b)) / op.TICK_BP
    top = max(ms)
    vals = [v for v in spreads[top] if v is not None]
    m5 = bootstrap_ci(vals) if vals else None
    m5v = op.band_verdict(m5["lower"], m5["upper"], 0.8 * breakeven, 1.2 * breakeven) if m5 else "MISS"
    m1 = sym[min(ms)][0]["profile"][0]
    edge = m1.quote and m1.half_spread == max(HALF_SPREADS_TICKS)
    parts = [s["verdict"] for s in steps] + [m5v]
    verdict = ("MISS" if "MISS" in parts else
               "HIT" if all(v == "HIT" for v in parts) and edge else "INCONCLUSIVE")
    return {"verdict": verdict, "steps": steps, "top_M": top, "top_M_half_spread_ticks": m5,
            "breakeven_ticks": breakeven, "band": [0.8 * breakeven, 1.2 * breakeven],
            "top_M_verdict": m5v, "M1_policy": describe(m1), "M1_on_upper_edge": bool(edge),
            "half_spread_grid_max": max(HALF_SPREADS_TICKS)}


def _active(profile) -> list:
    return [i for i, p in enumerate(profile) if p.quote]


def h8b_cell(ev, profile, lat, cost) -> dict:
    """Active count and the free-entry band for one solved cell."""
    act = _active(profile)
    rows = ev.rows([(profile, lat)], CONFIRM_REPORT)[0]
    if not act:
        return {"active": 0, "makers": [], "verdict": "HIT"}
    entrant = list(profile)
    idle = [i for i, p in enumerate(profile) if not p.quote]
    elat = list(lat)
    if idle:
        entrant[idle[0]] = profile[act[0]]
    else:
        entrant.append(profile[act[0]])
        elat.append(lat[act[0]])
    erows = ev.rows([(tuple(entrant), tuple(elat))], CONFIRM_REPORT)[0]
    eact = _active(entrant)
    gross = float(np.mean([np.mean([r["maker_net"][i] for i in act]) for r in rows]))
    gross_e = float(np.mean([np.mean([r["maker_net"][i] for i in eact]) for r in erows]))
    inc = max(0.0, gross - gross_e)
    makers = []
    for i in act:
        ci = bootstrap_ci([r["maker_net"][i] for r in rows])
        meets = ci["upper"] >= cost and ci["lower"] <= cost + inc
        makers.append({"maker": i, "gross_pnl": ci, "meets_band": meets})
    return {"active": len(act), "increment": inc, "band": [cost, cost + inc], "makers": makers,
            "verdict": "HIT" if all(m["meets_band"] for m in makers) else "MISS"}


def _slowest_lowest(rows_on, rows_off, lat) -> str:
    slow = int(np.argmax(lat))
    d = [[a - b for a, b in zip(ron["maker_net"], roff["maker_net"])]
         for ron, roff in zip(rows_on, rows_off)]
    d = np.array(d)
    cis = [bootstrap_ci(d[:, slow] - d[:, j]) for j in range(len(lat)) if j != slow]
    if any(c["lower"] > 0 for c in cis):
        return "not_lowest"
    return "lowest" if all(c["upper"] < 0 for c in cis) else "undetermined"


def confirmatory(argv) -> dict:
    import argparse
    from concurrent.futures import ProcessPoolExecutor
    import operating_point as op
    global CONFIRM_REPORT, CONFIRM_CONFIRM
    ap = argparse.ArgumentParser(description="exp12 confirmatory (P8)")
    ap.add_argument("--confirmatory", action="store_true")
    ap.add_argument("--horizon-s", type=float, default=op.HORIZON_S)
    ap.add_argument("--report-seeds", type=int, default=len(CONFIRM_REPORT), help="smoke only")
    ap.add_argument("--confirm-seeds", type=int, default=len(CONFIRM_CONFIRM), help="smoke only")
    ap.add_argument("--m-grid", type=int, nargs="+", default=list(M_CONFIRM), help="smoke only")
    ap.add_argument("--h8d-ks", type=int, nargs="+", default=list(H8D_KS), help="smoke only")
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--out-dir", type=Path, default=None)
    a = ap.parse_args(argv)
    CONFIRM_REPORT = tuple(range(300, 300 + a.report_seeds))
    CONFIRM_CONFIRM = tuple(range(200, 200 + a.confirm_seeds))
    out, ck = op.confirmatory_paths("exp12_endogenous_makers", a.out_dir)
    log = op.CellLog(ck)
    t0 = time.time()
    base = (CONFIRM_K, CONFIRM_RATIO)
    cells, notes = {}, []

    def note(label, nash):
        cells[label] = {"status": nash["status"], "profile": [describe(p) for p in nash["profile"]],
                        "log": _jsonable(nash["log"])}
        if nash["status"] != "equilibrium":
            notes.append(f"{label}: {nash['status']} (non-convergence)")
        print(f"[exp12] {label}: {nash['status']} ({time.time() - t0:.0f} s)", flush=True)

    pool = ProcessPoolExecutor(max_workers=a.workers) if a.workers > 1 else None
    try:
        evs = {}

        def ev(market):
            if market not in evs:
                evs[market] = OpEvaluator(market, a.horizon_s, log, pool)
            return evs[market]

        sym = {}
        for m in a.m_grid:
            start = dataclasses.replace(START, post_only=m > 1)
            nash = solve_op(ev(base), _lat((SYM_US,) * m), 0.0, (start,) * m)
            note(f"sym_M{m}_F0", nash)
            sym[m] = (nash, ev(base).rows([(nash["profile"], _lat((SYM_US,) * m))], CONFIRM_REPORT)[0])
        res = {"H8a": h8a(sym, CONFIRM_K)}

        m3 = sym[3][0]["profile"] if 3 in sym else (dataclasses.replace(START, post_only=True),) * 3
        lat3 = _lat((SYM_US,) * 3)
        h8b_cells = {(0.0, CONFIRM_K): h8b_cell(ev(base), m3, lat3, 0.0)}
        for c in H8B_COSTS:
            nash = solve_op(ev(base), lat3, c, m3)
            note(f"sym_M3_F{c}_k{CONFIRM_K}", nash)
            h8b_cells[(c, CONFIRM_K)] = h8b_cell(ev(base), nash["profile"], lat3, c)
            if c == H8B_K_COST:
                start_k = nash["profile"]
        for kk in H8B_KS:
            nash = solve_op(ev((kk, CONFIRM_RATIO)), lat3, H8B_K_COST, start_k)
            note(f"sym_M3_F{H8B_K_COST}_k{kk}", nash)
            h8b_cells[(H8B_K_COST, kk)] = h8b_cell(ev((kk, CONFIRM_RATIO)), nash["profile"], lat3,
                                                   H8B_K_COST)
        by_f = [h8b_cells[(c, CONFIRM_K)]["active"] for c in (0.0,) + H8B_COSTS]
        by_k = [h8b_cells[(H8B_K_COST, kk)]["active"] for kk in sorted(H8B_KS + (CONFIRM_K,))]
        mono = all(x >= y for x, y in zip(by_f, by_f[1:])) and all(x >= y for x, y in zip(by_k, by_k[1:]))
        res["H8b"] = {"active_by_cost": dict(zip(map(str, (0.0,) + H8B_COSTS), by_f)),
                      "active_by_k": dict(zip(map(str, sorted(H8B_KS + (CONFIRM_K,))), by_k)),
                      "cells": {f"F{c}_k{kk}": v for (c, kk), v in h8b_cells.items()},
                      "verdict": op.all_hit(["HIT" if mono else "MISS"]
                                            + [v["verdict"] for (c, _), v in h8b_cells.items() if c > 0])}

        asym = {}
        for m in (2, 3):
            lat = _lat(ASYM_US[:m])
            init = sym[m][0]["profile"] if m in sym else (dataclasses.replace(START, post_only=True),) * m
            nash = solve_op(ev(base), lat, 0.0, init)
            note(f"asym_M{m}_F0_k{CONFIRM_K}", nash)
            asym[m] = (nash["profile"], lat)
        h8c = {}
        for band, ratios in op.BANDS.items():
            labels = []
            for m, (profile, lat) in asym.items():
                off = ev((0, 1.0)).rows([(profile, lat)], CONFIRM_REPORT)[0]
                for r in ratios:
                    for kk in op.K_GRID:
                        on = ev((kk, r)).rows([(profile, lat)], CONFIRM_REPORT)[0]
                        labels.append({"M": m, "ratio": r, "k": kk,
                                       "slowest": _slowest_lowest(on, off, lat)})
            n = len(labels)
            low = sum(c["slowest"] == "lowest" for c in labels)
            nlow = sum(c["slowest"] == "not_lowest" for c in labels)
            v = "HIT" if low >= 0.9 * n else ("MISS" if nlow > 0.1 * n else "INCONCLUSIVE")
            h8c[band] = {"verdict": v, "lowest": low, "not_lowest": nlow, "cells": labels}
        res["H8c"] = h8c | {"verdict": h8c["paper"]["verdict"], "read_on": "paper band (§4)"}

        h8d = []
        for kk in a.h8d_ks:
            lat = _lat(ASYM_US[:2])
            if kk == CONFIRM_K:
                profile = asym[2][0]
            else:
                nash = solve_op(ev((kk, CONFIRM_RATIO)), lat, 0.0, asym[2][0])
                note(f"asym_M2_F0_k{kk}", nash)
                profile = nash["profile"]
            rows = ev((kk, CONFIRM_RATIO)).rows([(profile, lat)], CONFIRM_REPORT)[0]
            share = float(np.mean([r["maker_fills"][1] / max(1, sum(r["maker_fills"])) for r in rows]))
            counts = (not profile[1].quote) or share < 0.05
            h8d.append({"k": kk, "slow_policy": describe(profile[1]), "slow_fill_share": share,
                        "counts": bool(counts)})
        yes = sum(c["counts"] for c in h8d)
        res["H8d"] = {"cells": h8d, "counting": yes, "n": len(h8d),
                      "verdict": "HIT" if yes >= 2 / 3 * len(h8d) else
                      ("MISS" if yes < len(h8d) / 2 else "INCONCLUSIVE")}
    finally:
        if pool:
            pool.shutdown()
    status = (op.CONFIRMATORY_STATUS if a.horizon_s >= op.HORIZON_S and a.report_seeds >= op.N_SEEDS
              and tuple(a.m_grid) == M_CONFIRM and tuple(a.h8d_ks) == H8D_KS
              and a.confirm_seeds >= 6 else "smoke — NOT confirmatory: reduced sizes or grids")
    report = {"experiment": "exp12_endogenous_makers (P8), confirmatory", "status": status,
              "market": {"k": CONFIRM_K, "snipe_ratio": CONFIRM_RATIO, "sym_us": SYM_US,
                         "asym_us": ASYM_US}, "fees": vars(FEES), "horizon_s": a.horizon_s,
              "seeds": {"screen": CONFIRM_SCREEN, "confirm": CONFIRM_CONFIRM,
                        "report": CONFIRM_REPORT}, "max_rounds": CONFIRM_MAX_ROUNDS,
              "space": {"half_spreads_ticks": HALF_SPREADS_TICKS, "widening": WIDENING,
                        "qtys": QTYS, "post_only": "with rivals only", "no_quote": True},
              "p8": res, "non_convergence": notes, "cells": cells,
              "holm": "95% verdicts; Holm across the 11 primaries is applied at write-up",
              "elapsed_s": round(time.time() - t0, 1), "units": "USD per second"}
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(_jsonable(report), indent=1, default=str) + "\n")
    print(f"[exp12] H8a {res['H8a']['verdict']}, H8b {res['H8b']['verdict']}, H8c "
          f"{res['H8c']['verdict']}, H8d {res['H8d']['verdict']} -> {out}", flush=True)
    return report


if __name__ == "__main__":
    if "--confirmatory" in sys.argv[1:]:
        confirmatory(sys.argv[1:])
    else:
        main(resume="--resume" in sys.argv[1:])
