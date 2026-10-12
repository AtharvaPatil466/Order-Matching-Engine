#!/usr/bin/env python3
"""Experiment 19 — heterogeneous investors with endogenous participation, plus a
slow informed investor (PREREGISTRATION P6).

Why this exists. With inelastic investors every spread the snipers cause is a
pure transfer. BCS footnote 37: with elastic demand, wider spreads also destroy
gains from trade. And BCS §VI.D: information slower than the quote-setter's is
worth nothing in a continuous book. Three predictions, as §6 states them:

  H6a (primary)  HFT presence lowers participation by the factor
                 e^(-(h_HFT - h_0)/vbar), h the realised mean quoted
                 half-spreads with and without HFTs (+-10%), and creates a
                 deadweight loss (forgone lam_i E[v 1{h_0 < v < h_HFT}]) whose
                 interval excludes zero. Miss: the ratio outside +-10%, or the
                 deadweight interval including zero.
  H6b            The slow informed investor's realised edge per trade, against V
                 at fill time, is not positive: upper 95% bound <= +0.05 h.
                 Miss: lower bound above zero.
  H6c            The single-maker inversion depends on the participation
                 elasticity e = h g(h) / (1 - G(h)) at the base spread: measured
                 e <= 1 gives >= 15 of 18 tape-band maker-gains cells; e >= 2
                 gives <= 9 of 18. Miss: the count on the wrong side.

Setup, as P6 states it: each investor draws a gain from trade v ~ Exp(mean
vbar) and trades only if the all-in half-spread it faces (touch minus the last
public signal, plus its taker fee) is below v (ExpGainInvestor). With an
exponential law e = h / vbar exactly, with h the ALL-IN base half-spread. P6
fixes the law but not vbar, so the experiment runs two: vbar = h_base / 0.5
and h_base / 4, h_base being the operating point's BCS half-spread plus the
1.7-bp taker fee (~108 ticks), which puts one arm in each branch of H6c. The
arrival rate in each arm makes the crossing rate the tape's 16.28/s at h_base,
as the operating point does for its own investors. The slow informed investor
is facts_event_model's (1 s information lag, 10 IOC children of a 6,182-lot
parent, 60 s between decisions); one second exceeds the maker's requote
latency (2 x 300 ms), as P6 requires. The maker is the paper's
(_confirm_common.paper_policy) at its 300-ms one-way latency, since the
inversion H6c conditions on is the paper maker's and P6 restates no latency.

Tests. H6a and H6b pool each vbar arm's paper-band cells per seed (one
interval per arm, seeds resampled jointly): measured participation ratio over
the predicted factor, inside [0.9, 1.1]; deadweight = gains from trade
realised without HFTs minus with, USD/s, above zero (an interval at or below
zero is the miss line's "including zero"). H6b's edge is per lot, in ticks,
over the baseline and paper-band runs, against 0.05 x their mean quoted
half-spread. H6c's branch is read off e's 95% interval (wholly <= 1, or >= 2);
an interval straddling a threshold makes that arm INCONCLUSIVE. Each
hypothesis combines its two arms (MISS if either misses, HIT if both hit). The
corrected tape band is reported beside the paper band.

Run (from bcs_research/):
    python experiments/exp19_noise_heterogeneity.py --confirmatory
    python experiments/exp19_noise_heterogeneity.py --seeds 2 --out /tmp/exp19.json   # smoke
"""
from __future__ import annotations

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
import facts_event_model as facts  # noqa: E402
from contracts import Order, Wakeup  # noqa: E402
from event_noise import SlowInformedInvestor  # noqa: E402

NAME = "exp19_noise_heterogeneity"
NS_PER_S = 1_000_000_000
FEE_TICKS = op.FEES.taker_bps * op.BP                 # 1.7 bp at v0, in ticks
H_BASE_ALLIN = op.BCS_HALF_SPREAD_BP * op.BP + FEE_TICKS
DESIGN_E = (0.5, 4.0)                                  # one arm per H6c branch
PART_BAND = 0.10
EDGE_FRACTION = 0.05
INELASTIC_MAX_E, ELASTIC_MIN_E = 1.0, 2.0
INELASTIC_MIN_COUNT, ELASTIC_MAX_COUNT = 15, 9
INF = facts.CFG["informed"]


def vbar_for(e: float) -> float:
    return H_BASE_ALLIN / e


class ExpGainInvestor:
    """P6's investor population: Poisson arrivals with an exponential gain from trade.

    Arrival i draws v_i ~ Exp(mean vbar) (ticks per lot), a fair-coin side and
    a lognormal size (median `qty`, sigma_ln), and sends an IOC at the touch
    only if all-in = (ask - s) + fee (a buy; s - bid + fee for a sale) is
    below v_i, s being the last public signal it observed. Every draw is made
    for every arrival, traded or not, so two arms of one seed meet the same
    arrivals, gains, sides and sizes (common random numbers). `gains` is the
    sum of v_i x size over arrivals that traded (ticks x lots).
    """

    def __init__(self, participant_id: int, arrival_rate_per_s: float, vbar: float,
                 taker_bps: float, v0: float, qty: int, size_sigma_ln: float, seed):
        self.participant_id = int(participant_id)
        self.md_latency_ns = self.order_latency_ns = 0
        self._gap_ns = NS_PER_S / float(arrival_rate_per_s)
        self.vbar, self.fee_frac = float(vbar), taker_bps * 1e-4
        self.qty, self.sigma_ln = int(qty), float(size_sigma_ln)
        self._signal = float(v0)
        self._rng = np.random.default_rng(seed)
        self._oid = self.participant_id * 1_000_000_000
        self.arrivals = self.crossings = 0
        self.gains = 0.0

    def _next(self, t_ns):
        return Wakeup(t_ns + max(1, round(self._rng.exponential(self._gap_ns))), "arrive")

    def on_start(self, t_ns, view):
        return [self._next(t_ns)]

    def on_signal(self, t_ns, update, view):
        self._signal = update.value
        return []

    def on_fill(self, t_ns, fill, view):
        return []

    def on_wakeup(self, t_ns, tag, view):
        nxt = self._next(t_ns)
        v = self._rng.exponential(self.vbar)
        is_buy = self._rng.random() < 0.5
        size = max(1, round(self.qty * math.exp(self.sigma_ln * self._rng.standard_normal())))
        self.arrivals += 1
        price = view.best_ask() if is_buy else view.best_bid()
        if price <= 0:
            return [nxt]
        all_in = (price - self._signal if is_buy else self._signal - price) + self.fee_frac * price
        if all_in >= v:
            return [nxt]
        self.crossings += 1
        self.gains += v * size
        self._oid += 1
        side = be.Side.Buy if is_buy else be.Side.Sell
        return [nxt, Order(self._oid, side, price, size, be.OrderType.IOC)]


def run(e: float, k: int, ratio: float, seed: int, horizon_s: float = op.HORIZON_S) -> dict:
    """One run: paper maker, k snipers, the ExpGain investors of arm e, the slow informed."""
    mk = op.build_market(seed, maker_latencies_ns=(cc.PAPER_LATENCY_NS,), k=k,
                         snipe_ratio=ratio, policies=[cc.paper_policy()], horizon_s=horizon_s)
    vbar = vbar_for(e)
    noise = ExpGainInvestor(op.NOISE, op.INVESTOR_RATE_PER_S / math.exp(-H_BASE_ALLIN / vbar),
                            vbar, op.FEES.taker_bps, op.V0, op.INVESTOR_MEDIAN_LOTS,
                            op.INVESTOR_SIZE_SIGMA_LN, [seed, 1])
    informed = SlowInformedInvestor(
        cc.INFORMED, mk.path, info_lag_ns=int(INF["info_lag_s"] * NS_PER_S),
        decision_interval_ns=int(INF["decision_interval_s"] * NS_PER_S),
        parent_qty=INF["parent_lots"], n_children=INF["n_children"],
        child_interval_ns=int(INF["child_interval_s"] * NS_PER_S),
        min_edge=INF["min_edge_bp"] * op.BP, seed=[seed, 2])
    mk = mk._replace(agents=mk.makers + mk.snipers + [noise, informed], noise=noise)
    res, _ = cc.simulate(mk, seed)
    lots = sum(tr.quantity for tr in res.trades if cc.INFORMED in (tr.buyer_id, tr.seller_id))
    return {**cc.legs(res), "arrivals": noise.arrivals, "crossings": noise.crossings,
            "gains_usd": noise.gains / be.PRICE_PRECISION, "informed_lots": lots,
            "duration_s": horizon_s}


def tasks(seeds, horizon_s: float = op.HORIZON_S) -> dict:
    t = {}
    for band, ratio, k in sorted(cc.band_cells(), key=lambda c: -c[2]):
        for e in DESIGN_E:
            for s in seeds:
                t[cc.key("p6", e, band, ratio, k, s)] = (run, e, k, ratio, s, horizon_s)
    for e in DESIGN_E:
        for s in seeds:
            t[cc.key("p6", e, "base", s)] = (run, e, 0, op.PAPER_BAND[0], s, horizon_s)
    return t


# --- statistics and verdicts (pure) -------------------------------------------------

def participation_ci(part, h, part0, h0, vbar: float) -> dict:
    """Measured over predicted participation ratio, pooled over cells.

    part, h: cells x seeds (participation share, mean quoted half-spread in
    ticks); part0, h0: the no-HFT values per seed. Measured = mean part /
    mean part0; predicted = mean over cells of e^(-(mean h_c - mean h0)/vbar).
    Seeds resampled jointly across cells and the baseline (CRN)."""
    part, h = np.asarray(part, float), np.asarray(h, float)
    part0, h0 = np.asarray(part0, float), np.asarray(h0, float)

    def stat(cols):
        measured = part[:, cols].mean() / part0[cols].mean()
        predicted = np.mean(np.exp(-(h[:, cols].mean(1) - h0[cols].mean()) / vbar))
        return measured / predicted

    n = part.shape[1]
    rng = np.random.default_rng(op.BOOT_SEED)
    boots = [stat(rng.integers(0, n, n)) for _ in range(op.N_BOOTSTRAP)]
    return {"mean": float(stat(np.arange(n))), "lower": float(np.percentile(boots, 2.5)),
            "upper": float(np.percentile(boots, 97.5)), "n": n}


def h6a_verdict(ratio: dict, dwl: dict) -> str:
    band = op.band_verdict(ratio["lower"], ratio["upper"], 1 - PART_BAND, 1 + PART_BAND)
    if band == "MISS" or dwl["lower"] <= 0:
        return "MISS"
    return band


def h6b_verdict(edge: dict, h_bar: float) -> str:
    if edge["lower"] > 0:
        return "MISS"
    return "HIT" if edge["upper"] <= EDGE_FRACTION * h_bar else "INCONCLUSIVE"


def h6c_verdict(e_ci: dict, count: int) -> dict:
    if e_ci["upper"] <= INELASTIC_MAX_E:
        branch, ok = "e <= 1", count >= INELASTIC_MIN_COUNT
    elif e_ci["lower"] >= ELASTIC_MIN_E:
        branch, ok = "e >= 2", count <= ELASTIC_MAX_COUNT
    else:
        return {"branch": None, "count": count, "verdict": "INCONCLUSIVE"}
    return {"branch": branch, "count": count, "verdict": "HIT" if ok else "MISS"}


# --- summary -------------------------------------------------------------------------

def summarize(rows: dict, seeds) -> dict:
    out = {"H6a": {"by_arm": {}}, "H6b": {"by_arm": {}}, "H6c": {"by_arm": {}}}
    for e in DESIGN_E:
        vbar, arm = vbar_for(e), f"design_e={e}"
        base = [rows[cc.key("p6", e, "base", s)] for s in seeds]
        cells = {(b, r, k): [rows[cc.key("p6", e, b, r, k, s)] for s in seeds]
                 for b, r, k in cc.band_cells()}
        counts = cc.band_counts(cells, base)
        e_ci = cc.boot([(r["half_spread_ticks"] + FEE_TICKS) / vbar for r in base])
        h6a, h6c = {}, {}
        for band in op.BANDS:
            rs = [cells[c] for c in cells if c[0] == band]
            ratio = participation_ci([[r["crossings"] / r["arrivals"] for r in c] for c in rs],
                                     [[r["half_spread_ticks"] for r in c] for c in rs],
                                     [r["crossings"] / r["arrivals"] for r in base],
                                     [r["half_spread_ticks"] for r in base], vbar)
            dwl = cc.boot([np.mean([(b["gains_usd"] - c[i]["gains_usd"]) / b["duration_s"]
                                    for c in rs]) for i, b in enumerate(base)])
            h6a[band] = {"participation_over_predicted": ratio, "deadweight_usd_per_s": dwl,
                         "verdict": h6a_verdict(ratio, dwl)}
            h6c[band] = {**h6c_verdict(e_ci, counts[band]["count"]), "e": e_ci}
        paper = [base] + [cells[c] for c in cells if c[0] == "paper"]
        edge = cc.ratio_ci([sum(rs[i]["informed_edge"] for rs in paper) * be.PRICE_PRECISION
                            for i in range(len(seeds))],
                           [sum(rs[i]["informed_lots"] for rs in paper) for i in range(len(seeds))])
        h_bar = float(np.nanmean([r["half_spread_ticks"] for rs in paper for r in rs]))
        out["H6a"]["by_arm"][arm] = {"vbar_ticks": vbar, **h6a}
        out["H6b"]["by_arm"][arm] = {"edge_per_lot_ticks": edge, "h_bar_ticks": h_bar,
                                     "bound": EDGE_FRACTION * h_bar,
                                     "verdict": h6b_verdict(edge, h_bar)}
        out["H6c"]["by_arm"][arm] = {"vbar_ticks": vbar, **h6c, "cells": counts}
    for h in ("H6a", "H6c"):
        arms = out[h]["by_arm"].values()
        out[h]["verdict"] = cc.combine(a["paper"]["verdict"] for a in arms)
        out[h]["tape_band_verdict"] = cc.combine(a["tape"]["verdict"] for a in arms)
        out[h]["verdict_band"] = "paper"
    out["H6b"]["verdict"] = cc.combine(a["verdict"] for a in out["H6b"]["by_arm"].values())
    out["H6a"]["primary"] = True
    return out


def main(argv=None) -> dict:
    a = cc.parse_args(NAME, __doc__, argv)
    if INF["info_lag_s"] * NS_PER_S <= 2 * cc.PAPER_LATENCY_NS:
        raise SystemExit("P6: the informed lag must exceed the maker's requote latency")
    t0 = time.time()
    rows = cc.run_pending(tasks(a.seed_list), cc.call, a.checkpoint, a.workers, NAME)
    report = {"experiment": NAME, "prediction": "P6", "status": a.status,
              "seeds": a.seed_list, "horizon_s": op.HORIZON_S, "n_runs": len(rows),
              "config": {"design_e": DESIGN_E, "vbar_ticks": {str(e): vbar_for(e) for e in DESIGN_E},
                         "h_base_allin_ticks": H_BASE_ALLIN, "fee_ticks": FEE_TICKS,
                         "informed": INF, "maker_one_way_ns": cc.PAPER_LATENCY_NS,
                         "paper_policy": repr(cc.paper_policy()), "bands": op.BANDS,
                         "k_grid": op.K_GRID},
              "units": "maker P&L and deadweight USD; spreads and edges in ticks",
              **summarize(rows, a.seed_list), "wall_s": round(time.time() - t0, 1)}
    cc.write_report(a.out, report)
    print({h: report[h]["verdict"] for h in ("H6a", "H6b", "H6c")}, flush=True)
    return report


if __name__ == "__main__":
    main()
