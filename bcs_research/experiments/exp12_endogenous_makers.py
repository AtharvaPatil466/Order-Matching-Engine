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
        "noise_cost": -net(NOISE) / secs,
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
            "sniper_net": ci(lambda r: r["sniper_net"]),
            "informed_net": ci(lambda r: r["informed_net"]),
            "exchange": ci(lambda r: r["exchange"]),
            "maker_on_maker_fills_per_s": ci(lambda r: r["maker_on_maker_fills_per_s"]),
            "max_identity_residual": max(abs(r["identity_residual"]) for r in rows)}


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
                  "cells": cells, "elapsed_s": round(time.time() - t0, 1)}
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps(_jsonable(report), indent=1) + "\n")

    with ProcessPoolExecutor(max_workers=workers) as pool:
        ev = Evaluator(facts.CFG, HORIZON_S, pool)

        def cell(label, latencies_us, cost, initial):
            """The cell's equilibrium profile, run now unless resumed."""
            if label in done:
                return tuple(policy_from(d) for d in done[label]["nash"]["profile"])
            cells.append(_cell(ev, label, latencies_us, cost, initial, t0))
            checkpoint()
            return cells[-1]["nash"]["profile"]

        sym = {}
        for m in M_GRID:   # symmetric latencies, F = 0: spreads against M
            start = dataclasses.replace(START, post_only=m > 1)
            sym[m] = cell(f"sym_M{m}_F0", (LATENCY_US,) * m, 0.0, (start,) * m)
        for cost in COSTS_USD_PER_S[1:]:   # exit: three potential makers, F > 0
            cell(f"sym_M3_F{cost}", (LATENCY_US,) * 3, cost, sym[3])
        for m in (2, 3):   # asymmetric latencies, F = 0
            cell(f"asym_M{m}_F0", ASYM_LATENCY_US[:m], 0.0, sym[m])
    print(f"wrote {out_path} in {time.time() - t0:.0f} s")
    return {"cells": cells}


if __name__ == "__main__":
    main(resume="--resume" in sys.argv[1:])
