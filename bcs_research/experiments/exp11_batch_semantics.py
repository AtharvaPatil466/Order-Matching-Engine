#!/usr/bin/env python3
"""Experiment 11 — batch-auction semantics on the event clock (PILOT).

Research question: in the nanosecond event model, does a frequent batch
auction with BCS semantics (simulation/event_batch.py) eliminate sniping, as
BCS predict, and does the tick model's interval-1 backfire reappear at short
intervals? The grid crosses the clearing rule with the two batch choices
the tick model left implicit: within-batch rationing (time vs random) and
the clearing-price rule (the engine's reference-first cascade vs pressure,
BCS's Bertrand outcome). Both sniper kinds run, because the price rule can
only matter when snipers bid above the stale quote (PREREGISTRATION P9 H9b):
"stale" snipers send an IOC at the stale quote (EventSniper); "competitive"
ones bid their value less one tick.

Every arm of one seed shares the path, the noise draws and the scheduler
seed, so arm differences are matched. Fees are two accounting views of the
same run (agents do not condition on fees). The operating point is a
placeholder (CFG) and has not been signed off, so this is a pilot, not a
test of P9: results go to results/pilot/.

`tick_interval1_probe` holds the targeted tick-model runs behind
paper/batch_interval1_note.md; it writes results/experiments/exp11_interval1_probe.json.

Run: bcs_research/.venv/bin/python bcs_research/experiments/exp11_batch_semantics.py [--seeds N]
     ... exp11_batch_semantics.py --tick-probe [--seeds N]
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import math
import sys
import time
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_ROOT = _HERE.parent
for _d in ("build", "agents", "simulation", "metrics", "experiments", "analysis"):
    _p = str(_ROOT / _d)
    if _p not in sys.path:
        sys.path.insert(0, _p)

import bcs_engine as be                                       # noqa: E402
from accounting import TAKER_HEAVY, ZERO, build_ledger, decompose  # noqa: E402
from bcs_theory import fba_slow_lp_exposure, laplace_half_spread   # noqa: E402
from bootstrap import bootstrap_ci, bootstrap_ci_table           # noqa: E402
from event_batch import EventBatchScheduler                      # noqa: E402
from event_maker import EventMaker                               # noqa: E402
from event_noise import ReservationNoiseTrader                   # noqa: E402
from event_scheduler import EventScheduler                       # noqa: E402
from event_sniper import EventSniper                             # noqa: E402
from fundamental_paths import JumpDiffusionPath                  # noqa: E402

STATUS = "pilot — pre-sign-off, placeholder parameters; not a test of P9"
NS_PER_S = 1_000_000_000
MAKER, SNIPER0, NOISE0 = 1, 2, 100

# Placeholder operating point (one dict, per the brief). Rates per second,
# prices in engine units (PRICE_PRECISION 10000, so b = 1000 is $0.10 on $100),
# latencies in ns. Jump rates and b follow the replication spec's grid
# (lam_jump/lam_invest = 0.25, b = 1000); the half-spread is BCS's equilibrium
# h* = b W(lam_jump/lam_invest) for the continuous market.
CFG = dict(
    v0=100 * be.PRICE_PRECISION,
    duration_s=60,
    lam_invest=10.0, lam_jump=2.5, jump_b=1000.0,
    update_interval_ms=10,             # pure-jump signal; each grid update re-posts the
                                       # maker, refilling a side an investor emptied
    qty=1,                             # BCS unit orders for maker, snipers and investors
    n_snipers=3, sniper_latency_ns=5_000, maker_latency_ns=50_000,   # each leg: md and order
    jitter_ns=2_000,                   # U{0..jitter} per message, every agent
    n_noise=16,                        # one investor population split so its own buy and
                                       # sell can meet in a batch (engine STP is per participant)
    noise_eps_mult=2.0,                # reservation-value shock sd, in units of b
    snapshot_every_ns=1_000_000,
)
INTERVALS_NS = [100_000, 1_000_000, 10_000_000, 100_000_000, 1_000_000_000]
RATIONINGS = ("time", "random")
PRICE_RULES = ("engine", "pressure")
SNIPER_KINDS = ("stale", "competitive")
FEES = {"zero": ZERO, "taker_heavy": TAKER_HEAVY}
CONTINUOUS = "continuous"


class CompetitiveSniper(EventSniper):
    """Bids its value less one tick, not the stale quote (P9 H9b's price-competitive sniper)."""

    def on_signal(self, t_ns, update, view):
        out = super().on_signal(t_ns, update, view)
        if not out:
            return out
        o = out[0]
        price = math.ceil(update.value) - 1 if o.side == be.Side.Buy else math.floor(update.value) + 1
        return [dataclasses.replace(o, price=int(price))]


def half_spread(cfg):
    return round(laplace_half_spread(cfg["lam_invest"], cfg["lam_jump"], cfg["jump_b"]))


def make_path(cfg, seed):
    return JumpDiffusionPath(v0=cfg["v0"], sigma_step=0.0,
                             update_interval_ns=cfg["update_interval_ms"] * 1_000_000,
                             duration_ns=cfg["duration_s"] * NS_PER_S, seed=seed,
                             jump_intensity_per_s=cfg["lam_jump"], jump_scale=cfg["jump_b"])


def default_maker(cfg, h):
    lat = cfg["maker_latency_ns"]
    return EventMaker(MAKER, h, cfg["qty"], md_latency_ns=lat, order_latency_ns=lat)


def make_agents(cfg, seed, sniper_kind, maker_factory=default_maker):
    """maker_factory(cfg, h) -> EventAgent with participant_id MAKER; the slot an
    endogenous maker drops into."""
    h = half_spread(cfg)
    lat = cfg["sniper_latency_ns"]
    sniper_cls = CompetitiveSniper if sniper_kind == "competitive" else EventSniper
    snipers = [sniper_cls(SNIPER0 + i, cfg["qty"], md_latency_ns=lat, order_latency_ns=lat)
               for i in range(cfg["n_snipers"])]
    n = cfg["n_noise"]
    noise = [ReservationNoiseTrader(NOISE0 + i, cfg["lam_invest"] / n,
                                    cfg["noise_eps_mult"] * cfg["jump_b"], cfg["v0"],
                                    qty=cfg["qty"], seed=seed * 1000 + i)
             for i in range(n)]
    return [maker_factory(cfg, h)] + snipers + noise


def classes(agents):
    def cls(pid):
        if pid == MAKER:
            return "maker"
        return "sniper" if pid < NOISE0 else "noise"
    return {a.participant_id: cls(a.participant_id) for a in agents}


def run_once(cfg, seed, arm, sniper_kind, maker_factory=default_maker):
    """One run; arm is CONTINUOUS or (interval_ns, rationing, price_rule)."""
    agents = make_agents(cfg, seed, sniper_kind, maker_factory)
    path = make_path(cfg, seed)
    h = be.EngineHarness()
    h.add_symbol(1)
    h.start(disable_circuit_breaker=True)
    jitter = {a.participant_id: cfg["jitter_ns"] for a in agents}
    common = dict(seed=seed, snapshot_every_ns=cfg["snapshot_every_ns"], race_jitter_ns=jitter)
    if arm == CONTINUOUS:
        sched = EventScheduler(h, 1, **common)
    else:
        tau, rationing, rule = arm
        sched = EventBatchScheduler(h, 1, tau, rationing=rationing, price_rule=rule, **common)
    res = sched.run(agents, path)
    row = measure(res, classes(agents))
    row["repriced"] = getattr(sched, "repriced", 0)
    row["reprice_refused"] = getattr(sched, "reprice_refused", 0)
    h.stop()
    return row


def edges(res, cls):
    """Per-class trading edge against the true V in effect at each fill.

    V is a martingale, so this has the P&L's expectation without the
    terminal-inventory mark's variance; it is the primary welfare measure.
    """
    out = dict.fromkeys(("maker", "sniper", "noise"), 0.0)
    for tr in res.trades:
        e = (res.path.value_at(tr.t_ns) - tr.price) * tr.quantity / be.PRICE_PRECISION
        out[cls[tr.buyer_id]] += e
        out[cls[tr.seller_id]] -= e
    return out


def measure(res, cls):
    mark_v = res.path.value_at(res.duration_ns - 1)
    edge = edges(res, cls)
    row = {}
    for name, fees in FEES.items():
        w = decompose(build_ledger(res.trades, fees), cls, res.last_mid, mark_v)
        by = w["by_class"]
        row[f"maker_edge_{name}"] = edge["maker"] - by["maker"]["fees"]
        row[f"sniper_edge_{name}"] = edge["sniper"] - by["sniper"]["fees"]
        row[f"noise_cost_{name}"] = by["noise"]["fees"] - edge["noise"]
        row[f"maker_pnl_{name}"] = by["maker"]["net_v"]
        row[f"sniper_pnl_{name}"] = by["sniper"]["net_v"]
        row[f"exchange_{name}"] = w["exchange"]["revenue"]
        row[f"residual_{name}"] = w["transfer_residual_v"]
    vol = {}
    for tr in res.trades:
        key = "_".join(sorted((cls[tr.buyer_id], cls[tr.seller_id])))
        vol[key] = vol.get(key, 0) + tr.quantity
    row["vol_maker_sniper"] = vol.get("maker_sniper", 0)
    row["vol_noise_sniper"] = vol.get("noise_sniper", 0)
    row["vol_maker_noise"] = vol.get("maker_noise", 0)
    row["vol_noise_noise"] = vol.get("noise_noise", 0)
    sniped = [r for r in res.races
              if any(cls.get(a.participant_id) == "sniper" and a.outcome in ("filled", "partial")
                     for a in r.arrivals)]
    row["n_jumps"] = len(res.races)
    row["race_sniper_fill_share"] = len(sniped) / len(res.races) if res.races else 0.0
    row["maker_sniper_vol_per_jump"] = row["vol_maker_sniper"] / max(1, len(res.races))
    one_sided = sum(1 for s in res.snapshots if not (s.best_bid > 0 and s.best_ask > 0))
    row["one_sided_share"] = one_sided / len(res.snapshots)
    row["n_trades"] = len(res.trades)
    return row


def arms():
    return [CONTINUOUS] + [(tau, r, p) for tau in INTERVALS_NS for r in RATIONINGS for p in PRICE_RULES]


def arm_label(arm):
    return arm if arm == CONTINUOUS else f"tau={arm[0]}ns/{arm[1]}/{arm[2]}"


PAIRED = ("maker_edge_zero", "sniper_edge_zero", "noise_cost_zero", "maker_edge_taker_heavy",
          "maker_pnl_zero", "maker_sniper_vol_per_jump")


def summarize(cfg, rows_by_cell, seeds, boot_seed=0):
    keys = sorted(next(iter(rows_by_cell.values()))[0])
    cells = []
    for (kind, label), rows in rows_by_cell.items():
        cont = rows_by_cell[(kind, CONTINUOUS)]
        paired = {k: bootstrap_ci([r[k] - c[k] for r, c in zip(rows, cont)], seed=boot_seed)
                  for k in PAIRED}
        cells.append({"sniper_kind": kind, "arm": label,
                      "ci": bootstrap_ci_table(rows, keys, seed=boot_seed),
                      "delta_vs_continuous": paired, "rows": rows})
    lag = 2 * (cfg["maker_latency_ns"] - cfg["sniper_latency_ns"])
    theory = {"maker_lag_ns": lag, "half_spread": half_spread(cfg),
              "slow_lp_exposure_by_tau": {str(t): fba_slow_lp_exposure(lag, t) for t in INTERVALS_NS}}
    return {"status": STATUS, "config": cfg, "seeds": list(seeds), "intervals_ns": INTERVALS_NS,
            "rationings": RATIONINGS, "price_rules": PRICE_RULES, "sniper_kinds": SNIPER_KINDS,
            "theory": theory, "cells": cells}


def run_grid(cfg, n_seeds, log=print):
    seeds = range(1, n_seeds + 1)
    rows_by_cell = {}
    t0 = time.time()
    for kind in SNIPER_KINDS:
        for arm in arms():
            rows_by_cell[(kind, arm_label(arm))] = [run_once(cfg, s, arm, kind) for s in seeds]
            log(f"[{time.time() - t0:7.1f}s] {kind:11s} {arm_label(arm)}")
    return summarize(cfg, rows_by_cell, seeds)


def print_summary(report):
    print(f"Experiment 11 ({report['status']}), seeds={len(report['seeds'])}")
    print(f"{'sniper':12s}{'arm':38s}{'maker':>18s}{'dMaker':>18s}{'sniper':>10s}{'noise':>10s}"
          f"{'fill%':>8s}{'1side':>8s}")
    for c in report["cells"]:
        ci, d = c["ci"], c["delta_vs_continuous"]["maker_edge_zero"]
        m = ci["maker_edge_zero"]
        print(f"{c['sniper_kind']:12s}{c['arm']:38s}"
              f"{m['mean']:7.3f}[{m['lower']:.2f},{m['upper']:.2f}]"
              f"{d['mean']:7.3f}[{d['lower']:.2f},{d['upper']:.2f}]"
              f"{ci['sniper_edge_zero']['mean']:10.3f}{ci['noise_cost_zero']['mean']:10.3f}"
              f"{100 * ci['race_sniper_fill_share']['mean']:8.1f}{ci['one_sided_share']['mean']:8.3f}")


# --- the tick-model probe behind paper/batch_interval1_note.md ---------------

class _MidTickHarness:
    """Harness proxy noting whether an IOC left the book one-sided mid-tick."""

    def __init__(self, h):
        self._h = h
        self.hit = False

    def __getattr__(self, name):
        return getattr(self._h, name)

    def submit_order(self, sym, *args):
        out = self._h.submit_order(sym, *args)
        if args[5] == be.OrderType.IOC and not (self._h.best_bid(sym) > 0 and self._h.best_ask(sym) > 0):
            self.hit = True
        return out


def _tick_run(seed, cfg, interval, with_hft):
    """One tick-model run (exp1/exp4 construction) with the probe's measurements."""
    from batch_auction import BatchAuctionScheduler
    from bcs_market_maker import BCSMarketMaker
    from fundamental_value import FundamentalValueProcess
    from hft_agent import HFTAgent
    from liquidity_gap_detector import count_liquidity_gaps
    from noise_trader import NoiseTrader
    from scheduler import LatencyScheduler

    class MidTick(LatencyScheduler):
        def _record(self, t, v):
            super()._record(t, v)
            self.snapshots[-1]["midtick_one_sided"] = self._h.hit
            self._h.hit = False

    fund = FundamentalValueProcess(v0=100 * be.PRICE_PRECISION, sigma=cfg["sigma"], dt_us=cfg["dt_us"], seed=seed)
    mm = BCSMarketMaker(1, base_half_spread=cfg["base_half_spread"], quote_qty=cfg["quote_qty"],
                        latency_us=cfg["mm_latency_us"], inventory_skew=cfg["mm_inventory_skew"],
                        adverse_sensitivity=cfg["adverse_sensitivity"], spread_decay=cfg["spread_decay"],
                        spread_cap_mult=cfg["spread_cap_mult"], min_quote_qty=cfg["min_quote_qty"])
    noise = [NoiseTrader(100 + i, lambda_per_tick=cfg["lambda_per_tick"], qty=cfg["noise_qty"],
                         seed=seed * 1000 + i) for i in range(cfg["n_noise"])]
    hfts = [HFTAgent(200 + j, latency_us=cfg["hft_latency_us"], race_noise_us=cfg["hft_race_noise_us"],
                     transaction_cost_bps=cfg["hft_cost_bps"], qty=cfg["hft_qty"], seed=seed * 2000 + j)
            for j in range(cfg["n_hft"])] if with_hft else []
    h = be.EngineHarness()
    h.add_symbol(1)
    h.start()
    if interval == CONTINUOUS:
        sched = MidTick(_MidTickHarness(h), 1, cfg["dt_us"])
    else:
        sched = BatchAuctionScheduler(h, 1, cfg["dt_us"], interval)
    res = sched.run([mm] + noise + hfts, fund, cfg["duration_us"])
    h.stop()
    return _tick_measure(res, mm, noise, hfts, cfg, count_liquidity_gaps)


def _tick_measure(res, mm, noise, hfts, cfg, count_gaps):
    def cls(pid):
        return "mm" if pid == 1 else ("nt" if pid < 200 else "hft")
    v_at = {s["t"]: s["v"] for s in res.snapshots}
    row = dict.fromkeys(("mm_edge_nt", "mm_edge_hft", "vol_mm_nt", "vol_mm_hft", "vol_hft_nt",
                         "vol_nt_nt", "vol_hft_hft"), 0.0)
    for tr in res.trades:
        b, s = cls(tr["buyer_id"]), cls(tr["seller_id"])
        if "mm" in (b, s):
            other = s if b == "mm" else b
            e = (v_at[tr["t"]] - tr["price"]) * tr["quantity"] / be.PRICE_PRECISION
            row["mm_edge_" + other] += e if b == "mm" else -e
            row["vol_mm_" + other] += tr["quantity"]
        else:
            row["vol_" + "_".join(sorted((b, s)))] += tr["quantity"]
    mark = res.snapshots[-1]["mid"] or res.snapshots[-1]["v"]
    row["mm_pnl"] = mm.pnl(mark)
    row["nt_pnl"] = sum(a.pnl(mark) for a in noise)
    row["hft_pnl"] = sum(a.pnl(mark) for a in hfts)
    row["gaps"] = count_gaps(res.snapshots, cfg["dt_us"], cfg["sigma"])
    if "midtick_one_sided" in res.snapshots[0]:
        mid = [dict(s, best_bid=0) if s["midtick_one_sided"] else s for s in res.snapshots]
        row["gaps_midtick"] = count_gaps(mid, cfg["dt_us"], cfg["sigma"])
    return row


TICK_VARIANTS = {"published": {}, "no_noise": {"n_noise": 0}, "one_hft": {"n_hft": 1}}
TICK_ARMS = (CONTINUOUS, 1, 5)


def tick_interval1_probe(n_seeds=100, boot_seed=0):
    """Tick-model probe behind paper/batch_interval1_note.md.

    For each variant of Exp 1's operating point and each arm, per-seed maker
    P&L legs (edge against V at the fill, by counterparty), cross-class
    volume and gaps, with and without HFTs; continuous runs also count gaps
    as sampled at the worst instant inside the tick (after an IOC, before
    the same tick's requote). Seeds 1..n match exp4's.
    """
    from exp1_primary import CFG as TICK_CFG
    seeds = range(1, n_seeds + 1)
    cells = []
    for name, extra in TICK_VARIANTS.items():
        cfg = {**TICK_CFG, **extra}
        rows = {(arm, hft): [_tick_run(s, cfg, arm, hft) for s in seeds]
                for arm in TICK_ARMS for hft in (True, False)}
        for (arm, hft), rs in rows.items():
            cont = rows[(CONTINUOUS, hft)]
            base = rows[(arm, False)]
            cells.append({
                "variant": name, "arm": arm, "with_hft": hft,
                "ci": bootstrap_ci_table(rs, sorted(rs[0]), seed=boot_seed),
                "mm_pnl_minus_continuous": bootstrap_ci(
                    [r["mm_pnl"] - c["mm_pnl"] for r, c in zip(rs, cont)], seed=boot_seed),
                "hft_effect": {k: bootstrap_ci([r[k] - b[k] for r, b in zip(rs, base)], seed=boot_seed)
                               for k in ("mm_pnl", "mm_edge_nt", "mm_edge_hft", "nt_pnl")},
            })
    return {"source": "tick model, exp1_primary.CFG + variants", "n_seeds": n_seeds,
            "variants": TICK_VARIANTS, "arms": list(TICK_ARMS), "cells": cells}


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--seeds", type=int, default=None)
    ap.add_argument("--tick-probe", action="store_true")
    args = ap.parse_args(argv)
    if args.tick_probe:
        report = tick_interval1_probe(args.seeds or 100)
        out = _ROOT / "results" / "experiments" / "exp11_interval1_probe.json"
    else:
        report = run_grid(CFG, args.seeds or 20)
        out = _ROOT / "results" / "pilot" / "exp11_batch_semantics.json"
        print_summary(report)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2, sort_keys=True))
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
