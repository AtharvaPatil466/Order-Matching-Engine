#!/usr/bin/env python3
"""Exp 9 — does the event model recover BCS's own closed-form equilibrium? (P1)

Why this exists: a reviewer asked for the strongest validation available —
configure the simulator to Budish, Cramton & Shim's (2015) assumptions and
check it recovers their equilibrium spread and rents. Every later event-model
result is only worth what this check is: if the scheduler's race or the
accounting were wrong, BCS's numbers would not come back. And where they do
not come back, the gap names a mechanism the simulator has and BCS do not.

What runs. `paper/bcs_replication_spec.md` fixes everything: a pure-jump public
signal (Laplace jumps of scale b, no diffusion), one LP quoting one unit at
V ± h that cancels and requotes on every jump and refills a filled quote at its
own price, N − 1 stale-quote snipers sending one-unit IOCs, one-unit Poisson
investors at market, every firm at 10 µs. Per cell (λ_j/λ_i, N) the LP's h runs
over two seven-point grids around h* = b·W(λ_j/λ_i) and h₀(N) =
b·W((N−1)/N·λ_j/λ_i). Each run seed derives separate path, investor and
tie-break seeds, so every h and every N meets the SAME jumps and investors
(common random numbers) and a curve point differs from its neighbour only
through h and the race draws.

What is measured, all from fills, the path and a zero-latency quote probe
(spec §4): the race-loss share on snipe-able jumps against (N−1)/N; profit per
successful snipe against b; the sniper take rate; the LP's profit curve π_LP(h)
and its indifference curve D(h) = π_LP − mean sniper profit; the per-seed roots
ĥ of D (theory h*) and ĥ₀ of π_LP (theory h₀(N)), found by interpolating each
seed's curve across the grid; the slope of ĥ on (N−1)/N (theory 0); and, at h*
with 1 µs jitter, the race-loss share again. Every quantity gets a 95% interval
(seed bootstrap, Wilson for pooled proportions) and the spec's verdict: PASS if
the interval sits inside the band, MISS if it is disjoint, INCONCLUSIVE
otherwise. The spec calls MISS "FAIL"; P1 calls it a miss.

Not run: the batch-auction arm (spec §3.1). EventScheduler has no batch mode
yet (spec §7), so there is nothing to configure.

Run (from bcs_research/):
    .venv/bin/python experiments/exp9_bcs_replication.py --seeds 40
Raw per-run rows stream to <out>.runs.jsonl beside the summary JSON.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys
import time
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

import numpy as np
from scipy.stats import binomtest

_HERE = Path(__file__).resolve().parent
_ROOT = _HERE.parent
for _d in ("build", "agents", "simulation", "metrics", "analysis"):
    _p = str(_ROOT / _d)
    if _p not in sys.path:
        sys.path.insert(0, _p)

import bcs_engine as be                                         # noqa: E402
from bcs_theory import (SPEC_B, SPEC_JUMP_RATIOS, SPEC_LAM_INVEST, SPEC_N,  # noqa: E402
                        laplace_equilibrium, laplace_half_spread, race_loss_probability)
from bootstrap import bootstrap_ci                              # noqa: E402
from contracts import Cancel, Order, SignalUpdate, Wakeup                     # noqa: E402
from event_scheduler import EventScheduler                      # noqa: E402
from event_sniper import EventSniper                            # noqa: E402
from fundamental_paths import JumpDiffusionPath                 # noqa: E402

NS_PER_S = 1_000_000_000
V0 = 10_000_000.0                 # 1,000.00 (spec §2.1)
REFLECT_TICKS = 90_000            # |V - V0| cap: inside the engine's ±100,000-tick book window (JumpsOnlyPath)
LATENCY_NS = 10_000               # md and order latency, every firm and investor (spec §2.2)
JITTER_NS = 1_000                 # jitter arm: U[0, 1 µs] per callback (spec §2.3)
GRID_FACTORS = (0.85, 0.90, 0.95, 1.00, 1.05, 1.10, 1.15)
LP_ID, INVESTOR_ID, PROBE_ID, FIRST_SNIPER_ID = 1, 2, 3, 10
SYMBOL = 1
N_BOOTSTRAP, BOOT_SEED = 2000, 0  # spec §5
INVESTOR_FILL_FLOOR = 0.999       # spec §4 item 8

# Spec §5 bands. Absolute for shares, relative to the theory value otherwise;
# the curves' band is 5% of lam_i * h*, the slope's 3% of h*.
SHARE_BAND = 0.02
ROOT_BAND = 0.03
SNIPE_PROFIT_BAND = 0.03
TAKE_RATE_BAND = 0.05
CURVE_BAND = 0.05
SLOPE_BAND = 0.03

PILOT_STATUS = "pilot — pre-sign-off; not the confirmatory test of P1"


# --- agents the existing lanes do not provide ----------------------------------

class BCSLiquidityProvider:
    """Spec §2.2's LP. EventMaker's cancel-then-requote burst, plus the two things
    BCS's LP does that EventMaker deliberately does not: it quotes from the start
    (around V0), and it refills a filled quote at that quote's own price. A fill
    on a quote it has already replaced — a snipe that beat its cancel — is
    ignored, since re-posting it would re-post a stale price."""

    def __init__(self, participant_id, half_spread, v0, latency_ns):
        self.participant_id = int(participant_id)
        self.md_latency_ns = self.order_latency_ns = int(latency_ns)
        self.half_spread = int(half_spread)
        self.v0 = float(v0)
        self._oid = self.participant_id * 1_000_000_000
        self._live = {}   # order id -> (side, price) it believes rests

    def _quote(self, side, price):
        self._oid += 1
        self._live[self._oid] = (side, price)
        return Order(self._oid, side, price, 1, be.OrderType.Limit)

    def _requote(self, value):
        cancels = [Cancel(oid) for oid in self._live]
        self._live = {}
        bid = int(round(value - self.half_spread))
        ask = max(bid + 1, int(round(value + self.half_spread)))
        return cancels + [self._quote(be.Side.Buy, bid), self._quote(be.Side.Sell, ask)]

    def on_start(self, t_ns, view):
        return self._requote(self.v0)

    def on_signal(self, t_ns, update, view):
        return self._requote(update.value)

    def on_wakeup(self, t_ns, tag, view):
        return []

    def on_fill(self, t_ns, fill, view):
        quote = self._live.pop(fill.order_id, None)
        return [self._quote(*quote)] if quote is not None and fill.is_maker else []


class PoissonInvestor:
    """Spec §2.2's investors: Poisson arrivals at lam_i, a fair coin for the side,
    one unit at market. ReservationNoiseTrader with an infinite shock comes close,
    but it sends an IOC at the touch it read, which misses when the LP requotes
    during the order's flight; BCS's investor always trades."""

    def __init__(self, participant_id, rate_per_s, latency_ns, seed):
        self.participant_id = int(participant_id)
        self.md_latency_ns = self.order_latency_ns = int(latency_ns)
        self._mean_gap_ns = NS_PER_S / float(rate_per_s)
        self._rng = np.random.default_rng(seed)
        self._oid = self.participant_id * 1_000_000_000
        self.arrivals = 0

    def _next(self, t_ns):
        return Wakeup(t_ns + max(1, round(self._rng.exponential(self._mean_gap_ns))))

    def on_start(self, t_ns, view):
        return [self._next(t_ns)]

    def on_signal(self, t_ns, update, view):
        return []

    def on_fill(self, t_ns, fill, view):
        return []

    def on_wakeup(self, t_ns, tag, view):
        self.arrivals += 1
        self._oid += 1
        side = be.Side.Buy if self._rng.random() < 0.5 else be.Side.Sell
        return [self._next(t_ns), Order(self._oid, side, 0, 1, be.OrderType.Market)]


class QuoteProbe:
    """Zero-latency observer: the book as it stood at each jump, which is what
    makes a jump snipe-able (spec §4). It sends nothing, so it races nothing."""

    def __init__(self, participant_id=PROBE_ID):
        self.participant_id = int(participant_id)
        self.md_latency_ns = self.order_latency_ns = 0
        self.quotes = []   # (best_bid, best_ask) per update, in update order

    def on_start(self, t_ns, view):
        return []

    def on_signal(self, t_ns, update, view):
        self.quotes.append((view.best_bid(), view.best_ask()))
        return []

    def on_wakeup(self, t_ns, tag, view):
        return []

    def on_fill(self, t_ns, fill, view):
        return []


class JumpsOnlyPath:
    """BCS's y: the jumps of a σ = 0 JumpDiffusionPath and nothing else (spec
    §2.1), each jump's sign flipped if it would carry V more than `reflect_at`
    from v0 and the flip lands nearer.

    Why drop the rest: JumpDiffusionPath always publishes its diffusion grid; at
    σ = 0 those updates move nothing, and BCS's y is a pure jump process.

    Why reflect (a deviation from spec §2.1, which lets V wander): the engine's
    book is a FIXED window of 200,001 ticks centred on the first price it sees
    (FlatPriceMap::DEFAULT_CAPACITY, no re-centring). A quote outside it is
    accepted by submit_order, then rejected OutOfPriceRange inside the engine,
    so it never rests and the scheduler logs it "rested". An unreflected V has
    s.d. sqrt(2·λ_j·T)·b ≈ 219,000 ticks over 600 s at λ_j = 40/s, so on most
    paths the LP would silently leave the book. Flipping a sign keeps the jump
    times and |J| ~ Exp(b) (common random numbers survive); every BCS quantity
    depends on |J| and the race alone, never on the level or the direction.
    ponytail: the cap holds while every |jump| < reflect_at (90b in the spec:
    probability e^-90); a wider b needs a wider engine window, not this path.
    """

    def __init__(self, path, reflect_at: float):
        jumps = [u for u in path.updates() if u.is_jump]
        self.v0, self.duration_ns = float(path.v0), path.duration_ns
        self._t = np.array([u.t_ns for u in jumps], dtype=np.int64)
        v, values = self.v0, []
        for d in np.diff([self.v0] + [u.value for u in jumps]):
            # Flip only when the flip lands nearer v0, so a path never drifts
            # outside. |V - v0| <= reflect_at holds while every |jump| does.
            if abs(v + d - self.v0) > reflect_at and abs(v - d - self.v0) < abs(v + d - self.v0):
                d = -d
            v += d
            values.append(v)
        self._v = np.array(values)
        self._updates = [SignalUpdate(int(t), float(x), True) for t, x in zip(self._t, values)]

    def value_at(self, t_ns):
        i = int(np.searchsorted(self._t, t_ns, side="right")) - 1
        return self.v0 if i < 0 else float(self._v[i])

    def updates(self):
        return self._updates


# --- one run --------------------------------------------------------------------

def derive_seeds(seed: int) -> dict:
    """Independent path, investor and tie-break seeds from one run seed (spec §2.3),
    so a change of h or N leaves the jumps and investors untouched."""
    path, investor, ties = (int(x) for x in np.random.SeedSequence(seed).generate_state(3))
    return {"seed": int(seed), "path": path, "investor": investor, "ties": ties}


def run_one(lam_jump: float, n_firms: int, half_spread: int, seed: int,
            duration_s: float, jitter_ns: int = 0, b: float = SPEC_B,
            lam_invest: float = SPEC_LAM_INVEST) -> dict:
    """One simulated market at the spec's settings; returns the per-run tallies."""
    seeds = derive_seeds(seed)
    duration_ns = int(round(duration_s * NS_PER_S))
    path = JumpsOnlyPath(JumpDiffusionPath(V0, 0.0, duration_ns, duration_ns, seed=seeds["path"],
                                           jump_intensity_per_s=lam_jump, jump_scale=b),
                         REFLECT_TICKS)
    probe = QuoteProbe()
    investor = PoissonInvestor(INVESTOR_ID, lam_invest, LATENCY_NS, seeds["investor"])
    snipers = [EventSniper(FIRST_SNIPER_ID + k, qty=1, threshold=0, md_latency_ns=LATENCY_NS,
                           order_latency_ns=LATENCY_NS) for k in range(n_firms - 1)]
    agents = [BCSLiquidityProvider(LP_ID, half_spread, V0, LATENCY_NS), investor, probe, *snipers]
    jitter = ({a.participant_id: jitter_ns for a in agents if a is not probe}
              if jitter_ns else None)
    harness = be.EngineHarness()
    harness.add_symbol(SYMBOL)
    harness.start(disable_circuit_breaker=True)
    result = EventScheduler(harness, SYMBOL, seed=seeds["ties"], snapshot_every_ns=duration_ns,
                            race_jitter_ns=jitter).run(agents, path)
    row = measure(result, probe, investor, {s.participant_id for s in snipers}, duration_s)
    return {"lam_jump": lam_jump, "n_firms": n_firms, "half_spread": int(half_spread),
            "jitter_ns": int(jitter_ns), "duration_s": duration_s, "seeds": seeds, **row}


def measure(result, probe, investor, sniper_ids, duration_s) -> dict:
    """Spec §4 tallies. Edge is q·(V − p) for a buy at V in effect at execution
    (sign reversed for a sale): BCS's payoff, with no end-of-run mark."""
    path = result.path
    edge, fills = defaultdict(float), Counter()
    n_sniper_trades = 0
    for tr in result.trades:
        e = tr.quantity * (path.value_at(tr.t_ns) - tr.price)
        edge[tr.buyer_id] += e
        edge[tr.seller_id] -= e
        fills[tr.buyer_id] += tr.quantity
        fills[tr.seller_id] += tr.quantity
        n_sniper_trades += tr.buyer_id in sniper_ids or tr.seller_id in sniper_ids
    updates = path.updates()
    if len(probe.quotes) != len(updates):
        raise RuntimeError(f"probe saw {len(probe.quotes)} of {len(updates)} jumps")
    snipeable = [(0 < ask < u.value) or (0 < bid and u.value < bid)
                 for u, (bid, ask) in zip(updates, probe.quotes)]
    sniped = {r.update_index for r in result.races
              if any(a.participant_id in sniper_ids and a.outcome == "filled" for a in r.arrivals)}
    sniper_edge = sum(edge[s] for s in sniper_ids)
    return {
        "n_jumps": len(updates),
        "n_snipeable": sum(snipeable),
        "n_sniped": sum(1 for i in sniped if snipeable[i]),
        # Jumps that found a side of the LP's quote missing. Only the 20 µs
        # refill windows should produce any; more means quotes are not resting.
        "n_quote_gaps": sum(1 for bid, ask in probe.quotes if bid <= 0 or ask <= 0),
        "n_trades": len(result.trades),
        "n_sniper_trades": n_sniper_trades,
        "sniper_fills": sum(fills[s] for s in sniper_ids),
        "sniper_edge": sniper_edge,
        "lp_edge": edge[LP_ID],
        "investor_edge": edge[INVESTOR_ID],
        "investor_fills": fills[INVESTOR_ID],
        "investor_arrivals": investor.arrivals,
        "zero_sum_residual": sum(edge.values()),
        "lp_rate": edge[LP_ID] / duration_s,
        "sniper_rate": sniper_edge / duration_s,
        "n_events": result.n_events,
    }


def indifference(row: dict) -> float:
    """D = π_LP − mean sniper profit rate (spec §4 item 4)."""
    return row["lp_rate"] - row["sniper_rate"] / (row["n_firms"] - 1)


# --- estimation -----------------------------------------------------------------

def grid_root(hs, ys) -> tuple[float, bool, int]:
    """Root of one seed's increasing curve sampled on a grid (spec §4).

    Linear interpolation across the first sign change; with none, the end
    segment nearest zero is extrapolated. Returns (root, extrapolated,
    sign changes) so noisy multi-crossing seeds can be counted, not hidden.
    """
    hs, ys = np.asarray(hs, float), np.asarray(ys, float)
    changes = np.flatnonzero(np.sign(ys[:-1]) != np.sign(ys[1:]))
    if changes.size:
        i = int(changes[0])
    else:
        i = 0 if ys[0] > 0 else len(ys) - 2
    h0, h1, y0, y1 = hs[i], hs[i + 1], ys[i], ys[i + 1]
    return float(h0 - y0 * (h1 - h0) / (y1 - y0)), not changes.size, int(changes.size)


def root_estimate(hs, curves) -> dict:
    """ĥ over seeds: each seed's grid root, then their mean and seed-bootstrap CI.
    `curves` is seeds × grid points."""
    roots = [grid_root(hs, ys) for ys in curves]
    ci = bootstrap_ci([r for r, _, _ in roots], n_bootstrap=N_BOOTSTRAP, seed=BOOT_SEED)
    return {**ci, "per_seed": [r for r, _, _ in roots],
            "n_extrapolated": sum(x for _, x, _ in roots),
            "n_multi_crossing": sum(c > 1 for _, _, c in roots)}


def wilson(k: int, n: int) -> dict:
    ci = binomtest(k, n).proportion_ci(method="wilson")
    return {"mean": k / n, "lower": float(ci.low), "upper": float(ci.high), "k": k, "n": n}


def classify(lower: float, upper: float, band_lo: float, band_hi: float) -> str:
    """Spec §5: interval inside the band PASS, disjoint MISS, otherwise INCONCLUSIVE."""
    if band_lo <= lower and upper <= band_hi:
        return "PASS"
    if upper < band_lo or lower > band_hi:
        return "MISS"
    return "INCONCLUSIVE"


def judged(ci: dict, theory: float, half_width: float) -> dict:
    lo, hi = theory - half_width, theory + half_width
    return {**ci, "theory": theory, "band": [lo, hi], "verdict": classify(ci["lower"], ci["upper"], lo, hi)}


def paired_slope(x, per_n_roots, half_width) -> dict:
    """Slope of ĥ on (N−1)/N with a paired seed bootstrap: every N shares the
    run seeds, so seeds are resampled jointly and the pairing survives."""
    x = np.asarray(x, float)
    roots = np.asarray(per_n_roots, float)         # N values × seeds
    xc = x - x.mean()

    def slope(cols):
        return float(xc @ roots[:, cols].mean(axis=1) / (xc @ xc))

    n_seeds = roots.shape[1]
    rng = np.random.default_rng(BOOT_SEED)
    boots = [slope(rng.integers(0, n_seeds, n_seeds)) for _ in range(N_BOOTSTRAP)]
    ci = {"mean": slope(np.arange(n_seeds)), "lower": float(np.percentile(boots, 2.5)),
          "upper": float(np.percentile(boots, 97.5)), "n": n_seeds}
    return judged(ci, 0.0, half_width)


# --- the grid -------------------------------------------------------------------

def cell_grid(ratio: float, n_firms: int, b: float = SPEC_B,
              lam_invest: float = SPEC_LAM_INVEST) -> dict:
    """Theory numbers and the two seven-point h grids of one cell, rounded to ticks."""
    lam_jump = ratio * lam_invest
    h_star = laplace_half_spread(lam_invest, lam_jump, b)
    h_zero = laplace_half_spread(lam_invest, lam_jump * race_loss_probability(n_firms), b)
    return {"ratio": ratio, "n_firms": n_firms, "lam_jump": lam_jump, "h_star": h_star,
            "h_zero": h_zero,
            "grid_star": [int(round(h_star * f)) for f in GRID_FACTORS],
            "grid_zero": [int(round(h_zero * f)) for f in GRID_FACTORS]}


def tasks_for(cells, seeds, duration_s) -> list[tuple]:
    """Every run the summary needs, heaviest first so the pool load-balances."""
    tasks = set()
    for c in cells:
        for s in seeds:
            for h in c["grid_star"] + c["grid_zero"]:
                tasks.add((c["lam_jump"], c["n_firms"], h, s, duration_s, 0))
            tasks.add((c["lam_jump"], c["n_firms"], c["grid_star"][3], s, duration_s, JITTER_NS))
    return sorted(tasks, key=lambda t: (-t[0] * (t[1] + 3), t))


def run_tasks(tasks, workers, jsonl_path=None, log=print) -> dict:
    """Run every task; rows keyed by the task tuple. Rows stream to jsonl_path."""
    rows, t0 = {}, time.time()
    sink = open(jsonl_path, "w") if jsonl_path else None

    def keep(k, task, row):
        rows[task] = row
        if sink:
            sink.write(json.dumps(row) + "\n")
            sink.flush()
        if k % 50 == 0 or k == len(tasks):
            log(f"[exp9] {k}/{len(tasks)} runs, {time.time() - t0:.0f} s")

    try:
        if workers == 1:   # inline: tests and debugging, no process start-up
            for k, t in enumerate(tasks, 1):
                keep(k, t, run_one(*t[:5], jitter_ns=t[5]))
            return rows
        with ProcessPoolExecutor(max_workers=workers) as pool:
            futures = {pool.submit(run_one, *t[:5], jitter_ns=t[5]): t for t in tasks}
            for k, fut in enumerate(as_completed(futures), 1):
                keep(k, futures[fut], fut.result())
    finally:
        if sink:
            sink.close()
    return rows


def summarize_cell(c: dict, seeds, duration_s, rows) -> dict:
    """Every spec §5 row for one cell, with theory, interval and verdict."""
    lam_jump, n = c["lam_jump"], c["n_firms"]
    eq = laplace_equilibrium(SPEC_LAM_INVEST, lam_jump, SPEC_B, n)
    h_star, lose = c["h_star"], race_loss_probability(n)
    curve_tol = CURVE_BAND * SPEC_LAM_INVEST * h_star

    def run(h, s, jitter=0):
        return rows[(lam_jump, n, h, s, duration_s, jitter)]

    def boot(values):
        return bootstrap_ci(values, n_bootstrap=N_BOOTSTRAP, seed=BOOT_SEED)

    at_star = [run(c["grid_star"][3], s) for s in seeds]
    jit = [run(c["grid_star"][3], s, JITTER_NS) for s in seeds]
    d_curves = [[indifference(run(h, s)) for h in c["grid_star"]] for s in seeds]
    pi_curves = [[run(h, s)["lp_rate"] for h in c["grid_zero"]] for s in seeds]

    def pooled_race_loss(rs):
        return wilson(sum(r["n_sniped"] for r in rs), sum(r["n_snipeable"] for r in rs))

    h_hat = judged(root_estimate(c["grid_star"], d_curves), h_star, ROOT_BAND * h_star)
    h0_hat = judged(root_estimate(c["grid_zero"], pi_curves), c["h_zero"], ROOT_BAND * c["h_zero"])
    snipe_profit = boot([r["sniper_edge"] / r["sniper_fills"] for r in at_star if r["sniper_fills"]])
    stale = wilson(sum(r["n_sniper_trades"] for r in at_star), sum(r["n_trades"] for r in at_star))

    curves = []
    for h in sorted(set(c["grid_star"] + c["grid_zero"])):
        prize = lam_jump * SPEC_B * math.exp(-h / SPEC_B)
        rs = [run(h, s) for s in seeds]
        curves.append({
            "h": h,
            "pi_lp": judged(boot([r["lp_rate"] for r in rs]),
                            SPEC_LAM_INVEST * h - prize * lose, curve_tol),
            "D": judged(boot([indifference(r) for r in rs]), SPEC_LAM_INVEST * h - prize, curve_tol),
        })

    all_runs = [r for (lj, nf, *_), r in rows.items() if lj == lam_jump and nf == n]
    take_theory = lam_jump * SPEC_B * math.exp(-c["grid_star"][3] / SPEC_B) * lose
    out = {
        **c,
        "theory": eq,
        "race_loss": judged(pooled_race_loss(at_star), lose, SHARE_BAND),
        "h_hat": h_hat,
        "h0_hat": h0_hat,
        "snipe_profit": judged(snipe_profit, SPEC_B, SNIPE_PROFIT_BAND * SPEC_B),
        "sniper_take_rate": judged(boot([r["sniper_rate"] for r in at_star]),
                                   take_theory, TAKE_RATE_BAND * take_theory),
        "stale_volume_share": judged(stale, eq["stale_volume_share"], SHARE_BAND),
        "jitter_race_loss": judged(pooled_race_loss(jit), lose, SHARE_BAND),
        "curves": curves,
        "curve_points_passing": sum(p[k]["verdict"] == "PASS" for p in curves for k in ("pi_lp", "D")),
        "curve_points": 2 * len(curves),
        "sanity": {
            "investor_fill_share": (sum(r["investor_fills"] for r in all_runs)
                                    / sum(r["investor_arrivals"] for r in all_runs)),
            "min_investor_fill_share": min(r["investor_fills"] / r["investor_arrivals"]
                                           for r in all_runs),
            "investor_fills_over_lam_T": (sum(r["investor_fills"] for r in at_star)
                                          / (len(at_star) * SPEC_LAM_INVEST * duration_s)),
            "max_abs_zero_sum_residual": max(abs(r["zero_sum_residual"]) for r in all_runs),
            "quote_gap_share": (sum(r["n_quote_gaps"] for r in all_runs)
                                / sum(r["n_jumps"] for r in all_runs)),
        },
    }
    out["sanity"]["investor_fill_ok"] = out["sanity"]["investor_fill_share"] >= INVESTOR_FILL_FLOOR
    out["primary_verdicts"] = {"race_loss": out["race_loss"]["verdict"], "h_hat": h_hat["verdict"]}
    return out


def overall(verdicts) -> str:
    verdicts = list(verdicts)
    if "MISS" in verdicts:
        return "MISS"
    return "PASS" if all(v == "PASS" for v in verdicts) else "INCONCLUSIVE"


def summarize(cells, seeds, duration_s, rows) -> dict:
    by_cell = [summarize_cell(c, seeds, duration_s, rows) for c in cells]
    slopes = {}
    for ratio in sorted({c["ratio"] for c in cells}):
        block = [c for c in by_cell if c["ratio"] == ratio]
        if len(block) < 2:
            continue
        x = [race_loss_probability(c["n_firms"]) for c in block]
        slopes[str(ratio)] = paired_slope(x, [c["h_hat"]["per_seed"] for c in block],
                                          SLOPE_BAND * block[0]["h_star"])
    primary = [v for c in by_cell for v in c["primary_verdicts"].values()]
    primary += [s["verdict"] for s in slopes.values()]
    return {"cells": by_cell, "h_hat_slope_on_race_loss": slopes, "p1_primary": overall(primary)}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--seeds", type=int, default=40, help="seeds per cell (spec: 40)")
    ap.add_argument("--first-seed", type=int, default=0)
    ap.add_argument("--duration-s", type=float, default=600.0, help="spec: 600")
    ap.add_argument("--ratios", type=float, nargs="+", default=list(SPEC_JUMP_RATIOS))
    ap.add_argument("--n-firms", type=int, nargs="+", default=list(SPEC_N))
    ap.add_argument("--workers", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--out", type=Path, default=_ROOT / "results" / "pilot" / "exp9_bcs_replication.json")
    ap.add_argument("--status", default=PILOT_STATUS)
    args = ap.parse_args(argv)

    seeds = list(range(args.first_seed, args.first_seed + args.seeds))
    cells = [cell_grid(r, n) for r in args.ratios for n in args.n_firms]
    tasks = tasks_for(cells, seeds, args.duration_s)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    print(f"[exp9] {len(cells)} cells x {len(seeds)} seeds -> {len(tasks)} runs on {args.workers} workers",
          flush=True)
    t0 = time.time()
    rows = run_tasks(tasks, args.workers, args.out.with_suffix(".runs.jsonl"),
                     log=lambda m: print(m, flush=True))
    report = {
        "status": args.status,
        "experiment": "exp9_bcs_replication (P1; paper/bcs_replication_spec.md)",
        "config": {"lam_invest": SPEC_LAM_INVEST, "b": SPEC_B, "v0": V0, "latency_ns": LATENCY_NS,
                   "reflect_ticks": REFLECT_TICKS,
                   "jitter_arm_ns": JITTER_NS, "duration_s": args.duration_s,
                   "ratios": args.ratios, "n_firms": args.n_firms, "grid_factors": GRID_FACTORS,
                   "n_bootstrap": N_BOOTSTRAP, "bootstrap_seed": BOOT_SEED,
                   "batch_arm": "not run: EventScheduler has no batch mode (spec §3.1, §7)"},
        "seeds": [derive_seeds(s) for s in seeds],
        "wall_s": time.time() - t0,
        **summarize(cells, seeds, args.duration_s, rows),
    }
    args.out.write_text(json.dumps(report, indent=2, sort_keys=True))
    print(f"[exp9] P1 primary: {report['p1_primary']}  -> {args.out}", flush=True)
    return report


if __name__ == "__main__":
    main()
