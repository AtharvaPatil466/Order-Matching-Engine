#!/usr/bin/env python3
"""Liquidity gaps: find the channel behind their growth in the HFT count, or retire it.

Why this exists. Paper S1.2: in the tick model, gaps rise linearly in the
number of HFTs k (2.84 at k = 0 to 47.47 at k = 21, slope 2.13), with the
thinning rule ruled out and HFT fills flat. A reviewer asked that the channel
be identified or the result deleted. The batch lane already traced one gap
spike to the sampling instant (paper/batch_interval1_note.md), so the
sampling instant is tested first.

Mechanism (read off scheduler.py before any targeted run; the commit that adds
this file precedes the results). The tick scheduler snapshots the book once
per tick, after everything due that tick. Inside tick s the order is:
  1. last tick's HFT IOCs, stamped s - 1000 + j for jitter j >= 1;
  2. the maker's cancel and requote (stamped s, queued three ticks earlier);
  3. this tick's noise IOCs and any HFT IOC of this tick with jitter j = 0.
An HFT stamps its IOC t + int(U(0, 100)) us, so it runs in its decision tick
only when j = 0 (p = 1/100). Otherwise it runs at the start of the next tick.
Either way it takes the same quote at the same limit. But a side emptied at
the start of a tick is refilled by that tick's requote before the snapshot,
while a side emptied by a j = 0 IOC after the requote stays empty until the
next one, and the snapshot records it. So a snapshot shows a snipe-made gap
only when some sniper drew j = 0: per snipe-able tick 1 - 0.99^k, about k/100,
which is linear in k. One sniper wins each opportunity whatever k is, so
fills stay flat. On the submission clock every successful snipe empties a side
for about one tick, whatever k is.

Predictions, fixed before the targeted runs:
  T1 (attribution). The k-growth is made of one-sided snapshots emptied by a
     zero-jitter HFT IOC. Their count tracks 1 - 0.99^k; noise- and
     maker-made ones stay flat in k.
  T2 (causal). Stamp every HFT IOC 1 us later (jitter U{1..100}). The draws
     and the race order are unchanged, so the economics are too, but no IOC
     runs in its decision tick. The slope of gaps on k (k >= 1) collapses
     from about 2.1 to a 95% interval inside [-0.2, 0.2]. HFT rent and maker
     P&L move by less than 5% of their level at every k.
  T3 (duration). On the submission clock, one-sided episodes and one-sided
     time are flat in k: episodes(21)/episodes(1) and time(21)/time(1) both
     lie in [0.8, 1.25].
  H2c (event model, PREREGISTRATION P2). Gaps do not grow linearly in k:
     gaps(21)/gaps(3) < 2 at lag 0 and at lag > eps, read both from the
     section 3.3 detector on periodic snapshots and from exact one-sided time.
     MISS if the ratio is >= 3.

Event-model gaps are measured exactly: `TracedEventScheduler` logs the top of
book after every engine arrival, and `one_sided_episodes` turns that log into
maximal one-sided intervals of positive length. Periodic snapshots sample
at fixed phases of the 10 ms update grid, so their counts depend on where
the samples fall, the same trap as the tick model's.

Run (from bcs_research/):
    .venv/bin/python experiments/gap_channel.py tick  [--seeds 100]
    .venv/bin/python experiments/gap_channel.py event [--seeds 100]
Writes results/pilot/gap_channel_{tick,event}.json.
"""
from __future__ import annotations

import argparse
import heapq
import json
import os
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np

_HERE = Path(__file__).resolve().parent
_ROOT = _HERE.parent
for _d in ("build", "agents", "simulation", "metrics", "experiments", "analysis"):
    _p = str(_ROOT / _d)
    if _p not in sys.path:
        sys.path.insert(0, _p)

import bcs_engine as be                                     # noqa: E402
from bcs_market_maker import BCSMarketMaker                 # noqa: E402
from bootstrap import bootstrap_ci                          # noqa: E402
from event_scheduler import EventScheduler                  # noqa: E402
from exp1_primary import CFG as TICK_CFG                    # noqa: E402
from fundamental_value import FundamentalValueProcess       # noqa: E402
from hft_agent import HFTAgent                              # noqa: E402
from liquidity_gap_detector import count_liquidity_gaps     # noqa: E402
from noise_trader import NoiseTrader                        # noqa: E402
from scheduler import LatencyScheduler                      # noqa: E402
from welfare_decomposition import decompose_welfare, mark_price  # noqa: E402

TICK_K = (0, 1, 2, 3, 5, 8, 13, 21)          # exp3_hft_count.N_HFTS_GRID
EVENT_K = (1, 3, 8, 21)                      # P2's grid
EVENT_LAGS = ("lag0", "slow")
TICK_ARMS = ("published", "shifted")
EVENT_STATUS = "pilot — pre-sign-off, placeholder parameters (exp11 CFG); not the confirmatory test of P2 H2c"
MM, NT0, HFT0 = 1, 100, 200


# --- the metric -------------------------------------------------------------

def one_sided_episodes(states, end_t):
    """Maximal intervals of positive length on which the book is not two-sided.

    `states` are (t, best_bid, best_ask) after each book event, ascending in t;
    each holds until the next one's t (0 = empty side). Time before the book is
    first two-sided is not a gap: nothing had been quoted yet. An episode still
    open at end_t is cut there and flagged unrecovered. Returns a list of
    (start, end, recovered). A side emptied and refilled at the same instant
    has zero length and is not an episode.
    """
    out, start, quoted = [], None, False
    for t, bid, ask in states:
        if bid > 0 and ask > 0:
            quoted = True
            if start is not None and t > start:
                out.append((start, t, True))
            start = None
        elif quoted and start is None:
            start = t
    if start is not None and end_t > start:
        out.append((start, end_t, False))
    return out


def episode_stats(states, end_t, horizon):
    """Recovered one-sided episodes: count and share of `horizon` they cover."""
    eps = [(s, e) for s, e, ok in one_sided_episodes(states, end_t) if ok]
    return len(eps), sum(e - s for s, e in eps) / horizon


# --- tick model -------------------------------------------------------------

class TracedScheduler(LatencyScheduler):
    """LatencyScheduler that also logs the book after every submission.

    `_submit_due` is the parent's loop plus one log line, so submissions, fills
    and end-of-tick snapshots are identical to the parent's (pinned by a test
    against exp1_primary._run). Log rows: (tick, submit_at, pid, kind, bid, ask).
    """

    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.log = []

    def _submit_due(self, t):
        while self._pending and self._pending[0][0] <= t:
            submit_at, _, a = heapq.heappop(self._pending)
            if a.kind == "cancel":
                self._h.submit_cancel(self._sym, a.cancel_order_id)
            else:
                tif = a.tif if a.tif is not None else be.TimeInForce.GTC
                self._h.submit_order(self._sym, a.order_id, a.participant_id,
                                     a.side, a.price, a.quantity, a.order_type, tif)
            for tr in self._h.drain_trades(self._sym):
                self._route_fill(t, tr)
            self.log.append((t, submit_at, a.participant_id, a.kind,
                             self._h.best_bid(self._sym), self._h.best_ask(self._sym)))


class ShiftedJitterHFT(HFTAgent):
    """HFTAgent whose IOC lands 1 us later: jitter U{1..100} instead of U{0..99}.

    Same RNG draws and the same order among snipers, so the race is unchanged;
    only no IOC can be processed in the tick it was decided in (T2).
    """

    def _jitter(self):
        return super()._jitter() + 1


def _causes_at_tick_end(log, hft_ids):
    """For each tick, who emptied the side if the book ends that tick one-sided.

    The culprit is the submission that last took the book from two-sided to
    not; "carried" means the book was already one-sided when the tick began.
    """
    out, culprit, two = {}, None, False
    for t, submit_at, pid, kind, bid, ask in log:
        now_two = bid > 0 and ask > 0
        if two and not now_two:
            culprit = (t, submit_at, pid)
        two = now_two
        if now_two:
            out[t] = None
            continue
        if culprit is None or culprit[0] != t:
            out[t] = "carried"
        elif culprit[2] in hft_ids:
            out[t] = "hft_zero_jitter" if culprit[1] == t else "hft_late"
        else:
            out[t] = "noise" if culprit[2] >= NT0 else "maker"
    return out


def tick_run(seed, k, arm="published", cfg=None):
    """exp1_primary._run's construction (single maker), traced. Returns one row."""
    cfg = {**TICK_CFG, **(cfg or {})}
    h = be.EngineHarness()
    h.add_symbol(1)
    h.start()
    fund = FundamentalValueProcess(v0=100 * be.PRICE_PRECISION, sigma=cfg["sigma"],
                                   dt_us=cfg["dt_us"], seed=seed)
    mm = BCSMarketMaker(MM, base_half_spread=cfg["base_half_spread"], quote_qty=cfg["quote_qty"],
                        latency_us=cfg["mm_latency_us"], inventory_skew=cfg["mm_inventory_skew"],
                        adverse_sensitivity=cfg["adverse_sensitivity"],
                        spread_decay=cfg["spread_decay"], spread_cap_mult=cfg["spread_cap_mult"],
                        min_quote_qty=cfg["min_quote_qty"],
                        thin_liquidity=cfg.get("thin_liquidity", True))
    noise = [NoiseTrader(NT0 + i, lambda_per_tick=cfg["lambda_per_tick"], qty=cfg["noise_qty"],
                         seed=seed * 1000 + i, base_half_spread=cfg["base_half_spread"])
             for i in range(cfg["n_noise"])]
    hft_cls = ShiftedJitterHFT if arm == "shifted" else HFTAgent
    hfts = [hft_cls(HFT0 + j, latency_us=cfg["hft_latency_us"],
                    race_noise_us=cfg["hft_race_noise_us"],
                    transaction_cost_bps=cfg["hft_cost_bps"], qty=cfg["hft_qty"],
                    seed=seed * 2000 + j) for j in range(k)]
    sched = TracedScheduler(h, 1, cfg["dt_us"])
    res = sched.run([mm] + noise + hfts, fund, cfg["duration_us"])
    h.stop()

    hft_ids = {a.participant_id for a in hfts}
    welfare = decompose_welfare(res, [MM], [a.participant_id for a in noise], sorted(hft_ids),
                                mark=mark_price(res))
    end_cause = _causes_at_tick_end(sched.log, hft_ids)
    causes = dict.fromkeys(("hft_zero_jitter", "hft_late", "noise", "maker", "carried"), 0)
    for s in res.snapshots:
        if not (s["best_bid"] > 0 and s["best_ask"] > 0):
            causes[end_cause.get(s["t"], "carried")] += 1
    n_ep, share = episode_stats([(r[1], r[4], r[5]) for r in sched.log],
                                cfg["duration_us"], cfg["duration_us"])
    return {
        "seed": seed, "k": k, "arm": arm,
        "gaps": count_liquidity_gaps(res.snapshots, cfg["dt_us"], cfg["sigma"]),
        "one_sided_snapshots": sum(causes.values()),
        **{f"cause_{c}": n for c, n in causes.items()},
        "episodes_submit_clock": n_ep,
        "one_sided_share_submit_clock": share,
        "hft_rent": welfare["hft_rent"],
        "mm_pnl": welfare["mm_pnl"],
        "hft_fills": sum(a.fills for a in hfts),
        "zero_jitter_iocs": sum(1 for r in sched.log
                                if r[2] in hft_ids and r[1] == r[0]),
    }


# --- event model ------------------------------------------------------------

class TracedEventScheduler(EventScheduler):
    """EventScheduler that logs (t, best_bid, best_ask) after every engine arrival.

    A burst is applied atomically, so one row per burst is the book as any
    other participant could see it.
    """

    def run(self, agents, path):
        self.book_log = []
        return super().run(agents, path)

    def _arrive(self, t, *args, **kw):
        super()._arrive(t, *args, **kw)
        self.book_log.append((t, self._h.best_bid(self._sym), self._h.best_ask(self._sym)))


def event_cfg(lag):
    """exp11's placeholder operating point; lag0 makes the maker as fast as the snipers."""
    import exp11_batch_semantics as x11
    cfg = dict(x11.CFG)
    if lag == "lag0":
        cfg["maker_latency_ns"] = cfg["sniper_latency_ns"]
    return cfg


def snapshot_gaps(res, cfg):
    """The section 3.3 detector on the event model's periodic snapshots.

    ponytail: the path is pure-jump (sigma_step = 0), so "V flat across the
    episode" means "no jump inside it"; a vanishing sigma makes the detector's
    threshold exactly that. A diffusive path would pass its per-snapshot sigma.
    """
    snaps = [{"t": s.t_ns, "best_bid": s.best_bid, "best_ask": s.best_ask,
              "v": res.path.value_at(s.t_ns)} for s in res.snapshots]
    dt_us = cfg["snapshot_every_ns"] // 1000
    return count_liquidity_gaps(snaps, dt_us, 1e-9, max_gap_us=50_000)


def event_run(seed, k, lag, **overrides):
    import exp11_batch_semantics as x11
    cfg = {**event_cfg(lag), "n_snipers": k, **overrides}
    agents = x11.make_agents(cfg, seed, "stale")
    path = x11.make_path(cfg, seed)
    h = be.EngineHarness()
    h.add_symbol(1)
    h.start(disable_circuit_breaker=True)
    sched = TracedEventScheduler(h, 1, seed=seed, snapshot_every_ns=cfg["snapshot_every_ns"],
                                 race_jitter_ns={a.participant_id: cfg["jitter_ns"] for a in agents})
    res = sched.run(agents, path)
    h.stop()
    n_ep, share = episode_stats(sched.book_log, res.duration_ns, res.duration_ns)
    one_sided = sum(1 for s in res.snapshots if not (s.best_bid > 0 and s.best_ask > 0))
    edge = x11.edges(res, x11.classes(agents))
    return {
        "seed": seed, "k": k, "lag": lag,
        "gaps_snapshot_detector": snapshot_gaps(res, cfg),
        "one_sided_snapshot_share": one_sided / len(res.snapshots),
        "episodes_exact": n_ep,
        "one_sided_share_exact": share,
        "maker_edge": edge["maker"], "sniper_edge": edge["sniper"],
    }


# --- summaries --------------------------------------------------------------

def _ratio_ci(num, den, boot_seed=0, n_boot=2000):
    """Ratio of means over matched seeds, percentile bootstrap over seeds."""
    num, den = np.asarray(num, float), np.asarray(den, float)
    rng = np.random.default_rng(boot_seed)
    idx = rng.integers(0, len(num), size=(n_boot, len(num)))
    boots = num[idx].mean(1) / den[idx].mean(1)
    return {"ratio": float(num.mean() / den.mean()),
            "lower": float(np.percentile(boots, 2.5)), "upper": float(np.percentile(boots, 97.5))}


def _slope_ci(ks, cols, boot_seed=0, n_boot=2000):
    """OLS slope of the cell mean on k; seeds resampled jointly (common random numbers)."""
    m = np.asarray(cols, float)                      # (len(ks), n_seeds)
    rng = np.random.default_rng(boot_seed)
    idx = rng.integers(0, m.shape[1], size=(n_boot, m.shape[1]))
    boots = [np.polyfit(ks, m[:, i].mean(1), 1)[0] for i in idx]
    return {"slope": float(np.polyfit(ks, m.mean(1), 1)[0]),
            "lower": float(np.percentile(boots, 2.5)), "upper": float(np.percentile(boots, 97.5))}


def summarize_tick(rows, seeds):
    cell = {(r["arm"], r["k"], r["seed"]): r for r in rows}

    def col(arm, k, key):
        return [cell[(arm, k, s)][key] for s in seeds]

    keys = ("gaps", "one_sided_snapshots", "cause_hft_zero_jitter", "cause_hft_late",
            "cause_noise", "cause_maker", "cause_carried", "episodes_submit_clock",
            "one_sided_share_submit_clock", "hft_rent", "mm_pnl", "hft_fills", "zero_jitter_iocs")
    out = {"cells": {arm: {str(k): {key: bootstrap_ci(col(arm, k, key)) for key in keys}
                           for k in TICK_K} for arm in TICK_ARMS}}
    ks = [k for k in TICK_K if k >= 1]
    out["gap_slope_k_ge_1"] = {arm: _slope_ci(ks, [col(arm, k, "gaps") for k in ks])
                              for arm in TICK_ARMS}
    out["shift_effect"] = {str(k): {key: bootstrap_ci([b - a for a, b in zip(
        col("published", k, key), col("shifted", k, key))]) for key in ("hft_rent", "mm_pnl", "gaps")}
        for k in TICK_K}
    out["submit_clock_ratio_21_over_1"] = {
        arm: {key: _ratio_ci(col(arm, 21, key), col(arm, 1, key))
              for key in ("episodes_submit_clock", "one_sided_share_submit_clock")}
        for arm in TICK_ARMS}
    # T1: excess gaps over k = 0 against the zero-jitter law 1 - 0.99^k.
    base = np.mean(col("published", 0, "gaps"))
    excess = [np.mean(col("published", k, "gaps")) - base for k in ks]
    law = [1 - 0.99 ** k for k in ks]
    out["t1_excess_vs_zero_jitter_law"] = {
        "k": ks, "excess_gaps": excess, "law": law,
        "corr": float(np.corrcoef(excess, law)[0, 1]),
        "scale": float(np.dot(excess, law) / np.dot(law, law)),
    }
    return out


def verdicts_tick(s):
    sl = s["gap_slope_k_ge_1"]["shifted"]
    t2_slope = -0.2 <= sl["lower"] and sl["upper"] <= 0.2
    econ = []
    for k in TICK_K[1:]:
        for key in ("hft_rent", "mm_pnl"):
            level = abs(s["cells"]["published"][str(k)][key]["mean"])
            d = s["shift_effect"][str(k)][key]
            econ.append(max(abs(d["lower"]), abs(d["upper"])) < 0.05 * level)
    r = s["submit_clock_ratio_21_over_1"]["published"]
    t3 = all(0.8 <= r[key]["lower"] and r[key]["upper"] <= 1.25 for key in r)
    return {"T2_slope_collapses": t2_slope, "T2_economics_within_5pct": all(econ),
            "T3_submit_clock_flat": t3}


def summarize_event(rows, seeds):
    cell = {(r["lag"], r["k"], r["seed"]): r for r in rows}

    def col(lag, k, key):
        return [cell[(lag, k, s)][key] for s in seeds]

    keys = ("gaps_snapshot_detector", "one_sided_snapshot_share", "episodes_exact",
            "one_sided_share_exact", "maker_edge", "sniper_edge")
    out = {"cells": {lag: {str(k): {key: bootstrap_ci(col(lag, k, key)) for key in keys}
                           for k in EVENT_K} for lag in EVENT_LAGS}, "h2c": {}}
    for lag in EVENT_LAGS:
        out["h2c"][lag] = {}
        for key in ("gaps_snapshot_detector", "episodes_exact", "one_sided_share_exact"):
            r = _ratio_ci(col(lag, 21, key), col(lag, 3, key))
            r["verdict"] = ("HIT" if r["upper"] < 2 else "MISS" if r["lower"] >= 3
                            else "INCONCLUSIVE")
            out["h2c"][lag][key] = r
    return out


# --- driver -----------------------------------------------------------------

def _pool_map(fn, tasks):
    with ProcessPoolExecutor(max_workers=max(1, (os.cpu_count() or 2) - 1)) as pool:
        return list(pool.map(fn, *zip(*tasks), chunksize=4))


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("model", choices=("tick", "event"))
    ap.add_argument("--seeds", type=int, default=None)
    args = ap.parse_args(argv)
    t0 = time.time()
    out_dir = _ROOT / "results" / "pilot"
    out_dir.mkdir(parents=True, exist_ok=True)
    if args.model == "tick":
        seeds = list(range(1, (args.seeds or 100) + 1))     # exp3's seeds
        rows = _pool_map(tick_run, [(s, k, arm) for arm in TICK_ARMS for k in TICK_K for s in seeds])
        s = summarize_tick(rows, seeds)
        report = {"status": "tick model at the published exp3 operating point; seeds match exp3",
                  "config": TICK_CFG, "seeds": seeds, "summary": s, "verdicts": verdicts_tick(s),
                  "rows": rows}
    else:
        seeds = list(range(1, (args.seeds or 100) + 1))
        rows = _pool_map(event_run, [(s, k, lag) for lag in EVENT_LAGS for k in EVENT_K for s in seeds])
        report = {"status": EVENT_STATUS, "config": {lag: event_cfg(lag) for lag in EVENT_LAGS},
                  "seeds": seeds, "summary": summarize_event(rows, seeds), "rows": rows}
    report["elapsed_s"] = time.time() - t0
    out = out_dir / f"gap_channel_{args.model}.json"
    out.write_text(json.dumps(report, indent=2, sort_keys=True, default=float))
    print(json.dumps({k: v for k, v in report.items() if k in ("verdicts", "elapsed_s")}, indent=1))
    print(f"wrote {out.relative_to(_ROOT)}")
    return report


if __name__ == "__main__":
    main()
