#!/usr/bin/env python3
"""Experiment 10 — speed as an investment, free entry of snipers, and the waste.

Why this exists. Experiment 3 (paper §4.3) swept the number of snipers k as a
parameter and let them be fast for free, so the transfer was zero-sum by
construction and BCS's welfare claim — the arms race burns real resources —
was outside what it could measure. Here latency is BOUGHT: each sniper pays c
per second for the fast line, and snipers enter until the marginal entrant's
profit net of c is zero. The speed spend at that point is the rent dissipated
into fibre and towers (BCS 2015, §VI), and the welfare table says who funds it.

Design.
  - Market: one EventMaker (fixed half-spread, cancel-and-requote on every
    update), N EventSnipers, one ReservationNoiseTrader population, on a
    JumpDiffusionPath with Laplace news jumps. In the BCS case the maker is a
    fast firm too: it buys the same line at c, so its cancel and the snipers'
    IOCs tie at the engine and the seeded tie-break makes the race uniform
    over the N + 1 messages, exactly BCS's race.
  - Profit table: per-sniper net profit for N = 0..N_max on the same path and
    noise seeds for every N (common random numbers), so the N-differences are
    not path noise. c enters only the entry condition, so one table per
    (half-spread, fee schedule) serves every c.
  - Free entry: N* = largest N whose per-sniper profit is >= c, reported with
    the range of N whose bootstrap CI straddles c (`free_entry`).
  - Two tiers: a fast line at c and a free slow line; each of a fixed pool of
    snipers picks the tier that pays it more given the others, iterated one
    deviator at a time to a fixed point (`tier_fixed_point`).
  - Welfare at N*: sniper gross rent, speed spend, sniper net, maker P&L,
    noise-trader cost, exchange fees, the dissipated share, swept over c,
    half-spread and fee schedule; beside it the paper's exogenous-N world
    (N = 3, speed free). The same entry table is also run against the
    paper's slow maker, which never wins a race (P4's H4c).
  - Theory: every cell carries BCS's prediction (prize rate, N*, dissipated
    share; `theory`) and the gap to the measurement, at absolute costs and at
    P4's costs c = R/3.5, R/5.5, R/10.5.

Status: a PILOT. The pre-registration (paper/PREREGISTRATION.md, P4) needs the
operating point signed off before a confirmatory run, so output goes to
results/pilot/ and is labelled as such.

Rent is marked per fill at the true V at execution, not at the end of the run.
V is a martingale, so the two marks have the same mean, but the end mark adds
each participant's inventory carry over the whole run, whose variance swamps
the rent (position.py, Experiment 4). The end-mark accounting.decompose
figures are kept beside it for the identity checks.

Known model caveat. Book data has no latency (contracts.py): a sniper slower
than the maker's whole cancel-and-requote sees the fresh book and sends no
IOC, where a real one would send an IOC that misses. Fills and P&L are the
same either way, but IOC-attempt counts understate real message traffic, so
they are never used here as a cost basis.

Parameters are PLACEHOLDERS until the DATA lane re-estimates jumps and
diffusion from the tape. They live in CONFIG["path"] in from_calibration's
units, so swapping in the calibrated dict is the whole change.

Run: bcs_research/.venv/bin/python bcs_research/experiments/exp10_speed_entry.py [n_seeds] [duration_s]
"""
from __future__ import annotations

import json
import math
import os
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_ROOT = _HERE.parent
for _d in ("build", "simulation", "agents", "metrics", "analysis"):
    _p = str(_ROOT / _d)
    if _p not in sys.path:
        sys.path.insert(0, _p)

import numpy as np  # noqa: E402
from scipy import optimize, stats  # noqa: E402

import bcs_engine as be  # noqa: E402
import bcs_theory  # noqa: E402
from accounting import TAKER_HEAVY, ZERO, build_ledger, decompose  # noqa: E402
from bootstrap import bootstrap_ci  # noqa: E402
from event_maker import EventMaker  # noqa: E402
from event_noise import ReservationNoiseTrader  # noqa: E402
from event_scheduler import EventScheduler  # noqa: E402
from event_sniper import EventSniper  # noqa: E402
from fundamental_paths import JumpDiffusionPath  # noqa: E402

P = be.PRICE_PRECISION
NS_PER_S = 1_000_000_000
SYM = 1
MAKER, NOISE, FIRST_SNIPER = 1, 2, 10
FEES = {"zero": ZERO, "taker_heavy": TAKER_HEAVY}
PAPER_N = 3   # Experiment 3's operating point: exogenous N, speed free

CONFIG = {
    # PLACEHOLDERS in JumpDiffusionPath.from_calibration units (bp of v0).
    # Jumps: Laplace scale 6 bp = 3 base half-spreads, 2 per second.
    "path": {"v0": 100 * P, "sigma_bp_per_sqrt_s": 2.0, "jump_intensity_per_s": 2.0,
             "jump_size_dist": "laplace", "jump_scale_bp": 6.0, "update_interval_ms": 10.0},
    "maker_qty": 10,
    "sniper_qty": 10,                 # = maker_qty: one snipe clears the stale quote
    "noise_rate_per_s": 25.0,
    "noise_eps_bp": 10.0,             # reservation-value shock sd
    "noise_qty": 2,
    "noise_latency_ns": 1_000_000,    # retail: 1 ms each way
    "tier_latency_ns": {"fast": 5_000, "slow": 50_000},   # one way: md = order
    "maker_tier": "fast",             # BCS: the liquidity provider buys speed too
}
HALF_SPREADS_BP = (1.0, 2.0, 4.0)
N_GRID = tuple(range(0, 21))
COSTS = (0.4, 0.2, 0.1, 0.05)         # per sniper per second, money units
TWO_TIER_POOL = 8
# P4's cost design: c = R/m puts R/c mid-way between integers (no knife edge).
P4_RENT_MULTIPLES = (3.5, 5.5, 10.5)
PILOT_OUT = _ROOT / "results" / "pilot" / "exp10_speed_entry.json"


def _fixed(cfg: dict, bp: float) -> float:
    return bp * 1e-4 * cfg["path"]["v0"]


def _agents(cfg, half_spread_bp, sniper_tiers, fees, seed):
    lat = cfg["tier_latency_ns"]
    m = lat[cfg["maker_tier"]]
    # ponytail: the sniper's threshold is its taker fee at v0 (a rational
    # sniper never snipes below its fee); the maker and noise traders ignore
    # fees. Price drift over a run is << v0, so v0 stands in for the price.
    threshold = fees.taker_bps * 1e-4 * cfg["path"]["v0"]
    agents = [EventMaker(MAKER, _fixed(cfg, half_spread_bp), cfg["maker_qty"],
                         md_latency_ns=m, order_latency_ns=m),
              ReservationNoiseTrader(NOISE, cfg["noise_rate_per_s"], _fixed(cfg, cfg["noise_eps_bp"]),
                                     v0=cfg["path"]["v0"], qty=cfg["noise_qty"],
                                     md_latency_ns=cfg["noise_latency_ns"],
                                     order_latency_ns=cfg["noise_latency_ns"], seed=seed)]
    for i, tier in enumerate(sniper_tiers):
        agents.append(EventSniper(FIRST_SNIPER + i, cfg["sniper_qty"], threshold=threshold,
                                  md_latency_ns=lat[tier], order_latency_ns=lat[tier]))
    return agents


def simulate(cfg, half_spread_bp, sniper_tiers, fee_name, seed, duration_s):
    """One run on a fresh engine. Same seed -> same path and noise for any N."""
    path = JumpDiffusionPath.from_calibration(cfg["path"], int(duration_s * NS_PER_S), seed)
    h = be.EngineHarness()
    h.add_symbol(SYM)
    h.start(disable_circuit_breaker=True)
    agents = _agents(cfg, half_spread_bp, sniper_tiers, FEES[fee_name], seed)
    return EventScheduler(h, SYM, seed=seed, snapshot_every_ns=NS_PER_S).run(agents, path)


def edge_pnl(trades, path) -> dict:
    """Each participant's trading P&L with every fill marked at V at execution.

    The expected-value twin of accounting's end-of-run V mark (V is a
    martingale), without the inventory-carry noise. Zero-sum across
    participants, like any common mark.
    """
    out: dict[int, float] = {}
    for tr in trades:
        e = (path.value_at(tr.t_ns) - tr.price) * tr.quantity / P
        out[tr.buyer_id] = out.get(tr.buyer_id, 0.0) + e
        out[tr.seller_id] = out.get(tr.seller_id, 0.0) - e
    return out


def summarize(result, sniper_tiers, fee_name) -> dict:
    """Per-run welfare legs, per second, plus the accounting identity residuals."""
    ledger = build_ledger(result.trades, FEES[fee_name])
    edge = edge_pnl(result.trades, result.path)
    secs = result.duration_ns / NS_PER_S
    sniper_ids = [FIRST_SNIPER + i for i in range(len(sniper_tiers))]
    classes = {MAKER: "maker", NOISE: "noise"} | {pid: "sniper" for pid in sniper_ids}
    w = decompose(ledger, classes, result.last_mid, result.path.value_at(result.duration_ns - 1))

    def leg(pids, f):
        return sum(f(pid) for pid in pids) / secs

    def fee(pid):
        acc = ledger.accounts.get(pid)
        return acc.fees if acc else 0.0

    def net(pid):
        return edge.get(pid, 0.0) - fee(pid)

    tier_net = {t: [net(pid) / secs for pid, tt in zip(sniper_ids, sniper_tiers) if tt == t]
                for t in ("fast", "slow")}
    return {
        "sniper_edge": leg(sniper_ids, lambda p: edge.get(p, 0.0)),
        "sniper_fees": leg(sniper_ids, fee),
        "maker_edge": edge.get(MAKER, 0.0) / secs,
        "maker_fees": fee(MAKER) / secs,
        "noise_edge": edge.get(NOISE, 0.0) / secs,
        "noise_fees": fee(NOISE) / secs,
        "exchange": ledger.exchange_revenue / secs,
        # Per-sniper net (before speed), averaged within a tier: snipers in a
        # tier are symmetric, so this is the marginal entrant's expectation.
        "fast_net": float(np.mean(tier_net["fast"])) if tier_net["fast"] else None,
        "slow_net": float(np.mean(tier_net["slow"])) if tier_net["slow"] else None,
        "sniper_gross_end_v": w["by_class"]["sniper"]["gross_v"] / secs,
        "edge_residual": sum(edge.values()),
        "gross_residual_v": w["gross_residual_v"],
        "transfer_residual_v": w["transfer_residual_v"],
        "n_jumps": len(result.races),
        "n_trades": len(result.trades),
    }


def _task(args):
    cfg, h_bp, tiers, fee_name, seed, duration_s = args
    return summarize(simulate(cfg, h_bp, tiers, fee_name, seed, duration_s), tiers, fee_name)


def run_tasks(tasks, workers=None):
    """Runs every task in a process pool; results keep task order (deterministic)."""
    if workers == 1:
        return [_task(t) for t in tasks]
    with ProcessPoolExecutor(max_workers=workers or max(1, (os.cpu_count() or 2) - 1)) as ex:
        return list(ex.map(_task, tasks, chunksize=1))


# --- free entry --------------------------------------------------------------

def free_entry(profit_by_n, cost, ci_seed=0) -> dict:
    """Equilibrium sniper count under free entry, with its uncertainty.

    profit_by_n maps N >= 1 to per-seed samples of the per-sniper profit (net
    of fees, before speed) when N snipers are in. The N-th entrant comes in
    while that profit covers `cost`, so N* is the largest N whose mean profit
    is >= cost (0 if none). The range [n_star_lo, n_star_hi] runs from the
    largest N still profitable at the CI's lower bound to the largest N not
    ruled out by its upper bound: every N between them has a CI straddling
    cost. `censored` flags an N* at the top of the grid, where the true N*
    may be larger.
    """
    ns = sorted(n for n in profit_by_n if n >= 1)
    if not ns:
        raise ValueError("profit_by_n needs at least one N >= 1")
    cis = {n: bootstrap_ci(profit_by_n[n], seed=ci_seed) for n in ns}

    def largest(ok):
        return max((n for n in ns if ok(cis[n])), default=0)

    n_star = largest(lambda c: c["mean"] >= cost)
    return {"cost": cost, "n_star": n_star,
            "n_star_lo": largest(lambda c: c["lower"] >= cost),
            "n_star_hi": largest(lambda c: c["upper"] >= cost),
            "straddles": [n for n in ns if cis[n]["lower"] < cost <= cis[n]["upper"]],
            "censored": n_star == ns[-1],
            "profit_ci": {n: cis[n] for n in ns}}


def tier_fixed_point(fast_profit, slow_profit, cost, start) -> dict:
    """Best-response dynamics over the number of fast snipers in a fixed pool.

    fast_profit[k] / slow_profit[k]: a fast / slow sniper's profit (before
    speed) when k of the pool are fast. One sniper deviates at a time: a slow
    one buys the line if fast_profit[k+1] - cost > slow_profit[k]; a fast one
    drops it if slow_profit[k-1] > fast_profit[k] - cost. It stops at a state
    no one wants to leave.

    It cannot cycle: "up from k" and "down from k+1" are opposite strict
    inequalities, so once the walk moves it keeps its direction and stops
    within `pool` steps. Several fixed points can exist, so callers start from
    both ends (all slow, all fast) to expose any multiplicity.
    ponytail: "out" is not a choice; the slow line is free and its profit is
    >= 0, so staying out never beats it.
    """
    pool = len(fast_profit) - 1
    k, path = start, [start]
    while True:
        if k < pool and fast_profit[k + 1] - cost > slow_profit[k]:
            k += 1
        elif k > 0 and slow_profit[k - 1] > fast_profit[k] - cost:
            k -= 1
        else:
            return {"n_fast": k, "path": path}
        path.append(k)


# --- BCS predictions -----------------------------------------------------------
# From the THEORY lane's analysis/bcs_theory.py (Budish, Cramton & Shim 2015,
# QJE 130(4)): sniping_prize is E[(J - h)^+] (b * exp(-h/b) for Laplace jumps:
# memoryless, so a successful snipe earns b on average), free_entry_n is eq. (7)
# with integer N (§VI.D), and eq. (3) sets the spread.

def theory(cfg, half_spread_bp, fee_name, maker_tier, cost=None) -> dict:
    """BCS's numbers for this market, in money per second.

    Mappings, derived here rather than in BCS: a snipe takes the maker's whole
    quote, so the prize rate R is lam_jump * maker_qty * prize per unit; a
    sniper pays its taker fee f per unit, so it fires only on J > h + f and
    the prize net of fees is prize(h + f). A fast maker races too (N + 1
    firms, BCS); the paper's slow maker never wins, so only the N snipers
    race. free_entry_n refuses c > R/2 (BCS footnote 26: N* >= 2); such
    cells get no prediction rather than an extrapolated one.
    """
    b = _fixed(cfg, cfg["path"]["jump_scale_bp"])
    h = _fixed(cfg, half_spread_bp)
    f = FEES[fee_name].taker_bps * 1e-4 * cfg["path"]["v0"]
    prize = bcs_theory.sniping_prize(h + f, bcs_theory.laplace_jump(b))
    rent = cfg["path"]["jump_intensity_per_s"] * cfg["maker_qty"] * prize / P
    out = {"prize_rate": rent}
    if cost is None:
        return out
    try:
        firms = int(bcs_theory.free_entry_n(rent, cost, integer=True))
    except ValueError:
        return out | {"n_star": None, "dissipated_share": None, "outside_bcs": True}
    maker_races = int(maker_tier == "fast")
    n = firms - maker_races
    # Sniper speed / sniper rent: N c / (R N/(N+1)) = (N+1) c / R with a racing maker.
    return out | {"n_star": n, "dissipated_share": firms * cost / rent}


def theory_half_spread_bp(cfg) -> float:
    """h* for this market's investors, zero fees: eq. (3) with elastic participation.

    Derived here: the maker's spread revenue from reservation traders,
    lam_n * 2 Phi(-h/eps) * q_n * h, equals the prize rate lam_j * q_m *
    prize(h). With inelastic investors it is bcs_theory.laplace_half_spread
    (b W(lam_j / lam_i), lam_i = lam_n q_n / q_m in quote units); here
    participation falls with h, so it is solved for the smallest root, the
    competitive spread.
    """
    b = _fixed(cfg, cfg["path"]["jump_scale_bp"])
    eps = _fixed(cfg, cfg["noise_eps_bp"])
    pth = cfg["path"]
    jump = bcs_theory.laplace_jump(b)

    def gap(h):
        revenue = cfg["noise_rate_per_s"] * 2 * stats.norm.sf(h / eps) * cfg["noise_qty"] * h
        return revenue - pth["jump_intensity_per_s"] * cfg["maker_qty"] * bcs_theory.sniping_prize(h, jump)

    h = optimize.brentq(gap, 0.0, eps)
    return h / (1e-4 * pth["v0"])


# --- the grid ----------------------------------------------------------------

def _ci(values):
    return bootstrap_ci(values)


def _ratio(num, den):
    return num / den if den > 0 else None


def welfare_at(rows, n, cost, maker_pays_speed) -> dict:
    """Welfare legs per second at N snipers paying `cost`, mean and CI over seeds.

    Identities (accounting.py), per run: sum of trading P&L = 0 at any common
    mark; sum of net P&L + exchange revenue = 0. Speed sits outside both, so
    all-in welfare = -speed_total: the race's waste is exactly the spend. The
    snipers' gross rent splits into fees + speed + what they keep.
    """
    maker_speed = cost if maker_pays_speed else 0.0
    per_seed = {
        "sniper_gross_rent": [r["sniper_edge"] for r in rows],
        "sniper_fees": [r["sniper_fees"] for r in rows],
        "sniper_net_after_speed": [r["sniper_edge"] - r["sniper_fees"] - n * cost for r in rows],
        "per_sniper_net_after_speed": [(r["fast_net"] - cost) if n else 0.0 for r in rows],
        "maker_net": [r["maker_edge"] - r["maker_fees"] - maker_speed for r in rows],
        "noise_cost": [r["noise_fees"] - r["noise_edge"] for r in rows],
        "exchange_revenue": [r["exchange"] for r in rows],
        # Speed over the rent left after fees: the share the race competes away
        # (BCS's measure). And over gross rent, the brief's measure.
        "dissipated_share": [_ratio(n * cost, r["sniper_edge"] - r["sniper_fees"]) for r in rows],
        "speed_share_of_gross": [_ratio(n * cost, r["sniper_edge"]) for r in rows],
    }
    out = {k: _ci([x for x in v if x is not None]) if n or "share" not in k else None
           for k, v in per_seed.items()}
    out["n"] = n
    out["sniper_speed_spend"] = n * cost
    out["speed_spend_total"] = n * cost + maker_speed
    return out


def _profit_by_n(cells, h_bp, fee_name):
    return {n: [r["fast_net"] for r in cells[(h_bp, fee_name, n)]] for n in N_GRID if n >= 1}


def _max_residual(rows):
    return max(abs(r[k]) for r in rows for k in ("edge_residual", "gross_residual_v",
                                                  "transfer_residual_v"))


def run_entry_grid(cfg, seeds, duration_s, half_spreads, fee_names, workers=None) -> dict:
    keys = [(h, f, n) for h in half_spreads for f in fee_names for n in N_GRID]
    tasks = [(cfg, h, ("fast",) * n, f, s, duration_s) for h, f, n in keys for s in seeds]
    flat = run_tasks(tasks, workers)
    k = len(seeds)
    return {key: flat[i * k:(i + 1) * k] for i, key in enumerate(keys)}


def _cost_row(cfg, cells, h, f, cost, basis):
    table = _profit_by_n(cells, h, f)
    fe = free_entry(table, cost)
    th = theory(cfg, h, f, cfg["maker_tier"], cost)
    return fe | {"cost_basis": basis, "theory": th,
                 "n_star_gap": None if th["n_star"] is None else fe["n_star"] - th["n_star"],
                 "welfare": welfare_at(cells[(h, f, fe["n_star"])], fe["n_star"], cost,
                                       cfg["maker_tier"] == "fast")}


def _cell_report(cfg, cells, h, f) -> dict:
    maker_races = int(cfg["maker_tier"] == "fast")
    th = theory(cfg, h, f, cfg["maker_tier"])
    table = _profit_by_n(cells, h, f)
    # Per seed, R-hat = mean over N of (racing firms) x (per-sniper profit):
    # every N in the grid estimates the same prize rate.
    n_seeds = len(next(iter(table.values())))
    prize_hat = _ci([np.mean([table[n][i] * (n + maker_races) for n in table])
                     for i in range(n_seeds)])
    costs = [(c, "absolute") for c in COSTS] + [(th["prize_rate"] / m, f"P4: R_theory/{m}")
                                                 for m in P4_RENT_MULTIPLES]
    rows = lambda n: cells[(h, f, n)]  # noqa: E731
    return {
        "half_spread_bp": h, "fees": f, "maker_tier": cfg["maker_tier"],
        "max_identity_residual": max(_max_residual(rows(n)) for n in N_GRID),
        "prize_rate": {"theory": th["prize_rate"], "empirical": prize_hat,
                       "gap": prize_hat["mean"] / th["prize_rate"] - 1},
        "by_n": {n: {"per_sniper_net": _ci(table[n]) if n else None,
                     "theory_per_sniper": th["prize_rate"] / (n + maker_races) if n else None,
                     "sniper_gross_rent": _ci([r["sniper_edge"] for r in rows(n)]),
                     "sniper_gross_end_v": _ci([r["sniper_gross_end_v"] for r in rows(n)]),
                     "maker_net_before_speed": _ci([r["maker_edge"] - r["maker_fees"]
                                                    for r in rows(n)]),
                     "noise_cost": _ci([r["noise_fees"] - r["noise_edge"] for r in rows(n)]),
                     "jumps_per_run": float(np.mean([r["n_jumps"] for r in rows(n)]))}
                 for n in N_GRID},
        "free_entry": [_cost_row(cfg, cells, h, f, c, basis) for c, basis in costs],
        # The paper's world: exogenous N, speed free -> a pure transfer, zero waste.
        "exogenous_paper_world": welfare_at(rows(PAPER_N), PAPER_N, 0.0, False),
    }


def entry_report(cfg, cells) -> list:
    return [_cell_report(cfg, cells, h, f) for h, f in dict.fromkeys((h, f) for h, f, _ in cells)]


def run_two_tier(cfg, seeds, duration_s, half_spread_bp=2.0, fee_name="zero", workers=None):
    """Fast line at c vs a free slow line, for a fixed pool of TWO_TIER_POOL snipers.

    The maker stays on the slow line here (it does not buy speed), so a free
    slow sniper ties it — the case most favourable to the slow tier.
    """
    cfg = cfg | {"maker_tier": "slow"}
    states = range(TWO_TIER_POOL + 1)
    tasks = [(cfg, half_spread_bp, ("fast",) * k + ("slow",) * (TWO_TIER_POOL - k), fee_name,
              s, duration_s) for k in states for s in seeds]
    flat = run_tasks(tasks, workers)
    n = len(seeds)
    rows = {k: flat[k * n:(k + 1) * n] for k in states}

    def mean_of(key, k):
        vals = [r[key] for r in rows[k] if r[key] is not None]
        return float(np.mean(vals)) if vals else 0.0

    fast = [mean_of("fast_net", k) for k in states]
    slow = [mean_of("slow_net", k) for k in states]
    return {
        "half_spread_bp": half_spread_bp, "fees": fee_name, "pool": TWO_TIER_POOL,
        "maker_tier": "slow",
        "by_n_fast": {k: {"fast_net": _ci([r["fast_net"] for r in rows[k]]) if k else None,
                          "slow_net": _ci([r["slow_net"] for r in rows[k]])
                          if k < TWO_TIER_POOL else None}
                      for k in states},
        "fixed_points": [{"cost": c,
                          "from_all_slow": tier_fixed_point(fast, slow, c, 0),
                          "from_all_fast": tier_fixed_point(fast, slow, c, TWO_TIER_POOL),
                          "theory_single_tier_n_star": theory(cfg, half_spread_bp, fee_name,
                                                              "slow", c).get("n_star")}
                         for c in COSTS],
        "max_identity_residual": max(_max_residual(rows[k]) for k in states),
    }


def _jsonable(x):
    if isinstance(x, dict):
        return {str(k): _jsonable(v) for k, v in x.items()}
    if isinstance(x, (list, tuple)):
        return [_jsonable(v) for v in x]
    return x


def main(n_seeds=40, duration_s=600.0, out_path=PILOT_OUT):
    seeds = list(range(n_seeds))
    t0 = time.time()
    paper_cfg = CONFIG | {"maker_tier": "slow"}
    bcs_cells = run_entry_grid(CONFIG, seeds, duration_s, HALF_SPREADS_BP, tuple(FEES))
    print(f"BCS-maker entry grid: {time.time() - t0:.0f}s", flush=True)
    paper_cells = run_entry_grid(paper_cfg, seeds, duration_s, (2.0,), ("zero",))
    print(f"paper-maker entry grid: {time.time() - t0:.0f}s", flush=True)
    two_tier = run_two_tier(CONFIG, seeds, duration_s)
    print(f"two tier: {time.time() - t0:.0f}s", flush=True)
    report = {
        "experiment": "exp10_speed_entry",
        "status": "pilot — pre-sign-off, placeholder parameters; not a test of P4",
        "config": CONFIG, "half_spreads_bp": HALF_SPREADS_BP, "n_grid": N_GRID,
        "costs_per_sniper_per_s": COSTS, "p4_rent_multiples": P4_RENT_MULTIPLES,
        "fees": {k: vars(v) for k, v in FEES.items()},
        "seeds": seeds, "duration_s": duration_s, "paper_exogenous_n": PAPER_N,
        "theory_half_spread_bp": theory_half_spread_bp(CONFIG),
        "units": "money per second (engine price / PRICE_PRECISION); profits net of fees",
        "notes": [
            "N counts snipers. With the BCS maker (fast, pays c) there are N + 1 racing firms.",
            "Rent is marked per fill at V at execution (edge_pnl); sniper_gross_end_v is the "
            "accounting.decompose end-of-run V mark of the same quantity, noisier.",
            "Identities checked per run: sum of edge P&L = 0, sum of gross P&L at end V = 0, "
            "sum of net P&L + exchange revenue = 0. Speed is outside them: all-in welfare = -speed.",
            "Theory: analysis/bcs_theory.py; the fee, quote-size and elastic-investor mappings are "
            "derived here (see theory(), theory_half_spread_bp()). c > R/2 cells have no prediction.",
            "PLACEHOLDER path parameters (config.path) until the DATA lane's calibration.",
            "Book data has no latency: a sniper slower than the maker's cancel sends no IOC "
            "rather than a missing one; fills/P&L unaffected; IOC counts are not a cost basis.",
        ],
        "entry_bcs_maker": entry_report(CONFIG, bcs_cells),
        "entry_paper_maker": entry_report(paper_cfg, paper_cells),
        "two_tier": two_tier,
        "per_seed_profit_tables": {f"{tag}_h{h}_{f}": _profit_by_n(cells, h, f)
                                   for tag, cells in (("bcs", bcs_cells), ("paper", paper_cells))
                                   for h, f in dict.fromkeys((h, f) for h, f, _ in cells)},
        "elapsed_s": round(time.time() - t0, 1),
    }
    out_path = Path(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(_jsonable(report), indent=1) + "\n")
    print(f"wrote {out_path} in {report['elapsed_s']}s")
    return report


# --- confirmatory mode: P4 in the replication spec's geometry -----------------
#
# P4 states its numbers in the spec's geometry (BCS LP at h* = 567.1 ticks,
# lambda_i = lambda_j = 10/s, b = 1,000, rent R = lambda_i h* = 5,671 ticks/s), not
# at the operating point, so the confirmatory mode runs exp9's spec market:
# one-unit LP that cancels and requotes on every jump, one-unit IOC snipers,
# one-unit Poisson investors at market, every firm 10 us, zero jitter. The
# placeholder CONFIG above is the pilot's and is not used.
#   H4a/H4b  BCS LP (as fast as the snipers, pays c like them). Firms enter one
#            at a time, the LP first: firm N + 1 enters while its measured
#            profit at N + 1 firms (the per-sniper profit; snipers are
#            symmetric) has a 95% interval above c. N* counts firms, LP
#            included; waste = N* c, over the measured total rent at N* (LP
#            plus sniper edge, BCS's "total rents").
#   H4c      the paper's maker: the same LP 10 us slower than every sniper (it
#            never wins a race) and not paying c; snipers enter alone. Waste
#            over the snipers' rent at n*; the toll is the investors' loss.
# c in {R/3.5, R/5.5, R/10.5} with R the theory rent, as P4 fixes them.
# Under fixed h* and common random numbers the investor revenue, the total
# rent and the toll are nearly invariant by construction; H4b and H4c's TOSTs
# test exactly that, as P4 states them.

SPEC_H_STAR = 567                     # round(b W(1)), exp9.cell_grid(1, n)["grid_star"][3]
SPEC_RENT = 5_671.432904097838        # lambda_i h*, ticks/s
P4_COSTS = {m: SPEC_RENT / m for m in P4_RENT_MULTIPLES}
FIRMS_GRID = tuple(range(2, 13))      # BCS arm: LP + 1..11 snipers
SNIPERS_GRID = tuple(range(1, 13))    # paper-maker arm
SLOW_LP_EXTRA_NS = 10_000
CONFIRM_SEEDS, CONFIRM_DURATION_S = 100, 1800.0


def spec_run(n_snipers: int, slow_lp: bool, seed: int, duration_s: float) -> dict:
    """One spec-geometry run: rates per second, ticks (not money)."""
    import exp9_bcs_replication as x9
    seeds = x9.derive_seeds(seed)
    dur = int(duration_s * NS_PER_S)
    path = x9.JumpsOnlyPath(JumpDiffusionPath(x9.V0, 0.0, dur, dur, seed=seeds["path"],
                                              jump_intensity_per_s=10.0, jump_scale=x9.SPEC_B),
                            x9.REFLECT_TICKS)
    lat = x9.LATENCY_NS
    lp = x9.BCSLiquidityProvider(x9.LP_ID, SPEC_H_STAR, x9.V0,
                                 lat + (SLOW_LP_EXTRA_NS if slow_lp else 0))
    inv = x9.PoissonInvestor(x9.INVESTOR_ID, x9.SPEC_LAM_INVEST, lat, seeds["investor"])
    probe = x9.QuoteProbe()
    snipers = [EventSniper(x9.FIRST_SNIPER_ID + k, qty=1, md_latency_ns=lat, order_latency_ns=lat)
               for k in range(n_snipers)]
    h = be.EngineHarness()
    h.add_symbol(SYM)
    h.start(disable_circuit_breaker=True)
    try:
        res = EventScheduler(h, SYM, seed=seeds["ties"], snapshot_every_ns=dur).run(
            [lp, inv, probe, *snipers], path)
    finally:
        h.stop()
    ids = {s.participant_id for s in snipers}
    row = x9.measure(res, probe, inv, ids, duration_s)
    lp_inv = sum((path.value_at(t.t_ns) - t.price) * (1 if t.buyer_id == x9.LP_ID else -1)
                 for t in res.trades if {t.buyer_id, t.seller_id} == {x9.LP_ID, x9.INVESTOR_ID})
    return {"lp_rate": row["lp_rate"], "sniper_rate": row["sniper_rate"],
            "per_sniper": row["sniper_rate"] / n_snipers if n_snipers else 0.0,
            "rent": row["lp_rate"] + row["sniper_rate"], "toll": -row["investor_edge"] / duration_s,
            "lp_investor_revenue": lp_inv / duration_s, "n_snipeable": row["n_snipeable"],
            "n_sniped": row["n_sniped"], "zero_sum_residual": row["zero_sum_residual"]}


def _ci(v):
    return bootstrap_ci(v, seed=0)


def entry_stop(entrant_profit: dict, cost: float, first: int) -> dict:
    """Enter one at a time while the next entrant's 95% interval lies above cost.

    entrant_profit: {n: per-seed profit of the n-th entrant}. Starts with
    first - 1 in; returns the count in when entry stops, and every interval.
    """
    cis = {n: _ci(v) for n, v in sorted(entrant_profit.items())}
    n_in = first - 1
    for n in sorted(cis):
        if n != n_in + 1 or cis[n]["lower"] <= cost:
            break
        n_in = n
    return {"n_star": n_in, "censored": n_in == max(cis), "entrant_ci": cis}


def _waste_share(rows, n, cost) -> dict:
    """n c over the mean rent: the percentile interval transforms monotonically."""
    r = _ci([x["rent_of_interest"] for x in rows])
    return {"mean": n * cost / r["mean"], "lower": n * cost / r["upper"],
            "upper": n * cost / r["lower"]}


def p4_verdicts(bcs: dict, paper: dict) -> dict:
    """H4a, H4b, H4c from {n: per-seed rows} of the two arms."""
    import operating_point as op
    out = {"H4b": {"cells": []}, "H4c": {"cells": []}}
    bcs_n = {}
    for m, c in P4_COSTS.items():
        entry = entry_stop({n: [r["per_sniper"] for r in bcs[n]] for n in FIRMS_GRID}, c, 2)
        n = entry["n_star"]
        rows = [{"rent_of_interest": r["rent"]} for r in bcs.get(n, [])] if n >= 2 else []
        share = _waste_share(rows, n, c) if rows else None
        theory = math.floor(SPEC_RENT / c) * c / SPEC_RENT
        cell = {"cost": c, "rent_multiple": m, "theory_n_star": math.floor(SPEC_RENT / c),
                "entry": entry, "waste_share": share, "theory_waste_share": theory,
                "waste_verdict": op.band_verdict(share["lower"], share["upper"], theory - 0.03,
                                                 theory + 0.03) if share else "MISS"}
        out["H4b"]["cells"].append(cell)
        bcs_n[m] = n
        if m == 5.5:
            ci5, ci6 = entry["entrant_ci"][5], entry["entrant_ci"][6]
            excluded = ci5["upper"] < c or ci6["lower"] > c
            waste = op.band_verdict(share["lower"], share["upper"], 0.909 - 0.05, 0.909 + 0.05) \
                if share else "MISS"
            if (n != 5 and excluded) or waste == "MISS":
                h4a = "MISS"
            else:
                h4a = "HIT" if n == 5 and waste == "HIT" else "INCONCLUSIVE"
            out["H4a"] = {"verdict": h4a, "n_star": n, "prediction_n_star": 5,
                          "entrant_5": ci5, "entrant_6": ci6, "waste_share": share,
                          "waste_band": [0.859, 0.959], "waste_verdict": waste}
    ref = 5.5
    inv = [op.equivalence([r["lp_investor_revenue"] for r in bcs[bcs_n[m]]],
                          [r["lp_investor_revenue"] for r in bcs[bcs_n[ref]]], 0.05)
           for m in P4_COSTS if m != ref and bcs_n[m] in bcs and bcs_n[ref] in bcs]
    rent = [op.equivalence([r["rent"] for r in bcs[bcs_n[m]]], [r["rent"] for r in bcs[bcs_n[ref]]],
                           0.05)
            for m in P4_COSTS if m != ref and bcs_n[m] in bcs and bcs_n[ref] in bcs]
    out["H4b"] |= {"investor_revenue_tost_vs_R/5.5": inv, "total_rent_tost_vs_R/5.5": rent,
                   "verdict": op.all_hit([c["waste_verdict"] for c in out["H4b"]["cells"]]
                                         + [e["verdict"] for e in inv + rent] if inv else ["MISS"])}
    paper_n = {}
    for m, c in P4_COSTS.items():
        entry = entry_stop({n: [r["per_sniper"] for r in paper[n]] for n in SNIPERS_GRID}, c, 1)
        n = entry["n_star"]
        rows = [{"rent_of_interest": r["sniper_rate"]} for r in paper.get(n, [])]
        share = _waste_share(rows, n, c) if rows else None
        floor = 1 - c / SPEC_RENT
        v = ("MISS" if share is None or share["upper"] < floor
             else "HIT" if share["lower"] >= floor else "INCONCLUSIVE")
        out["H4c"]["cells"].append({"cost": c, "rent_multiple": m, "entry": entry,
                                    "dissipated_share": share, "at_least": floor, "verdict": v})
        paper_n[m] = n
    toll = [op.equivalence([r["toll"] for r in paper[paper_n[m]]],
                           [r["toll"] for r in paper[paper_n[ref]]], 0.10)
            for m in P4_COSTS if m != ref and paper_n[m] in paper and paper_n[ref] in paper]
    out["H4c"] |= {"toll_tost_vs_R/5.5": toll,
                   "verdict": op.all_hit([c["verdict"] for c in out["H4c"]["cells"]]
                                         + [t["verdict"] for t in toll] if toll else ["MISS"])}
    return out


def confirmatory(argv=None) -> dict:
    import argparse
    import operating_point as op
    ap = argparse.ArgumentParser(description="exp10 confirmatory (P4)")
    ap.add_argument("--confirmatory", action="store_true")
    ap.add_argument("--seeds", type=int, default=CONFIRM_SEEDS)
    ap.add_argument("--duration-s", type=float, default=CONFIRM_DURATION_S)
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--out-dir", type=Path, default=None)
    a = ap.parse_args(argv)
    out, ck = op.confirmatory_paths("exp10_speed_entry", a.out_dir)
    log = op.CellLog(ck)
    seeds = range(a.seeds)
    t0 = time.time()
    tasks = {json.dumps([n - 1, False, s]): (n - 1, False, s, a.duration_s)
             for n in FIRMS_GRID for s in seeds}
    tasks |= {json.dumps([n, True, s]): (n, True, s, a.duration_s)
              for n in SNIPERS_GRID for s in seeds}
    got = op.run_cached(spec_run, tasks, log, a.workers)
    bcs = {n: [got[json.dumps([n - 1, False, s])] for s in seeds] for n in FIRMS_GRID}
    paper = {n: [got[json.dumps([n, True, s])] for s in seeds] for n in SNIPERS_GRID}
    report = {
        "experiment": "exp10_speed_entry (P4), confirmatory",
        "status": op.smoke_status(a.seeds, a.duration_s),
        "geometry": "replication spec: lambda_i = lambda_j = 10/s, b = 1000 ticks, h* = 567, "
                    "unit orders, 10 us every firm, zero jitter",
        "rent_theory_ticks_per_s": SPEC_RENT, "costs": P4_COSTS,
        "slow_lp_extra_ns": SLOW_LP_EXTRA_NS, "seeds": list(seeds), "duration_s": a.duration_s,
        "units": "ticks per second",
        "p4": p4_verdicts(bcs, paper),
        "by_firms_bcs": {n: {k: _ci([r[k] for r in bcs[n]]) for k in
                             ("per_sniper", "lp_rate", "rent", "toll", "lp_investor_revenue")}
                         for n in FIRMS_GRID},
        "by_snipers_paper": {n: {k: _ci([r[k] for r in paper[n]]) for k in
                                 ("per_sniper", "sniper_rate", "toll")} for n in SNIPERS_GRID},
        "max_zero_sum_residual": max(abs(r["zero_sum_residual"]) for v in got.values() for r in [v]),
        "elapsed_s": round(time.time() - t0, 1),
    }
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(_jsonable(report), indent=1) + "\n")
    p4 = report["p4"]
    print(f"[exp10] H4a {p4['H4a']['verdict']} (N* {p4['H4a']['n_star']}), H4b "
          f"{p4['H4b']['verdict']}, H4c {p4['H4c']['verdict']} -> {out}", flush=True)
    return report


if __name__ == "__main__":
    args = sys.argv[1:]
    if "--confirmatory" in args:
        confirmatory(args)
    else:
        main(int(args[0]) if args else 40, float(args[1]) if len(args) > 1 else 600.0)
