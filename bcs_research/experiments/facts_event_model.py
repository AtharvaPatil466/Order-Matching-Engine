#!/usr/bin/env python3
"""Stylized facts of the event model at its tape-calibrated operating point.

The reviewer asked for spread, volume, return autocorrelation and price impact
against the tape, or a plain statement that this is a mechanism model. The
DATA lane measured the tape and the OLD tick model with one function
(analysis/stylized_facts.py). This runs the REBUILT event model at the
operating point the calibration files pin down and feeds it to that same
function, so the paper can say fact by fact what the new model reproduces.

What may be tuned. Nothing here is fitted to a stylized fact. Every value
below is either read from a calibration file, an institutional fact of the
venue, BCS theory at the fitted parameters, or a stated choice the facts were
never consulted on. Hand-fitting the facts would be the over-claim the
reviewer is looking for.

The operating point (CFG, built once by `tape_config`), and why each value:

Price grid. v0 = 1e4 / tick_bp ~ 630,927, so one engine price unit is
  BTC-perp's 0.10 USD tick (0.01585 bp at the sample's price: targets
  spread_p50_bp / spread_p50_ticks). jump_fit_v2 states v0 = 1e6, but every
  path parameter is relative to v0, so this changes only the grid the makers
  must round to. It makes the tick constraint the tape lives under (a spread
  of one tick) bind here too. A finer grid would let quotes sit inside a
  tick, which the venue does not allow.
Quantity unit. One lot = 0.001 BTC, BTCUSDT-perp's quantity step; the tape's
  smallest record is one lot and its median three.
Path. JumpDiffusionPath.from_calibration(jump_fit_v2): 10 ms updates,
  diffusive sigma 0.242 bp/sqrt(s), 0.200 Laplace jumps/s with b = 0.705 bp.
  Used unchanged.
Noise. ReservationNoiseTrader. Crossings arrive at 16.28/s, the tape's
  aggTrade record rate (targets lambda_per_s) and the lam_invest the BCS
  spread below is computed with. Sizes are lognormal: median 3 lots (tape
  p50 0.003 BTC) and sigma_ln = calibration/fit.size_model's 2.80 (the tape's
  p50-p99 fit, the same shape the tick model uses). The private-value scale
  eps is the path's total 1-s volatility (0.52 bp). That is a stated choice:
  a trader's private value is uncertain at about the price's own 1-s scale.
  The facts barely depend on it, because at a one-tick spread 98.7% of
  arrivals cross for any eps much larger than half a tick; the arrival rate
  is set so that crossings hit 16.28/s. Latency 0, so an arrival always
  meets the quote it decided on and the calibrated rate is also the trade
  rate.
Makers. One risk-neutral StrategicMaker (as_gamma 0, no skew, no limit, no
  fading), which is BCS's maker. One, because BCS's equilibrium has one
  liquidity provider and N-1 snipers. Also because StrategicMaker sends plain
  limits, not post-only: two equally fast makers then cross each other's
  stale quotes on most 10-ms steps (a pilot run gave 75 fills/s against 16
  noise crossings/s, all of it maker-on-maker). It quotes the BCS zero-profit
  half-spread at the fitted parameters,
  bcs_theory.laplace_half_spread(16.28, 0.200, 0.705) = 0.00857 bp, about half
  a tick. After rounding to the grid the spread is one tick (two when V sits
  near a half-tick). It rests the tape's median top-of-book depth (targets
  top_depth_p50 6.182 BTC = 6,182 lots). Latency 50 us to observe plus 50 us
  to order.
Snipers. Three EventSnipers (ABO 2022: 3.3 participants per race), 5 + 5 us,
  so the 90 us head start over the makers is about ABO's mean race length
  (79 us). The threshold is their marginal cost: Binance USD-M's lowest
  public taker fee, 1.7 bp (VIP 9, 0.017%). VIP 0 pays 5 bp (accounting.py
  TAKER_HEAVY); an HFT sniper is top tier. Each sniper sends the whole touch
  depth, so the race winner takes every stale lot, as in BCS. This threshold
  is what keeps the one-tick spread viable. One 10-ms diffusion step is 1.5
  ticks (sd) against a half-spread of half a tick, so cost-free snipers
  would pick off the makers on most updates (the zero-fee sensitivity run
  shows this). The BCS closed form counts jumps only, i.e. it assumes the
  maker tracks the diffusion for free; the fee is what makes that true here.
Informed. One SlowInformedInvestor. It reads V one second late (slow next to
  the microsecond HFTs) and works a parent order of the median touch depth
  (6,182 lots, which it cannot send at once without emptying the touch). The
  parent goes out in 10 IOC children of 10% of the touch each, 1 s apart.
  The 1 s matches depth recovery: depth_replenishment p75 0.92 s, so each
  child meets a refilled touch. A new decision follows 60 s after the last
  child. min_edge is 0. In this path every move of V is public, so the
  investor is informed relative to noise but stale relative to the makers.
  It is a small (~5% of volume) directional splitter, not a source of
  private information. None of these values was chosen with the
  sign-autocorrelation fact in view.
Sampling. Book snapshots every 100 ms, like the tape's 10 Hz book. Fills
  are floored to the tape's 1 ms stamps and merged into aggTrade-style
  records (same price, same aggressor side, within 100 ms of the record's
  first fill: the USD-M rule the DATA lane documents in fine_grain_v2.json),
  so the facts count what the tape counts. Raw-fill facts are kept as well.

Tolerance (stated before the run). A model fact MATCHES the tape if it has
the same sign and lies within a factor of 2 of it, or within an absolute
floor of it: 0.05 for autocorrelations, one tick (0.0158 bp) for impacts,
1 ms for interarrival quantiles, 0.02 for shares. If it does not match, it is
CLOSER than the tick model when its error relative to the tape,
|m - t| / max(|t|, floor), is smaller than the tick model's. Otherwise it
MISSES.

Run (background, ~minutes): bcs_research/.venv/bin/python
bcs_research/experiments/facts_event_model.py
Writes results/pilot/stylized_facts_event_model.json and prints the table.
"""
from __future__ import annotations

import json
import math
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np

_ROOT = Path(__file__).resolve().parents[1]
for _d in ("build", "agents", "simulation", "metrics", "analysis", "calibration"):
    _p = str(_ROOT / _d)
    if _p not in sys.path:
        sys.path.insert(0, _p)

import bcs_engine as be  # noqa: E402
import stylized_facts as sf  # noqa: E402
from bcs_theory import laplace_half_spread  # noqa: E402
from event_noise import ReservationNoiseTrader, SlowInformedInvestor  # noqa: E402
from event_scheduler import EventScheduler  # noqa: E402
from event_sniper import EventSniper  # noqa: E402
from fit import size_model  # noqa: E402
from fundamental_paths import JumpDiffusionPath  # noqa: E402
from strategic_maker import MakerPolicy, StrategicMaker  # noqa: E402

CAL = _ROOT / "results" / "calibration"
OUT = _ROOT / "results" / "pilot" / "stylized_facts_event_model.json"
STATUS = "pilot — pre-sign-off operating point"
SEEDS = tuple(range(1, 11))
SENSITIVITY_SEED, SENSITIVITY_HORIZON_S = 1, 600
NS_PER_S, NS_PER_MS = 1_000_000_000, 1_000_000
AGG_WINDOW_MS = 100        # aggTrade: same price and side within 100 ms merge
LOT_BTC = 0.001            # BTCUSDT-perp quantity step
VIP9_TAKER_BP = 1.7        # Binance USD-M lowest public taker fee, 0.017%
MAKER, SNIPER, NOISE, INFORMED = 100, 200, 300, 400   # participant id blocks


def _load(name: str) -> dict:
    return json.loads((CAL / name).read_text())


def tape_config() -> dict:
    """The one operating point, in physical units, from the calibration files."""
    jump, tg = _load("jump_fit_v2.json"), _load("targets_btcusdt.json")
    lam, mu, b = tg["lambda_per_s"], jump["jump_intensity_per_s"], jump["jump_scale_bp"]
    tick_bp = tg["spread_p50_bp"] / tg["spread_p50_ticks"]
    depth_lots = round(tg["top_depth_p50"] / LOT_BTC)
    return {
        "horizon_s": 3600,
        "snapshot_ms": 100,
        "tick_bp": tick_bp,
        "lot_btc": LOT_BTC,
        "path": {k: jump[k] for k in ("sigma_bp_per_sqrt_s", "jump_intensity_per_s",
                                      "jump_size_dist", "jump_scale_bp", "update_interval_ms")}
                | {"v0": 1e4 / tick_bp},
        "noise": {"trade_rate_per_s": lam,
                  "eps_bp": jump["diagnostics"]["sigma_total_bp_per_sqrt_s"],
                  "median_lots": round(tg["size_p50"] / LOT_BTC),
                  "size_sigma_ln": size_model(tg)["sigma_ln"]},
        "makers": {"n": 1, "half_spread_bp": laplace_half_spread(lam, mu, b),
                   "depth_lots_total": depth_lots,
                   "md_latency_ns": 50_000, "order_latency_ns": 50_000},
        "snipers": {"n": 3, "threshold_bp": VIP9_TAKER_BP, "qty_lots": depth_lots,
                    "md_latency_ns": 5_000, "order_latency_ns": 5_000},
        "informed": {"info_lag_s": 1.0, "decision_interval_s": 60.0, "parent_lots": depth_lots,
                     "n_children": 10, "child_interval_s": 1.0, "min_edge_bp": 0.0},
    }


CFG = tape_config()


# ---------------------------------------------------------------- the market

def _crossing_probability(half_spread_bp: float, eps_bp: float) -> float:
    """ReservationNoiseTrader's 2 * Phi(-h / eps) with the signal at the mid."""
    return math.erfc(half_spread_bp / eps_bp / math.sqrt(2))


def build_market(cfg: dict, seed: int, horizon_s: float | None = None):
    """(agents, path) for one run. Seeds: path `seed`, noise [seed, 1], informed [seed, 2]."""
    duration_ns = int((horizon_s or cfg["horizon_s"]) * NS_PER_S)
    path = JumpDiffusionPath.from_calibration(cfg["path"], duration_ns, seed=seed)
    v0 = cfg["path"]["v0"]
    bp = v0 * 1e-4                                   # engine units per bp
    m, s, n, i = cfg["makers"], cfg["snipers"], cfg["noise"], cfg["informed"]
    policy = MakerPolicy(half_spread=m["half_spread_bp"] * bp,
                         qty=m["depth_lots_total"] // m["n"])
    agents = [StrategicMaker(MAKER + k, policy, md_latency_ns=m["md_latency_ns"],
                             order_latency_ns=m["order_latency_ns"]) for k in range(m["n"])]
    agents += [EventSniper(SNIPER + k, qty=s["qty_lots"], threshold=s["threshold_bp"] * bp,
                           md_latency_ns=s["md_latency_ns"], order_latency_ns=s["order_latency_ns"])
               for k in range(s["n"])]
    p_cross = _crossing_probability(m["half_spread_bp"], n["eps_bp"])
    agents.append(ReservationNoiseTrader(
        NOISE, arrival_rate_per_s=n["trade_rate_per_s"] / p_cross, eps_scale=n["eps_bp"] * bp,
        v0=v0, qty=n["median_lots"], size_sigma_ln=n["size_sigma_ln"], seed=[seed, 1]))
    agents.append(SlowInformedInvestor(
        INFORMED, path, info_lag_ns=int(i["info_lag_s"] * NS_PER_S),
        decision_interval_ns=int(i["decision_interval_s"] * NS_PER_S),
        parent_qty=i["parent_lots"], n_children=i["n_children"],
        child_interval_ns=int(i["child_interval_s"] * NS_PER_S),
        min_edge=i["min_edge_bp"] * bp, seed=[seed, 2]))
    return agents, path


def run(cfg: dict, seed: int, horizon_s: float | None = None):
    """One EventSimResult on a fresh engine."""
    agents, path = build_market(cfg, seed, horizon_s)
    h = be.EngineHarness()
    h.add_symbol(1)
    h.start(disable_circuit_breaker=True)
    try:
        return EventScheduler(h, 1, seed=seed,
                              snapshot_every_ns=cfg["snapshot_ms"] * NS_PER_MS).run(agents, path)
    finally:
        h.stop()


# ---------------------------------------------------------------- the adapter

def aggregate_records(t_ms, px, qty, buy, window_ms: int = AGG_WINDOW_MS):
    """Merge fills into USD-M aggTrade records: a fill joins the open record with
    its price and aggressor side if it lands within `window_ms` of that record's
    first fill (the tape's same-key records are >= 101 ms apart), else opens
    one. Records keep their first fill's time, so they stay time-ordered."""
    open_rec: dict = {}
    rt, rp, rq, rb = [], [], [], []
    for t, p, q, b in zip(t_ms.tolist(), px.tolist(), qty.tolist(), buy.tolist()):
        j = open_rec.get((p, b))
        if j is not None and t - rt[j] <= window_ms:
            rq[j] += q
            continue
        open_rec[(p, b)] = len(rt)
        rt.append(t)
        rp.append(p)
        rq.append(q)
        rb.append(b)
    return (np.array(rt, np.int64), np.array(rp, float), np.array(rq, float),
            np.array(rb, bool))


def sim_arrays(result, aggregate: bool = True) -> dict:
    """EventSimResult -> the keyword arrays stylized_facts.segment_stats takes.

    Trades: times floored to the tape's 1 ms stamps, in seconds; fixed-point
    price; quantity in lots; aggressor side. Merged into aggTrade records
    unless `aggregate` is False. Quotes: the two-sided snapshots only, as
    stylized_facts.tick_model_segment keeps them.
    """
    tr = result.trades
    t_ms = np.array([x.t_ns // NS_PER_MS for x in tr], np.int64)
    px = np.array([x.price for x in tr], float)
    qty = np.array([x.quantity for x in tr], float)
    buy = np.array([x.aggressor_buy for x in tr], bool)
    if aggregate:
        t_ms, px, qty, buy = aggregate_records(t_ms, px, qty, buy)
    snaps = [s for s in result.snapshots if s.best_bid > 0 and s.best_ask > s.best_bid]
    return {"t": t_ms / 1e3, "px": px, "qty": qty, "buy": buy,
            "qt": np.array([s.t_ns for s in snaps], float) / NS_PER_S,
            "bid": np.array([s.best_bid for s in snaps], float),
            "ask": np.array([s.best_ask for s in snaps], float)}


def _diagnostics(result, n_records: int) -> dict:
    snipers = {a.participant_id for a in result.agents if isinstance(a, EventSniper)}
    makers = {a.participant_id for a in result.agents if isinstance(a, StrategicMaker)}
    noise = next(a for a in result.agents if isinstance(a, ReservationNoiseTrader))
    dur = result.duration_ns / NS_PER_S
    contested = [r for r in result.races if r.arrivals]
    sniped = [r for r in contested if r.arrivals[0].participant_id not in makers
              and r.arrivals[0].outcome in ("filled", "partial")]
    sniper_fills = sum(tr.buyer_id in snipers or tr.seller_id in snipers for tr in result.trades)
    return {"fills_per_s": len(result.trades) / dur, "records_per_s": n_records / dur,
            "noise_crossing_share": noise.crossings / noise.arrivals,
            "sniper_fills_per_s": sniper_fills / dur,
            "jump_races": len(contested), "jump_races_won_by_sniper": len(sniped)}


def seed_segments(cfg: dict, seed: int, horizon_s: float | None = None) -> dict:
    """One run reduced to its stylized-facts segments (aggTrade records and raw fills)."""
    res = run(cfg, seed, horizon_s)
    rec = sim_arrays(res)
    return {"seed": seed, "records": sf.segment_stats(**rec),
            "fills": sf.segment_stats(**sim_arrays(res, aggregate=False)),
            "diagnostics": _diagnostics(res, rec["t"].size)}


# ---------------------------------------------------------------- comparison

def _flat(facts: dict) -> dict:
    """Every fact as a dotted name -> number, coverage left out, in facts order."""
    out = {}
    for k, v in facts.items():
        if k in ("coverage", "source"):
            continue
        if isinstance(v, list):
            out |= {f"{k}.{i}": x for i, x in enumerate(v)}
        elif isinstance(v, dict):
            for k2, v2 in v.items():
                if isinstance(v2, dict):
                    out |= {f"{k}.{k2}.{k3}": x for k3, x in v2.items()}
                else:
                    out[f"{k}.{k2}"] = v2
    return out


def floor_for(name: str, tick_bp: float) -> float:
    if "autocorrelation" in name:
        return 0.05
    if name.startswith("impact"):
        return tick_bp
    if name.startswith("interarrival_ms.p"):
        return 1.0
    if "share" in name:
        return 0.02
    return 0.0


def verdict(tape: float, tick: float, event: float, floor: float) -> str:
    if any(math.isnan(x) for x in (tape, event)):
        return "n/a"
    same_sign_within_2x = tape != 0 and 0.5 <= event / tape <= 2
    if same_sign_within_2x or abs(event - tape) <= floor:
        return "matches"
    scale = max(abs(tape), floor)
    if math.isnan(tick) or abs(event - tape) / scale < abs(tick - tape) / scale:
        return "closer than tick model"
    return "misses"


def compare(tape: dict, tick: dict, event: dict, per_seed: list[dict], tick_bp: float) -> list:
    ft, fk, fe = _flat(tape), _flat(tick), _flat(event)
    seeds = [_flat(s) for s in per_seed]
    rows = []
    for name, e in fe.items():
        t, k = ft.get(name, float("nan")), fk.get(name, float("nan"))
        vals = [s[name] for s in seeds if not math.isnan(s[name])]
        rows.append({"stat": name, "tape": t, "tick_model": k, "event_model": e,
                     "event_seed_min": min(vals, default=float("nan")),
                     "event_seed_max": max(vals, default=float("nan")),
                     "verdict": verdict(t, k, e, floor_for(name, tick_bp))})
    return rows


def table(rows: list) -> str:
    g = lambda x: f"{x:.4g}"  # noqa: E731
    lines = ["| stat | tape | tick model | event model (seed range) | verdict |",
             "|---|---|---|---|---|"]
    lines += [f"| {r['stat']} | {g(r['tape'])} | {g(r['tick_model'])} | {g(r['event_model'])} "
              f"({g(r['event_seed_min'])}..{g(r['event_seed_max'])}) | {r['verdict']} |"
              for r in rows]
    return "\n".join(lines)


def main() -> dict:
    zero_fee = {**CFG, "snipers": {**CFG["snipers"], "threshold_bp": 0.0}}
    with ProcessPoolExecutor() as pool:
        futs = [pool.submit(seed_segments, CFG, s) for s in SEEDS]
        sens = pool.submit(seed_segments, zero_fee, SENSITIVITY_SEED, SENSITIVITY_HORIZON_S)
        parts = [f.result() for f in futs]
        sens = sens.result()
    tape = _load("stylized_facts_tape.json")
    tick = _load("stylized_facts_tick_model_v2.json")
    event = sf.combine([p["records"] for p in parts])
    per_seed = [sf.combine([p["records"]]) for p in parts]
    rows = compare(tape, tick, event, per_seed, CFG["tick_bp"])
    out = {
        "status": STATUS,
        "source": "experiments/facts_event_model.py; facts from analysis/stylized_facts.py",
        "config": CFG, "seeds": list(SEEDS),
        "tolerance": ("matches: same sign and within a factor of 2 of the tape, or within an "
                      "absolute floor (0.05 autocorrelations, one tick for impacts in bp, 1 ms "
                      "interarrival quantiles, 0.02 shares); else 'closer than tick model' if "
                      "|m-t|/max(|t|,floor) beats the tick model's; else 'misses'"),
        "tuning": ("no parameter fitted to a fact; every value from calibration files, venue "
                   "facts or BCS theory. One structural change after a 60-s pilot: 2 makers "
                   "-> 1, because plain-limit makers cross each other's stale quotes "
                   "(52 records/s, all maker-on-maker, before; 14 after)"),
        "event_model": event,
        "event_model_per_seed": per_seed,
        "event_model_raw_fills": sf.combine([p["fills"] for p in parts]),
        "diagnostics_per_seed": [p["diagnostics"] for p in parts],
        "comparison": rows,
        "sensitivity_zero_fee_snipers": {
            "change": "snipers.threshold_bp 1.7 -> 0", "seed": SENSITIVITY_SEED,
            "horizon_s": SENSITIVITY_HORIZON_S, "diagnostics": sens["diagnostics"],
            "facts": sf.combine([sens["records"]])},
    }
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(out, indent=2, allow_nan=True) + "\n")
    print(table(rows))
    print("\ndiagnostics:", json.dumps(out["diagnostics_per_seed"], indent=1))
    print("zero-fee sensitivity:", json.dumps(sens["diagnostics"]))
    print(f"\nwrote {OUT.relative_to(_ROOT)}")
    return out


if __name__ == "__main__":
    main()
