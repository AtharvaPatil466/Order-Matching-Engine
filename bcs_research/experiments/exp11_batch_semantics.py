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


def arms(intervals=INTERVALS_NS, rationings=RATIONINGS):
    return [CONTINUOUS] + [(tau, r, p) for tau in intervals for r in rationings for p in PRICE_RULES]


def arm_label(arm):
    return arm if arm == CONTINUOUS else f"tau={arm[0]}ns/{arm[1]}/{arm[2]}"


PAIRED = ("maker_edge_zero", "sniper_edge_zero", "noise_cost_zero", "maker_edge_taker_heavy",
          "maker_pnl_zero", "maker_sniper_vol_per_jump")
ROWS_KEPT = ("maker_edge_zero", "sniper_edge_zero", "noise_cost_zero", "maker_sniper_vol_per_jump",
             "race_sniper_fill_share")   # per-seed rows stored for re-analysis; CIs cover every metric


def summarize(cfg, rows_by_cell, seeds, intervals=INTERVALS_NS, rationings=RATIONINGS, boot_seed=0):
    keys = sorted(next(iter(rows_by_cell.values()))[0])
    cells = []
    for (kind, label), rows in rows_by_cell.items():
        cont = rows_by_cell[(kind, CONTINUOUS)]
        paired = {k: bootstrap_ci([r[k] - c[k] for r, c in zip(rows, cont)], seed=boot_seed)
                  for k in PAIRED}
        cells.append({"sniper_kind": kind, "arm": label,
                      "ci": bootstrap_ci_table(rows, keys, seed=boot_seed),
                      "delta_vs_continuous": paired,
                      "rows": [{k: r[k] for k in ROWS_KEPT} for r in rows]})
    lag = 2 * (cfg["maker_latency_ns"] - cfg["sniper_latency_ns"])
    theory = {"maker_lag_ns": lag, "half_spread": half_spread(cfg),
              "slow_lp_exposure_by_tau": {str(t): fba_slow_lp_exposure(lag, t) for t in intervals}}
    return {"status": STATUS, "config": cfg, "seeds": list(seeds), "intervals_ns": list(intervals),
            "rationings": list(rationings), "price_rules": PRICE_RULES, "sniper_kinds": SNIPER_KINDS,
            "theory": theory, "cells": cells}


def run_grid(cfg, n_seeds, intervals=INTERVALS_NS, rationings=RATIONINGS, log=print):
    seeds = range(1, n_seeds + 1)
    rows_by_cell = {}
    t0 = time.time()
    for kind in SNIPER_KINDS:
        for arm in arms(intervals, rationings):
            rows_by_cell[(kind, arm_label(arm))] = [run_once(cfg, s, arm, kind) for s in seeds]
            log(f"[{time.time() - t0:7.1f}s] {kind:11s} {arm_label(arm)}")
    return summarize(cfg, rows_by_cell, seeds, intervals, rationings)


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


# --- confirmatory mode: P9 at the operating point ------------------------------
#
# The market is operating_point.build_market. P9 states neither k nor the snipe
# size, so: k = 3 (the operating point's default, ABO's 3.3 per race) and snipe
# ratio 1, one snipe clears the quote, BCS's assumption and the one under which
# footnote 34's bound is stated (both logged as §4 ambiguities). The fast maker
# is the operating point's (lag 0 <= eps); the slow maker (H9c's "tau below the
# maker's latency") observes and orders 5 ms each way, 10 ms in all.
# Arms per seed share the path, the investors and the scheduler seed.
#   H9a  tau in {1, 10, 100} ms, fast maker, time rationing, engine price rule,
#        stale snipers: sniping loss below continuous (paired interval) and its
#        upper bound <= 1.2 x lambda_j (eps / 2 tau) prize(h) x quote.
#   H9b  (i) stale snipers: maker edge and every sniper fill identical across
#        the engine and pressure price rules, exactly; (ii) competitive snipers:
#        the snipers' edge per filled lot is the stale prize under the engine
#        rule (> 2 ticks) and at most ~one tick under pressure (<= 2 ticks): the
#        measurable form of "clears at the stale quote vs at V' - 1 tick";
#        (iii) where eps/tau <= 0.01 (tau >= 10 ms) the maker's edge differs
#        across price rules by less than 1% of continuous sniping loss.
#   H9c  every tau, both makers, including tau = 50 us below the fast maker's
#        100 us and tau = 1 ms below the slow maker's 10 ms: MISS if any
#        batch-minus-continuous loss interval lies above 0; HIT only if every
#        interval's upper bound is <= 0 (failing to reject is never a hit, §2);
#        the lenient "none significantly above" reading is reported beside.
#   H9d  time vs random rationing: maker sniping loss TOST +-2% at each tau.

CONFIRM_TAUS_NS = (1_000_000, 10_000_000, 100_000_000)
BELOW_FAST_TAU_NS = 50_000
SLOW_MAKER_ONE_WAY_NS = 5_000_000
CONFIRM_K, CONFIRM_RATIO = 3, 1.0


def confirm_arms() -> list[tuple]:
    """(maker, sniper_kind, arm) for every run of one seed."""
    out = [("fast", kind, CONTINUOUS) for kind in SNIPER_KINDS]
    out += [("fast", kind, (tau, r, p)) for kind in SNIPER_KINDS for tau in CONFIRM_TAUS_NS
            for r in RATIONINGS for p in PRICE_RULES]
    out += [("fast", "stale", (BELOW_FAST_TAU_NS, "time", "engine")),
            ("slow", "stale", CONTINUOUS)]
    out += [("slow", "stale", (tau, "time", "engine")) for tau in CONFIRM_TAUS_NS]
    return out


def run_op(seed: int, maker: str, sniper_kind: str, arm, horizon_s: float) -> dict:
    """One operating-point run: the maker's sniping loss and the snipers' fills."""
    import operating_point as op
    lat = op.MAKER_ONE_WAY_NS if maker == "fast" else SLOW_MAKER_ONE_WAY_NS
    mk = op.build_market(seed, maker_latencies_ns=(lat,), k=CONFIRM_K, snipe_ratio=CONFIRM_RATIO,
                         horizon_s=horizon_s)
    agents = mk.agents
    if sniper_kind == "competitive":
        swap = {s.participant_id: CompetitiveSniper(s.participant_id, s.qty, s.threshold,
                                                    s.md_latency_ns, s.order_latency_ns)
                for s in mk.snipers}
        agents = [swap.get(a.participant_id, a) for a in agents]
    h = be.EngineHarness()
    h.add_symbol(1)
    h.start(disable_circuit_breaker=True)
    common = dict(seed=seed, snapshot_every_ns=op.SNAPSHOT_MS * 1_000_000,
                  race_jitter_ns=mk.race_jitter_ns)
    try:
        if arm == CONTINUOUS:
            sched = EventScheduler(h, 1, **common)
        else:
            tau, rationing, rule = arm
            sched = EventBatchScheduler(h, 1, tau, rationing=rationing, price_rule=rule, **common)
        res = sched.run(agents, mk.path)
    finally:
        h.stop()
    maker_id = mk.makers[0].participant_id
    snipers = {s.participant_id for s in mk.snipers}
    loss = edge_units = lots = 0.0
    fills = []
    for tr in res.trades:
        e = res.path.value_at(tr.t_ns) - tr.price           # ticks per lot, buyer's edge
        if tr.buyer_id in snipers or tr.seller_id in snipers:
            sniper_edge = e if tr.buyer_id in snipers else -e
            edge_units += sniper_edge * tr.quantity
            lots += tr.quantity
            fills.append((tr.t_ns, tr.price, tr.quantity))
            if maker_id in (tr.buyer_id, tr.seller_id):
                loss += sniper_edge * tr.quantity
    edge = op.edge_by_participant(res.trades, res.path)
    return {"maker_sniping_loss": loss / be.PRICE_PRECISION / horizon_s,
            "maker_edge": edge.get(maker_id, 0.0) / horizon_s,
            "sniper_lots": lots, "sniper_edge_ticks_per_lot": edge_units / lots if lots else None,
            "sniper_fills_hash": hash(tuple(fills)) if fills else 0,
            "n_jumps": len(res.races), "repriced": getattr(sched, "repriced", 0)}


def _arm_key(maker, kind, arm) -> str:
    return f"{maker}/{kind}/{arm_label(arm)}"


def footnote34_bound(tau_ns: int) -> float:
    """lambda_j (eps / 2 tau) prize(h) x quote, USD/s, at the operating point."""
    import operating_point as op
    from bcs_theory import laplace_jump, sniping_prize
    b = op.PATH["jump_scale_bp"] * op.BP
    prize = sniping_prize(op.BCS_HALF_SPREAD_BP * op.BP, laplace_jump(b))
    return (op.PATH["jump_intensity_per_s"] * op.JITTER_NS / (2 * tau_ns) * prize
            * op.QUOTE_LOTS / be.PRICE_PRECISION)


def p9_verdicts(rows: dict) -> dict:
    """rows: {arm key: [per-seed run dicts, seeds aligned]}."""
    import operating_point as op

    def col(key, field="maker_sniping_loss"):
        return [r[field] for r in rows[key]]

    def diff_ci(key, base):
        return bootstrap_ci([a - b for a, b in zip(col(key), col(base))], seed=0)

    cont = {m: _arm_key(m, "stale", CONTINUOUS) for m in ("fast", "slow")}
    cont_loss = float(sum(col(cont["fast"])) / len(rows[cont["fast"]]))
    h9a = []
    for tau in CONFIRM_TAUS_NS:
        k = _arm_key("fast", "stale", (tau, "time", "engine"))
        d, lvl, bound = diff_ci(k, cont["fast"]), bootstrap_ci(col(k), seed=0), footnote34_bound(tau)
        below = "HIT" if d["upper"] < 0 else ("MISS" if d["lower"] >= 0 else "INCONCLUSIVE")
        under = ("HIT" if lvl["upper"] <= 1.2 * bound else
                 "MISS" if lvl["lower"] > 1.2 * bound else "INCONCLUSIVE")
        h9a.append({"tau_ns": tau, "batch_minus_continuous": d, "batch_loss": lvl,
                    "bound_1.2x": 1.2 * bound, "below_continuous": below, "under_bound": under,
                    "verdict": op.all_hit([below, under])})
    same = []
    for tau in CONFIRM_TAUS_NS:
        for r in RATIONINGS:
            a, b = (rows[_arm_key("fast", "stale", (tau, r, p))] for p in PRICE_RULES)
            # Exposure events only: the pressure rule also reprices investor batches,
            # which H9b(iii) reads through the aggregate maker P&L.
            same.append(all(x["maker_sniping_loss"] == y["maker_sniping_loss"] and
                            x["sniper_fills_hash"] == y["sniper_fills_hash"] for x, y in zip(a, b)))
    per_lot = {}
    for p in PRICE_RULES:
        vals = [r["sniper_edge_ticks_per_lot"] for tau in CONFIRM_TAUS_NS
                for r in rows[_arm_key("fast", "competitive", (tau, "time", p))]
                if r["sniper_edge_ticks_per_lot"] is not None]
        per_lot[p] = bootstrap_ci(vals, seed=0) if vals else None
    eng, prs = per_lot["engine"], per_lot["pressure"]
    if eng is None or prs is None:
        comp = "INCONCLUSIVE"
    elif prs["lower"] > 2 or eng["upper"] <= 2:
        comp = "MISS"
    else:
        comp = "HIT" if prs["upper"] <= 2 and eng["lower"] > 2 else "INCONCLUSIVE"
    agg = []
    for tau in CONFIRM_TAUS_NS:
        if op.JITTER_NS / tau > 0.01:
            continue
        for kind in SNIPER_KINDS:
            a, b = (col(_arm_key("fast", kind, (tau, "time", p)), "maker_edge") for p in PRICE_RULES)
            d = bootstrap_ci([x - y for x, y in zip(b, a)], seed=0)
            band = 0.01 * cont_loss
            agg.append({"tau_ns": tau, "sniper_kind": kind, "pressure_minus_engine": d,
                        "band": band, "verdict": op.band_verdict(d["lower"], d["upper"], -band, band)})
    h9b = {"stale_identical_across_price_rules": all(same),
           "competitive_edge_per_lot_ticks": per_lot, "competitive_verdict": comp,
           "aggregate_within_1pct": agg,
           "verdict": op.all_hit(["HIT" if all(same) else "MISS", comp] + [x["verdict"] for x in agg])}
    h9c = []
    taus = {"fast": (BELOW_FAST_TAU_NS,) + CONFIRM_TAUS_NS, "slow": CONFIRM_TAUS_NS}
    for m, ts in taus.items():
        for tau in ts:
            d = diff_ci(_arm_key(m, "stale", (tau, "time", "engine")), cont[m])
            v = "MISS" if d["lower"] > 0 else ("HIT" if d["upper"] <= 0 else "INCONCLUSIVE")
            h9c.append({"maker": m, "tau_ns": tau, "batch_minus_continuous": d, "verdict": v})
    h9d = [{"tau_ns": tau, **op.equivalence(col(_arm_key("fast", "stale", (tau, "random", "engine"))),
                                            col(_arm_key("fast", "stale", (tau, "time", "engine"))),
                                            0.02)} for tau in CONFIRM_TAUS_NS]
    return {"H9a": {"cells": h9a, "verdict": op.all_hit(c["verdict"] for c in h9a)},
            "H9b": h9b,
            "H9c": {"cells": h9c, "verdict": op.all_hit(c["verdict"] for c in h9c),
                    "lenient_none_significantly_above":
                        "MISS" if any(c["verdict"] == "MISS" for c in h9c) else "HIT"},
            "H9d": {"cells": h9d, "verdict": op.all_hit(c["verdict"] for c in h9d)},
            "continuous_sniping_loss_usd_per_s": cont_loss}


def _run_op_task(seed, maker, kind, arm, horizon_s):
    return run_op(seed, maker, kind, tuple(arm) if isinstance(arm, list) else arm, horizon_s)


def confirmatory(args) -> dict:
    import operating_point as op
    out, ck = op.confirmatory_paths("exp11_batch_semantics", args.out_dir)
    log = op.CellLog(ck)
    n, horizon = args.seeds or op.N_SEEDS, args.horizon_s or op.HORIZON_S
    seeds = range(1, n + 1)
    t0 = time.time()
    tasks = {f"{_arm_key(m, k, a)}#{s}": (s, m, k, a, horizon)
             for m, k, a in confirm_arms() for s in seeds}
    got = op.run_cached(_run_op_task, tasks, log, args.workers)
    rows = {_arm_key(m, k, a): [got[f"{_arm_key(m, k, a)}#{s}"] for s in seeds]
            for m, k, a in confirm_arms()}
    report = {"experiment": "exp11_batch_semantics (P9), confirmatory",
              "status": op.smoke_status(n, horizon), "seeds": list(seeds), "horizon_s": horizon,
              "market": {"k": CONFIRM_K, "snipe_ratio": CONFIRM_RATIO,
                         "fast_maker_one_way_ns": op.MAKER_ONE_WAY_NS,
                         "slow_maker_one_way_ns": SLOW_MAKER_ONE_WAY_NS,
                         "jitter_ns": op.JITTER_NS, "taus_ns": CONFIRM_TAUS_NS,
                         "below_fast_tau_ns": BELOW_FAST_TAU_NS},
              "units": "USD per second; sniper edge per lot in ticks",
              "p9": p9_verdicts(rows),
              "arms": {k: bootstrap_ci_table(v, ["maker_sniping_loss", "maker_edge", "sniper_lots"],
                                             seed=0) for k, v in rows.items()},
              "elapsed_s": round(time.time() - t0, 1)}
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2, sort_keys=True, default=str))
    p9 = report["p9"]
    print(f"[exp11] H9a {p9['H9a']['verdict']}, H9b {p9['H9b']['verdict']}, H9c "
          f"{p9['H9c']['verdict']}, H9d {p9['H9d']['verdict']} -> {out}", flush=True)
    return report


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--seeds", type=int, default=None)
    ap.add_argument("--tick-probe", action="store_true")
    ap.add_argument("--intervals-ns", type=int, nargs="+", default=INTERVALS_NS)
    ap.add_argument("--rationings", nargs="+", default=list(RATIONINGS))
    ap.add_argument("--out-name", default="exp11_batch_semantics.json")
    ap.add_argument("--confirmatory", action="store_true", help="P9 at the operating point")
    ap.add_argument("--horizon-s", type=float, default=None, help="confirmatory smoke only")
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--out-dir", type=Path, default=None)
    args = ap.parse_args(argv)
    if args.confirmatory:
        return confirmatory(args)
    if args.tick_probe:
        report = tick_interval1_probe(args.seeds or 100)
        out = _ROOT / "results" / "experiments" / "exp11_interval1_probe.json"
    else:
        report = run_grid(CFG, args.seeds or 20, args.intervals_ns, args.rationings)
        out = _ROOT / "results" / "pilot" / args.out_name
        print_summary(report)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2, sort_keys=True))
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
