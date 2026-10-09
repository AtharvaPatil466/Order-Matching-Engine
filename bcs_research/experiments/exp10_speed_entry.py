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


if __name__ == "__main__":
    args = sys.argv[1:]
    main(int(args[0]) if args else 40, float(args[1]) if len(args) > 1 else 600.0)
