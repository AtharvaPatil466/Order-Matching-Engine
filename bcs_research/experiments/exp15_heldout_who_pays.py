#!/usr/bin/env python3
"""Experiment 15 — held-out test of which competing maker pays (PREREGISTRATION P10).

Why this exists. The paper's within-class headline was that among competing
makers one gains while another pays, and that who pays is "set by how the
makers' latencies are spaced, not by speed rank". Its support was a count of
136/144 cells, which is pseudo-replicated: the cells share seeds and eight
latency configurations. A reviewer asked for a held-out prediction test
instead. P10 fixes a parameter-free predictor, "the slowest maker pays", and
a decision rule. Both are fixed in this file before any test configuration
is generated: the commit that adds this predictor precedes the commit that
adds the configuration generator.

Unit of replication: the CONFIGURATION. Each configuration gets its own block
of seeds; it contributes one hit or miss and one Spearman sign. Cells that
share seeds are never counted separately.

Decision rule (P10, read with PREREGISTRATION section 2's band convention:
a threshold on a proportion is met when the 95% Wilson interval clears it,
and a miss when the interval lies wholly on the wrong side):
  MISS          Wilson upper bound of the overall hit rate < 0.60, or of the
                closely spaced configurations' hit rate < 0.50;
  HIT           otherwise, if the Wilson lower bounds of the hit rate AND of
                the share of configurations with Spearman(latency, PnL delta)
                < 0 are both >= 0.80;
  INCONCLUSIVE  otherwise.
The point-estimate reading (rates >= 0.80) is reported beside it and never
replaces it. A one-sided exact test against chance (a configuration with M
makers hits by luck with probability 1/M) is reported too.
"""
from __future__ import annotations

import json
import math
import os
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
from scipy import stats

HIT_BAND = 0.80          # P10: right in >= 80% of held-out cells
MISS_OVERALL = 0.60      # P10: miss below 60%
MISS_CLOSE = 0.50        # P10: miss below 50% on the closely spaced sets
Z95 = 1.959963984540054


# --- the predictor (P10, parameter-free) ------------------------------------

def predicted_payer(latencies) -> int:
    """Index of the maker P10 says pays: the one with the largest latency."""
    lat = list(latencies)
    if len(set(lat)) != len(lat):
        raise ValueError(f"latencies must be distinct, got {lat}")
    return int(np.argmax(lat))


def is_hit(latencies, deltas) -> bool:
    """The maker with the most negative PnL delta is the slowest one."""
    return int(np.argmin(deltas)) == predicted_payer(latencies)


def spearman(latencies, deltas) -> float:
    """Spearman rank correlation of latency with PnL delta (average ranks on ties)."""
    a, b = stats.rankdata(latencies), stats.rankdata(deltas)
    if np.ptp(a) == 0 or np.ptp(b) == 0:
        return 0.0
    return float(np.corrcoef(a, b)[0, 1])


# --- the statistics ---------------------------------------------------------

def wilson(successes: int, n: int, z: float = Z95) -> dict:
    """Wilson score interval for a proportion."""
    if n <= 0:
        return {"rate": float("nan"), "lower": 0.0, "upper": 1.0, "n": 0, "k": 0}
    p = successes / n
    den = 1 + z * z / n
    centre = (p + z * z / (2 * n)) / den
    half = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / den
    return {"rate": p, "lower": max(0.0, centre - half), "upper": min(1.0, centre + half),
            "n": n, "k": successes}


def chance_pvalue(hits: int, maker_counts) -> float:
    """P(at least `hits` hits) if each configuration with M makers hits w.p. 1/M.

    Exact Poisson-binomial upper tail, since configurations mix maker counts.
    """
    dist = np.array([1.0])
    for m in maker_counts:
        p = 1.0 / m
        dist = np.convolve(dist, [1 - p, p])
    return float(dist[hits:].sum())


def classify(per_config) -> dict:
    """P10's verdict over configurations.

    per_config: dicts with "latencies", "deltas" (one mean PnL delta per maker,
    same order) and "closely_spaced" (bool).
    """
    hits = [is_hit(c["latencies"], c["deltas"]) for c in per_config]
    neg = [spearman(c["latencies"], c["deltas"]) < 0 for c in per_config]
    close = [h for h, c in zip(hits, per_config) if c["closely_spaced"]]
    overall, rho, close_w = wilson(sum(hits), len(hits)), wilson(sum(neg), len(neg)), \
        wilson(sum(close), len(close))
    if overall["upper"] < MISS_OVERALL or (close and close_w["upper"] < MISS_CLOSE):
        verdict = "MISS"
    elif overall["lower"] >= HIT_BAND and rho["lower"] >= HIT_BAND:
        verdict = "HIT"
    else:
        verdict = "INCONCLUSIVE"
    point = ("MISS" if overall["rate"] < MISS_OVERALL or (close and close_w["rate"] < MISS_CLOSE)
             else "HIT" if overall["rate"] >= HIT_BAND and rho["rate"] >= HIT_BAND
             else "INCONCLUSIVE")
    return {
        "verdict": verdict,
        "verdict_point_estimate": point,
        "hit_rate": overall,
        "spearman_negative_share": rho,
        "closely_spaced_hit_rate": close_w,
        "chance_pvalue": chance_pvalue(sum(hits), [len(c["latencies"]) for c in per_config]),
        "chance_rate": float(np.mean([1 / len(c["latencies"]) for c in per_config])),
        "n_configs": len(per_config),
    }


# --- held-out configurations (added after the predictor above was committed) --
#
# P10 maps the tick model's latencies onto the event clock at 1 tick = 100 ms.
# Each configuration is a latency set (one latency per maker, both legs, as the
# tick maker observes and posts with the same lag), a sniper count k and a
# snipe/quote ratio from the tape band, as P10's "tape-band cells". The
# latencies go to maker slots in a random order, so slot and rank are not
# confounded (exp7d's slot concern).

TICK_NS = 100_000_000
PAPER_CONFIGS = frozenset(map(frozenset, ({0, 5}, {0, 3, 5}, {1, 5}, {1, 3, 5},
                                          {2, 6}, {1, 4}, {1, 4, 7}, {2, 3, 4})))
# P10's own list; the first three are the ones it calls closely spaced.
P10_LISTED = ((1, 2), (1, 3), (1, 2, 3), (3, 5), (1, 7), (1, 2, 6), (1, 5, 6), (3, 4, 7), (2, 4, 8))
P10_CLOSE = frozenset(map(frozenset, ((1, 2), (1, 3), (1, 2, 3))))
K_GRID = (1, 2, 3, 5, 6, 7, 8, 13, 21)          # exp6_incidence_surface.K_GRID
TAPE_BAND = (0.0049, 0.0097)                    # snipe / aggregate quote
MAKER_COUNTS = (2, 3, 4)
CLOSE_GAP_TICKS = 1.0                            # generated sets: every gap <= 1 tick
GENERATOR_SEED = 15_000
SEED_BLOCK = 1_000                               # config i runs seeds i*1000 + 1 ..


def _with_cell(rng, ticks, family, closely_spaced):
    ticks = [float(t) for t in ticks]
    return {"ticks": ticks,
            "slots": [int(i) for i in rng.permutation(len(ticks))],
            "k": int(rng.choice(K_GRID)), "ratio": float(rng.choice(TAPE_BAND)),
            "family": family, "closely_spaced": bool(closely_spaced)}


def generate_configs(n_generated, seed=GENERATOR_SEED):
    """P10's nine listed sets, then `n_generated` drawn sets, alternating two families.

    spread: M latencies uniform on [0.5, 10] ticks;
    close:  a base uniform on [0.5, 8] ticks plus M - 1 gaps uniform on
            [0.1, 1] tick, so every gap is at most one tick.
    Draws are continuous (rounded to 1 us), so none coincides with the paper's
    eight integer sets or with the 10 ms update grid; the check is kept anyway.
    """
    rng = np.random.default_rng(seed)
    out = [_with_cell(rng, t, "p10_listed", frozenset(t) in P10_CLOSE) for t in P10_LISTED]
    while len(out) < len(P10_LISTED) + n_generated:
        m = int(rng.choice(MAKER_COUNTS))
        if (len(out) - len(P10_LISTED)) % 2 == 0:
            family, ticks = "spread", rng.uniform(0.5, 10.0, m)
        else:
            family = "close"
            ticks = rng.uniform(0.5, 8.0) + np.r_[0.0, np.cumsum(rng.uniform(0.1, 1.0, m - 1))]
        ticks = np.round(np.sort(ticks), 5)                  # 1e-5 tick = 1 us
        if len(set(ticks)) < m or frozenset(ticks) in PAPER_CONFIGS:
            continue
        out.append(_with_cell(rng, ticks, family, np.all(np.diff(ticks) <= CLOSE_GAP_TICKS)))
    for i, c in enumerate(out):
        c["index"] = i
    return out


# --- the market (placeholder operating point: exp11's CFG) ------------------

_ROOT = Path(__file__).resolve().parent.parent
for _d in ("build", "agents", "simulation", "metrics", "experiments", "analysis"):
    if str(_ROOT / _d) not in sys.path:
        sys.path.insert(0, str(_ROOT / _d))

STATUS = "pilot — pre-sign-off; not the confirmatory test of P10"
SNIPER0, NOISE0 = 50, 100


def market_cfg():
    import exp11_batch_semantics as x11
    return dict(x11.CFG)


def split_qty(total, n):
    """exp1_primary._split_qty: aggregate depth held fixed, remainder to slot 0."""
    base = total // n
    return [base + (total - base * n if i == 0 else 0) for i in range(n)]


def run_market(config, seed, with_snipers, cfg=None):
    """One run; returns each maker slot's trading edge against the true V."""
    import bcs_engine as be
    import exp11_batch_semantics as x11
    from event_noise import ReservationNoiseTrader
    from event_scheduler import EventScheduler
    from event_sniper import EventSniper
    from strategic_maker import MakerPolicy, StrategicMaker

    cfg = cfg or market_cfg()
    m = len(config["ticks"])
    tick_ns = config.get("tick_ns", TICK_NS)
    lat = [int(round(config["ticks"][r] * tick_ns)) for r in config["slots"]]   # slot -> latency
    qtys = split_qty(int(round(1 / config["ratio"])), m)
    h = x11.half_spread(cfg)
    makers = [StrategicMaker(1 + i, MakerPolicy(half_spread=h, qty=qtys[i], post_only=True),
                             md_latency_ns=lat[i], order_latency_ns=lat[i]) for i in range(m)]
    snipers = ([EventSniper(SNIPER0 + j, 1, md_latency_ns=cfg["sniper_latency_ns"],
                            order_latency_ns=cfg["sniper_latency_ns"])
                for j in range(config["k"])] if with_snipers else [])
    n = cfg["n_noise"]
    noise = [ReservationNoiseTrader(NOISE0 + i, cfg["lam_invest"] / n,
                                    cfg["noise_eps_mult"] * cfg["jump_b"], cfg["v0"],
                                    qty=cfg["qty"], seed=seed * 1000 + i) for i in range(n)]
    agents = makers + snipers + noise
    path = x11.make_path(cfg, seed)
    harness = be.EngineHarness()
    harness.add_symbol(1)
    harness.start(disable_circuit_breaker=True)
    res = EventScheduler(harness, 1, seed=seed, snapshot_every_ns=cfg["snapshot_every_ns"],
                         race_jitter_ns={a.participant_id: cfg["jitter_ns"] for a in agents}
                         ).run(agents, path)
    harness.stop()
    edge = dict.fromkeys(range(1, m + 1), 0.0)
    sniped = dict.fromkeys(range(1, m + 1), 0)
    for tr in res.trades:
        e = (path.value_at(tr.t_ns) - tr.price) * tr.quantity / be.PRICE_PRECISION
        if tr.buyer_id in edge:
            edge[tr.buyer_id] += e
            sniped[tr.buyer_id] += tr.quantity if SNIPER0 <= tr.seller_id < NOISE0 else 0
        if tr.seller_id in edge:
            edge[tr.seller_id] -= e
            sniped[tr.seller_id] += tr.quantity if SNIPER0 <= tr.buyer_id < NOISE0 else 0
    # ponytail: no reject count. StrategicMaker counts a refusal only if the order
    # is still in its believed book, and a 100 ms maker has requoted ten times
    # before the refusal reaches it, so `rejects` would read 0 here.
    return {"latency_ns": lat, "edge": [edge[i] for i in range(1, m + 1)],
            "sniped_qty": [sniped[i] for i in range(1, m + 1)]}


def run_config_seed(config, seed, cfg=None):
    """Matched pair: the same path, investors and scheduler seed, snipers on and off.

    The maker's P&L delta is its edge against V with snipers minus without
    (exp11's welfare measure: P&L's mean without the terminal mark's noise).
    """
    on, off = run_market(config, seed, True, cfg), run_market(config, seed, False, cfg)
    return {"index": config["index"], "seed": seed, "latency_ns": on["latency_ns"],
            "delta": [a - b for a, b in zip(on["edge"], off["edge"])],
            "edge_on": on["edge"], "edge_off": off["edge"], "sniped_qty": on["sniped_qty"]}


def score_config(config, rows):
    """One configuration's verdict from its own seeds: mean delta per maker slot."""
    from bootstrap import bootstrap_ci
    lat = rows[0]["latency_ns"]
    deltas = np.array([r["delta"] for r in rows])
    mean = deltas.mean(0).tolist()
    return {**{k: config[k] for k in ("index", "ticks", "slots", "k", "ratio", "family",
                                      "closely_spaced")},
            "n_seeds": len(rows), "latencies": lat, "deltas": mean,
            "delta_ci": [bootstrap_ci(deltas[:, i], seed=0) for i in range(len(lat))],
            "sniped_qty": np.mean([r["sniped_qty"] for r in rows], 0).tolist(),
            "edge_off": np.mean([r["edge_off"] for r in rows], 0).tolist(),
            "hit": is_hit(lat, mean), "spearman": spearman(lat, mean)}


def _run_task(args):
    return run_config_seed(*args)


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--configs", type=int, default=40, help="generated configs beyond P10's nine")
    ap.add_argument("--seeds", type=int, default=30, help="seeds per configuration")
    ap.add_argument("--tick-ns", type=int, default=TICK_NS,
                    help="event-clock length of one tick; P10 fixes 100 ms. Any other "
                         "value is an exploratory rescaling, outside P10's verdict")
    ap.add_argument("--out-name", default="exp15_heldout_who_pays.json")
    args = ap.parse_args(argv)
    t0 = time.time()
    configs = generate_configs(args.configs)
    for c in configs:
        c["seeds"] = [c["index"] * SEED_BLOCK + s for s in range(1, args.seeds + 1)]
        c["tick_ns"] = args.tick_ns
    tasks = [(c, s) for c in configs for s in c["seeds"]]
    out_dir = _ROOT / "results" / "pilot"
    out_dir.mkdir(parents=True, exist_ok=True)
    rows_path = out_dir / (Path(args.out_name).stem + ".rows.jsonl")
    rows = []
    with ProcessPoolExecutor(max_workers=max(1, (os.cpu_count() or 2) - 1)) as pool, \
            rows_path.open("w") as fh:
        for r in pool.map(_run_task, tasks, chunksize=2):
            rows.append(r)
            fh.write(json.dumps(r) + "\n")
            if len(rows) % 200 == 0:
                print(f"{len(rows)}/{len(tasks)} runs, {time.time() - t0:.0f}s", flush=True)
    by_cfg = {}
    for r in rows:
        by_cfg.setdefault(r["index"], []).append(r)
    scored = [score_config(c, by_cfg[c["index"]]) for c in configs]
    families = sorted({c["family"] for c in scored})
    report = {
        "status": STATUS if args.tick_ns == TICK_NS else
        STATUS + f"; EXPLORATORY rescaling, 1 tick = {args.tick_ns} ns, outside P10's verdict",
        "config": market_cfg(), "tick_ns": args.tick_ns,
        "generator_seed": GENERATOR_SEED, "n_seeds_per_config": args.seeds,
        "p10": classify(scored),
        "by_family": {f: classify([c for c in scored if c["family"] == f]) for f in families},
        "by_maker_count": {str(m): classify([c for c in scored if len(c["latencies"]) == m])
                           for m in sorted({len(c["latencies"]) for c in scored})},
        "configs": scored, "elapsed_s": time.time() - t0,
    }
    out = out_dir / args.out_name
    out.write_text(json.dumps(report, indent=2, sort_keys=True))
    p = report["p10"]
    print(f"P10 {p['verdict']} (point: {p['verdict_point_estimate']}): hit rate "
          f"{p['hit_rate']['k']}/{p['n_configs']} [{p['hit_rate']['lower']:.2f}, "
          f"{p['hit_rate']['upper']:.2f}], Spearman<0 {p['spearman_negative_share']['k']}/"
          f"{p['n_configs']}, close {p['closely_spaced_hit_rate']['k']}/"
          f"{p['closely_spaced_hit_rate']['n']}, chance p = {p['chance_pvalue']:.2g}")
    print(f"wrote {out.relative_to(_ROOT)} in {report['elapsed_s']:.0f}s")
    return report


if __name__ == "__main__":
    main()
