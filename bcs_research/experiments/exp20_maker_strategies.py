#!/usr/bin/env python3
"""Experiment 20 — richer maker strategies vs the adverse-selection maker (PREREGISTRATION P7).

Why this exists. Every paper result used one hand-set maker. P7 asks what
Avellaneda-Stoikov quoting, inventory skew, quote fading, multiple levels and
explicit cancels do on the event clock. Five predictions, as §6 states them,
at the signed-off operating point (operating_point.py):

  H7a (primary)  A maker that cancels on every observed signal update loses,
                 per snipe-able jump, the fraction 1 - race_win_probability(lag,
                 eps, k) of what an otherwise identical non-cancelling maker
                 loses (k/(k+1) at lag 0, 1 at lag > eps), within +-0.03.
  H7b            Inventory skew raises expected sniping loss per jump by the
                 factor E[cosh(delta/b)] over the realised skews, relative to
                 the unskewed maker at the same half-spread, within +-5%.
                 Miss: outside, or the ratio below 1.
  H7c            On a ladder, per-unit sniping loss at level j relative to level
                 1 is e^(-(h_j - h_1)/b), within +-15%.
  H7d            An A-S maker (inventory-risk spread, no adverse-fill response)
                 shows no inversion, <= 2 of 18 tape-band maker-gains cells,
                 and lower P&L with HFTs present than the adverse-selection
                 maker at equal base spread (paired interval below zero).
                 Miss: >= 6 cells, or A-S P&L not lower.
  H7e            Quote fading reproduces the paper's leg pattern: revenue leg
                 negative and sniping leg positive (both intervals excluding
                 zero) in >= 6 of the 8 decomposed tape-band cells.

Choices P7 leaves open, each stated once here:
  H7a  The cancelling maker is the operating point's default (fixed BCS
       half-spread, cancel-and-requote on every update). Its non-cancelling
       twin (NonCancellingMaker) is the same maker ignoring jump-flagged
       updates: it requotes at the next 10-ms update, so a sniper always beats
       it. Snipe size = quote, so one snipe clears the stale side and the loss
       per snipe-able jump is the maker's whole stale exposure. Grid: P2's
       k x lag/eps (the formula's own). A jump is snipe-able when the snipers
       fired at it (race log); loss = minus the maker's edge on sniper fills.
  H7b  inventory_skew = (delta/b) x b / 6,180 for delta/b in {0.1, 0.3, 0.5}:
       the labels hold at one full quote of inventory, and the realised skews
       (logged by ShiftLogMaker at every observed jump) set the factor tested.
       k = 3, lag 0, snipe = quote.
  H7c  Three levels spaced round(b/2) ticks apart, 6,180 lots each, k = 3
       sweeping snipers (SweepSniper: one IOC limited at the last price still
       worth more than its fee, taking every stale level; EventSniper only
       hits the touch, and BCS eq. 4's snipers take all stale depth). A maker
       fill is a snipe when a sniper traded at that nanosecond; LadderMaker
       records each order's level.
  H7d  A-S with a 1-s rolling horizon (MakerPolicy's), sigma^2 the path's
       total variance rate (diffusion + jumps) and gamma such that one full
       quote of inventory moves the reservation price by b; its half_spread is
       solved so that its base half-spread equals the paper maker's. Both run
       at the paper maker's 300-ms one-way latency (P7 restates none). The
       P&L comparison pools the paper-band cells per seed.
  H7e  The 8 cells are exp7c's: paper-band ratios x k in {1, 3, 7, 21}. The
       fading maker pulls every quote for 100 ms (one paper tick) after a jump
       or an adverse fill, at 300 ms one way, no widening. Legs as exp7c: the
       maker's edge against V at fill time over non-adverse (edge >= 0) and
       adverse (edge < 0) fills; with-HFT minus no-HFT per seed.
The corrected tape band is reported beside the paper band (H7d, H7e).

Run (from bcs_research/):
    python experiments/exp20_maker_strategies.py --confirmatory
    python experiments/exp20_maker_strategies.py --seeds 2 --out /tmp/exp20.json   # smoke
"""
from __future__ import annotations

import dataclasses
import math
import sys
import time
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import _confirm_common as cc  # noqa: E402
import operating_point as op  # noqa: E402

import numpy as np  # noqa: E402

import bcs_engine as be  # noqa: E402
from bcs_theory import race_win_probability  # noqa: E402
from contracts import Order  # noqa: E402
from event_sniper import EventSniper  # noqa: E402
from exp16_race_incidence import K_RACE, LAG_FRACS  # noqa: E402
from strategic_maker import StrategicMaker  # noqa: E402

NAME = "exp20_maker_strategies"
B_TICKS = op.PATH["jump_scale_bp"] * op.BP
RACE_RATIO = 1.0
H7A_BAND = 0.03
SKEW_TARGETS = (0.1, 0.3, 0.5)
H7B_BAND = 0.05
LADDER_LEVELS, LADDER_SPACING = 3, int(round(B_TICKS / 2))
H7C_BAND = 0.15
H7D_HIT_MAX, H7D_MISS_MIN = 2, 6
H7E_K = (1, 3, 7, 21)
H7E_MIN_CELLS = 6
FADE_NS = cc.PAPER_TICK_NS
AS_TAU_NS = 1_000_000_000
SIGMA2_TICKS_PER_S = ((op.PATH["sigma_bp_per_sqrt_s"] ** 2
                       + op.PATH["jump_intensity_per_s"] * 2 * op.PATH["jump_scale_bp"] ** 2)
                      * op.BP ** 2)
AS_GAMMA = B_TICKS / (op.QUOTE_LOTS * SIGMA2_TICKS_PER_S * AS_TAU_NS / 1e9)


# --- policies and agents ---------------------------------------------------------

def as_policy():
    """A-S whose base half-spread equals the paper maker's (equal base spread)."""
    g, base = AS_GAMMA, cc.paper_policy().half_spread
    x = base - g * SIGMA2_TICKS_PER_S * (AS_TAU_NS / 1e9) / 2   # = ln(1 + g h) / g
    return dataclasses.replace(op.default_policy(), as_gamma=g, as_sigma2_per_s=SIGMA2_TICKS_PER_S,
                               as_horizon_ns=AS_TAU_NS, half_spread=math.expm1(g * x) / g)


def fade_policy():
    return op.default_policy(fade_ns=FADE_NS, fade_widen=None)


STRATEGIES = {"paper": cc.paper_policy, "as": as_policy, "fade": fade_policy}


class NonCancellingMaker(StrategicMaker):
    """H7a's twin: identical, except it never reacts to a jump-flagged update."""

    def on_signal(self, t_ns, update, view):
        return [] if update.is_jump else super().on_signal(t_ns, update, view)


class ShiftLogMaker(StrategicMaker):
    """Logs, at each observed jump, how far its live quotes' centre sits below
    the signal it quoted around: (skew + A-S shift) x inventory at its last requote."""

    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.shift, self.jump_shifts = 0.0, []

    def _requote(self, t_ns):
        self.shift = (self.policy.inventory_skew + self._as_shift) * self.inventory
        return super()._requote(t_ns)

    def on_signal(self, t_ns, update, view):
        if update.is_jump:
            self.jump_shifts.append(self.shift)
        return super().on_signal(t_ns, update, view)


class LadderMaker(StrategicMaker):
    """Records each order's ladder level and every fill (t, order id, qty, price, is_buyer)."""

    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.level_of, self.fills = {}, []

    def _requote(self, t_ns):
        out = super()._requote(t_ns)
        seen = {}
        for oid, q in self._live.items():            # insertion order: level by level
            self.level_of[oid] = seen.get(q.side, 0)
            seen[q.side] = self.level_of[oid] + 1
        return out

    def on_fill(self, t_ns, fill, view):
        self.fills.append((fill.t_ns, fill.order_id, fill.quantity, fill.price, fill.is_buyer))
        return super().on_fill(t_ns, fill, view)


class SweepSniper(EventSniper):
    """Takes every stale level: one IOC limited at the last price still worth
    more than its fee, for the whole ladder's depth."""

    def on_signal(self, t_ns, update, view):
        ask, bid = view.best_ask(), view.best_bid()
        if ask > 0 and update.value - ask > self.threshold:
            return self._sweep(be.Side.Buy, math.ceil(update.value - self.threshold) - 1)
        if bid > 0 and bid - update.value > self.threshold:
            return self._sweep(be.Side.Sell, math.floor(update.value + self.threshold) + 1)
        return []

    def _sweep(self, side, price):
        self._oid += 1
        return [Order(self._oid, side, int(price), self.qty, be.OrderType.IOC)]


def _swap(mk, maker_cls=None, sniper_cls=None, sniper_qty=None):
    """The same market with its maker and/or snipers rebuilt as subclasses."""
    makers, snipers = mk.makers, mk.snipers
    if maker_cls:
        makers = [maker_cls(m.participant_id, m.policy, path=mk.path,
                            md_latency_ns=m.md_latency_ns, order_latency_ns=m.order_latency_ns)
                  for m in makers]
    if sniper_cls:
        snipers = [sniper_cls(s.participant_id, sniper_qty or s.qty, threshold=s.threshold,
                              md_latency_ns=s.md_latency_ns, order_latency_ns=s.order_latency_ns)
                   for s in snipers]
    return mk._replace(agents=makers + snipers + [mk.noise], makers=makers, snipers=snipers)


def sniping_loss(res) -> float:
    """What snipers took from the makers: minus the makers' edge on sniper fills, USD."""
    loss = 0.0
    for tr in res.trades:
        b, s = cc.class_of(tr.buyer_id), cc.class_of(tr.seller_id)
        if {b, s} == {"maker", "sniper"}:
            e = (res.path.value_at(tr.t_ns) - tr.price) * tr.quantity / be.PRICE_PRECISION
            loss -= e if b == "maker" else -e
    return loss


def maker_legs(res) -> tuple[float, float]:
    """exp7c's legs: the makers' edge on non-adverse (>= 0) and adverse (< 0) fills."""
    rev = snipe = 0.0
    for tr in res.trades:
        for pid, sign in ((tr.buyer_id, 1), (tr.seller_id, -1)):
            if cc.class_of(pid) == "maker":
                e = sign * (res.path.value_at(tr.t_ns) - tr.price) * tr.quantity / be.PRICE_PRECISION
                if e >= 0:
                    rev += e
                else:
                    snipe += e
    return rev, snipe


# --- runs ---------------------------------------------------------------------------

def h7a_run(kind: str, k: int, lag_frac: float, seed: int, horizon_s: float = op.HORIZON_S) -> dict:
    lag = int(round(lag_frac * op.JITTER_NS))
    mk = op.build_market(seed, maker_latencies_ns=(op.MAKER_ONE_WAY_NS + lag // 2,), k=k,
                         snipe_ratio=RACE_RATIO, horizon_s=horizon_s)
    if kind == "non_cancelling":
        mk = _swap(mk, NonCancellingMaker)
    res, _ = cc.simulate(mk, seed)
    snipers = {s.participant_id for s in mk.snipers}
    snipeable = sum(any(a.participant_id in snipers for a in r.arrivals) for r in res.races)
    return {"snipeable": snipeable, "loss_usd": sniping_loss(res), "n_jumps": len(res.races)}


def h7b_run(target: float, seed: int, horizon_s: float = op.HORIZON_S) -> dict:
    skew = target * B_TICKS / op.QUOTE_LOTS
    mk = op.build_market(seed, k=3, snipe_ratio=RACE_RATIO, horizon_s=horizon_s,
                         policies=[op.default_policy(inventory_skew=skew)])
    mk = _swap(mk, ShiftLogMaker)
    res, _ = cc.simulate(mk, seed)
    shifts = mk.makers[0].jump_shifts
    return {"loss_usd": sniping_loss(res), "n_jumps": len(res.races),
            "cosh_sum": float(sum(math.cosh(d / B_TICKS) for d in shifts)),
            "n_shifts": len(shifts),
            "rms_shift_over_b": float(np.sqrt(np.mean(np.square(shifts))) / B_TICKS) if shifts else 0.0}


def h7c_run(seed: int, horizon_s: float = op.HORIZON_S) -> dict:
    policy = op.default_policy(n_levels=LADDER_LEVELS, level_spacing=LADDER_SPACING)
    mk = op.build_market(seed, k=3, horizon_s=horizon_s, policies=[policy])
    mk = _swap(mk, LadderMaker, SweepSniper, sniper_qty=LADDER_LEVELS * op.QUOTE_LOTS)
    res, _ = cc.simulate(mk, seed)
    snipe_t = {tr.t_ns for tr in res.trades
               if "sniper" in (cc.class_of(tr.buyer_id), cc.class_of(tr.seller_id))}
    m = mk.makers[0]
    loss = [0.0] * LADDER_LEVELS
    for t, oid, qty, price, is_buyer in m.fills:
        if t in snipe_t:
            e = (res.path.value_at(t) - price) * qty / be.PRICE_PRECISION
            loss[m.level_of[oid]] -= e if is_buyer else -e
    return {"loss_by_level_usd": loss, "n_jumps": len(res.races)}


def strategy_run(strategy: str, k: int, ratio: float, seed: int,
                 horizon_s: float = op.HORIZON_S) -> dict:
    mk = op.build_market(seed, maker_latencies_ns=(cc.PAPER_LATENCY_NS,), k=k, snipe_ratio=ratio,
                         policies=[STRATEGIES[strategy]()], horizon_s=horizon_s)
    res, _ = cc.simulate(mk, seed)
    rev, snipe = maker_legs(res)
    return {**cc.legs(res), "revenue_leg_usd": rev, "sniping_leg_usd": snipe}


def tasks(seeds, horizon_s: float = op.HORIZON_S) -> dict:
    t = {}
    for k in sorted(K_RACE, reverse=True):
        for lf in LAG_FRACS:
            for kind in ("cancelling", "non_cancelling"):
                for s in seeds:
                    t[cc.key("h7a", kind, k, lf, s)] = (h7a_run, kind, k, lf, s, horizon_s)
    for band, ratio, k in sorted(cc.band_cells(), key=lambda c: -c[2]):
        for strat in ("as", "paper") + (("fade",) if k in H7E_K else ()):
            for s in seeds:
                t[cc.key("strat", strat, band, ratio, k, s)] = (strategy_run, strat, k, ratio, s,
                                                                horizon_s)
    for strat in STRATEGIES:
        for s in seeds:
            t[cc.key("strat", strat, "base", s)] = (strategy_run, strat, 0, op.PAPER_BAND[0], s,
                                                    horizon_s)
    for target in (0.0, *SKEW_TARGETS):
        for s in seeds:
            t[cc.key("h7b", target, s)] = (h7b_run, target, s, horizon_s)
    for s in seeds:
        t[cc.key("h7c", s)] = (h7c_run, s, horizon_s)
    return t


# --- statistics and verdicts (pure) ---------------------------------------------------

def per_jump_ratio_ci(loss_a, n_a, loss_b, n_b) -> dict:
    """(sum loss_a / sum n_a) / (sum loss_b / sum n_b), seeds resampled jointly."""
    arr = [np.asarray(x, float) for x in (loss_a, n_a, loss_b, n_b)]

    def stat(i):
        la, na, lb, nb = (x[i].sum() for x in arr)
        return (la / na) / (lb / nb) if na and nb and lb else math.nan

    n = len(arr[0])
    rng = np.random.default_rng(op.BOOT_SEED)
    boots = np.array([stat(rng.integers(0, n, n)) for _ in range(op.N_BOOTSTRAP)])
    boots = boots[np.isfinite(boots)]
    lo, hi = np.percentile(boots, [2.5, 97.5]) if boots.size else (math.nan, math.nan)
    return {"mean": float(stat(np.arange(n))), "lower": float(lo), "upper": float(hi), "n": n}


def h7a(cells: dict) -> dict:
    """cells: {(k, lag_frac): (loss_a, n_a, loss_b, n_b) per-seed arrays}."""
    out = {}
    for (k, lf), cols in sorted(cells.items()):
        th = 1 - race_win_probability(lf * op.JITTER_NS, op.JITTER_NS, k)
        out[f"k={k},lag/eps={lf}"] = cc.judged(per_jump_ratio_ci(*cols), th, th - H7A_BAND,
                                               th + H7A_BAND)
    return {"cells": out, "verdict": cc.combine(v["verdict"] for v in out.values())}


def h7b_cell(ratio: dict, factor: float) -> dict:
    v = cc.judged(ratio, factor, factor * (1 - H7B_BAND), factor * (1 + H7B_BAND))
    if ratio["upper"] < 1:
        v["verdict"] = "MISS"                         # skew reduced sniping loss
    return v


def h7d_verdict(count: int, pnl_diff: dict) -> str:
    if count >= H7D_MISS_MIN or pnl_diff["lower"] >= 0:
        return "MISS"
    return "HIT" if count <= H7D_HIT_MAX and pnl_diff["upper"] < 0 else "INCONCLUSIVE"


def h7e_cell(rev: dict, snipe: dict) -> bool:
    return rev["upper"] < 0 and snipe["lower"] > 0


def h7e_verdict(n_pattern: int) -> str:
    return "HIT" if n_pattern >= H7E_MIN_CELLS else "MISS"


# --- summary --------------------------------------------------------------------------

def summarize(rows: dict, seeds) -> dict:
    def get(*parts):
        return [rows[cc.key(*parts, s)] for s in seeds]

    cells = {}
    for k in K_RACE:
        for lf in LAG_FRACS:
            a, b = get("h7a", "cancelling", k, lf), get("h7a", "non_cancelling", k, lf)
            cells[(k, lf)] = ([r["loss_usd"] for r in a], [r["snipeable"] for r in a],
                              [r["loss_usd"] for r in b], [r["snipeable"] for r in b])
    out = {"H7a": {**h7a(cells), "primary": True}}

    base0 = get("h7b", 0.0)
    h7b = {}
    for target in SKEW_TARGETS:
        rs = get("h7b", target)
        factor = sum(r["cosh_sum"] for r in rs) / max(1, sum(r["n_shifts"] for r in rs))
        ratio = cc.ratio_ci([r["loss_usd"] for r in rs], [r["loss_usd"] for r in base0])
        h7b[str(target)] = {**h7b_cell(ratio, factor), "rms_shift_over_b": float(
            np.mean([r["rms_shift_over_b"] for r in rs]))}
    out["H7b"] = {"by_skew": h7b, "verdict": cc.combine(v["verdict"] for v in h7b.values())}

    lad = get("h7c")
    levels = {}
    for j in range(1, LADDER_LEVELS):
        th = math.exp(-j * LADDER_SPACING / B_TICKS)
        levels[f"level {j + 1} / level 1"] = cc.judged(
            cc.ratio_ci([r["loss_by_level_usd"][j] for r in lad],
                        [r["loss_by_level_usd"][0] for r in lad]),
            th, th * (1 - H7C_BAND), th * (1 + H7C_BAND))
    out["H7c"] = {"levels": levels, "spacing_ticks": LADDER_SPACING,
                  "verdict": cc.combine(v["verdict"] for v in levels.values())}

    band = cc.band_cells()
    strat_cells = {s: {c: get("strat", s, *c) for c in band} for s in ("as", "paper")}
    counts = {s: cc.band_counts(strat_cells[s], get("strat", s, "base")) for s in strat_cells}
    h7d = {}
    for b in op.BANDS:
        keys = [c for c in band if c[0] == b]
        diff = cc.boot([np.mean([cc.net(strat_cells["as"][c][i], "maker")
                                 - cc.net(strat_cells["paper"][c][i], "maker") for c in keys])
                        for i in range(len(seeds))])
        n = counts["as"][b]["count"]
        h7d[b] = {"as_count": n, "paper_count": counts["paper"][b]["count"],
                  "as_minus_paper_pnl_with_hft_usd": diff, "verdict": h7d_verdict(n, diff)}
    out["H7d"] = {"verdict": h7d["paper"]["verdict"], "verdict_band": "paper", "by_band": h7d,
                  "cells": counts, "as_policy": repr(as_policy())}

    fbase = get("strat", "fade", "base")
    h7e = {}
    for b, ratios in op.BANDS.items():
        cs = []
        for r in ratios:
            for k in H7E_K:
                rs = get("strat", "fade", b, r, k)
                rev = cc.boot([x["revenue_leg_usd"] - y["revenue_leg_usd"] for x, y in zip(rs, fbase)])
                snp = cc.boot([x["sniping_leg_usd"] - y["sniping_leg_usd"] for x, y in zip(rs, fbase)])
                cs.append({"ratio": r, "k": k, "revenue_leg_delta": rev,
                           "sniping_leg_delta": snp, "pattern": h7e_cell(rev, snp)})
        n = sum(c["pattern"] for c in cs)
        h7e[b] = {"cells_with_pattern": n, "n_cells": len(cs), "cells": cs,
                  "verdict": h7e_verdict(n)}
    out["H7e"] = {"verdict": h7e["paper"]["verdict"], "verdict_band": "paper", "by_band": h7e}
    return out


def main(argv=None) -> dict:
    a = cc.parse_args(NAME, __doc__, argv)
    t0 = time.time()
    rows = cc.run_pending(tasks(a.seed_list), cc.call, a.checkpoint, a.workers, NAME)
    report = {"experiment": NAME, "prediction": "P7", "status": a.status,
              "seeds": a.seed_list, "horizon_s": op.HORIZON_S, "n_runs": len(rows),
              "config": {"k_race": K_RACE, "lag_over_eps": LAG_FRACS, "race_snipe_ratio": RACE_RATIO,
                         "skew_targets_delta_over_b_at_one_quote": SKEW_TARGETS,
                         "ladder": {"levels": LADDER_LEVELS, "spacing_ticks": LADDER_SPACING},
                         "as": {"gamma": AS_GAMMA, "sigma2_ticks2_per_s": SIGMA2_TICKS_PER_S,
                                "tau_ns": AS_TAU_NS},
                         "fade": {"fade_ns": FADE_NS, "fade_widen": None},
                         "strategy_maker_one_way_ns": cc.PAPER_LATENCY_NS,
                         "b_ticks": B_TICKS, "bands": op.BANDS, "k_grid": op.K_GRID,
                         "h7e_k": H7E_K},
              "units": "losses and P&L in USD per run; maker P&L net of the operating point's fees",
              **summarize(rows, a.seed_list), "wall_s": round(time.time() - t0, 1)}
    cc.write_report(a.out, report)
    print({h: report[h]["verdict"] for h in ("H7a", "H7b", "H7c", "H7d", "H7e")}, flush=True)
    return report


if __name__ == "__main__":
    main()
