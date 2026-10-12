#!/usr/bin/env python3
"""Experiment 14 — incidence over priors, not at points (PILOT).

Why. The paper states incidence at point values: one snipe/depth ratio
(0.0283, since found mismeasured), one maker, one fee schedule. A reviewer
asked that these become priors and that incidence be reported over them:
who bears the latency tax, makers or noise traders, as a distribution, and
which prior drives its spread.

Priors.
  Snipe / depth ratio — the tape's own distribution of race size over top-of-
    book depth (results/calibration/race_proxy_v2.json, window 0 ms), as an
    inverse CDF through its published p10..p99 (sensitivity.quantile_prior):
    median 0.052. Robustness twin: the same measure restricted to book
    snapshots <= 20 ms old (median 0.18), since a stale snapshot overstates
    depth. A ratio above 1 lets one IOC take the whole quote, so the 19% of
    races larger than the touch all act as ratio 1.
  Maker count M — uniform on {1, ..., 5}. The public tape is anonymous, so
    no measurement of how many firms quote the touch exists; a flat prior
    over P11's pre-registered range says exactly that. M is a prior and not a
    point because the tick model's incidence flipped sign between M = 1 and
    M = 2 (paper §4.4), so whatever is assumed about M should be visible in
    the answer: results are also reported conditional on each M.
  Fees — Binance USDⓈ-M USDT-margined tiers, uniform over VIP 0..9, one tier
    for the professional participants (makers and snipers), whose tier is
    unknown; noise traders pay the retail VIP 0 rate. The sniper's threshold
    is its taker fee. Table (maker / taker, bp), FEE_TIERS below:
      VIP 0 2.0/5.0  1 1.8/5.0  2 1.6/4.0  3 1.2/3.2  4 1.0/3.0
      VIP 5 0.8/2.7  6 0.6/2.5  7 0.4/2.2  8 0.2/2.0  9 0.0/1.7
    Sources, checked 2026-10-10: VIP 0 — Binance FAQ "Binance Futures Fee
    Structure & Fee Calculations", binance.com/en/support/faq/360033544231
    (updated 2026-05-01: "a Regular User's maker fee is 0.02% and a Regular
    User's taker fee is 0.05%"). VIP 1-3 — Binance's VIP programme change of
    2026-03-18 as reported by Forklog (19.03.2026, "Binance lowers VIP
    programme entry thresholds..."): VIP 1 0.018/0.05%, VIP 2 0.016/0.04%,
    VIP 3 0.012/0.032% unchanged. VIP 4-9 — the live table
    binance.com/en/fee/futureFee is script-rendered and could not be read
    directly; the values are those transcribed from it on 2026-09-18 in
    github.com/jack0752168/crypto-exchange-fee-data (fees/binance-2026.json),
    which agrees with the two primary sources on VIP 0-3. Standard rates;
    the 10% BNB discount is ignored.
Everything else sits at the tape operating point (exp13 BASE_POINT): k = 3
snipers, makers 90 us behind them with 50 us jitter (so a maker never wins a
race), BCS half-spread, the paper's widening.

Design. Not Monte Carlo draws but the priors' stratification: the ratio at
the midpoints of RATIO_NODES equal-probability bins, every M and every tier,
each cell weighted by its prior mass. Each cell runs SEEDS common random
numbers (same paths and noise in every cell), so cell differences are not path
luck. The no-sniper arm depends on M and the seed only (fees reach a run only
through the snipers' threshold), so it runs once per (M, seed) and is re-priced
under each tier. Per cell: the mean over seeds of each outcome. Over the prior:
weighted quantiles of those cell means; the probability that makers bear the
majority (maker delta below noise delta); and which prior drives the spread,
from exact first-order variance shares on the grid (sensitivity.grid_indices).
Intervals: percentile bootstrap over seeds (1,000 resamples, seed 0).

Latency tax = -(maker_delta + noise_delta): what the two liquidity-facing
classes lose to the snipers' presence (= snipers' net + extra exchange fees).
Maker share = -maker_delta / tax, over cells where a snipe happened and tax > 0
(see summarise_cells for why cells without a snipe carry no incidence).

Status: PILOT, pre-sign-off; output to results/pilot/.

Run (background): bcs_research/.venv/bin/python bcs_research/experiments/exp14_incidence_priors.py [seeds] [horizon_s] [ratio_nodes] [workers]
"""
from __future__ import annotations

import json
import os
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
for _d in ("build", "agents", "simulation", "metrics", "analysis", "calibration", "experiments"):
    _p = str(_ROOT / _d)
    if _p not in sys.path:
        sys.path.insert(0, _p)

import numpy as np  # noqa: E402

import exp13_global_sensitivity as x13  # noqa: E402
import sensitivity as sa  # noqa: E402

STATUS = x13.STATUS
RACE_PROXY = _ROOT / "results" / "calibration" / "race_proxy_v2.json"
RATIO_SOURCES = {"tape": "race_qty_over_top_depth",
                 "fresh_snapshot": "race_qty_over_top_depth_snapshot_le_20ms"}
QUANTILE_KEYS = ("p10", "p25", "p50", "p75", "p90", "p99")
FEE_TIERS = ((2.0, 5.0), (1.8, 5.0), (1.6, 4.0), (1.2, 3.2), (1.0, 3.0),
             (0.8, 2.7), (0.6, 2.5), (0.4, 2.2), (0.2, 2.0), (0.0, 1.7))   # VIP 0..9, bp
TIER_PRIOR = np.full(len(FEE_TIERS), 1 / len(FEE_TIERS))
MAKER_COUNTS = (1, 2, 3, 4, 5)
MAKER_PRIOR = np.full(len(MAKER_COUNTS), 1 / len(MAKER_COUNTS))
RETAIL = FEE_TIERS[0]
PILOT_SEEDS, PILOT_HORIZON_S, PILOT_NODES = 8, 60.0, 4
N_BOOT, BOOT_SEED = 1000, 0
QS = (0.1, 0.25, 0.5, 0.75, 0.9)
PILOT_OUT = _ROOT / "results" / "pilot" / "exp14_incidence_priors.json"


def ratio_prior(source: str):
    """Inverse CDF of the tape's race size / top depth (window 0 ms)."""
    d = json.loads(RACE_PROXY.read_text())["by_window_ms"]["0"][RATIO_SOURCES[source]]
    return sa.quantile_prior([float(k[1:]) / 100 for k in QUANTILE_KEYS],
                             [d[k] for k in QUANTILE_KEYS])


def ratio_nodes(source: str, n: int) -> np.ndarray:
    """Midpoints of n equal-probability bins: each node carries mass 1/n."""
    return ratio_prior(source)((np.arange(n) + 0.5) / n)


def fee_table(tier: int) -> dict:
    m, t = FEE_TIERS[tier]
    return {"maker": (m, t), "sniper": (m, t), "noise": RETAIL}


def point(ratio: float, n_makers: int, tier: int, seed: int) -> dict:
    return x13.BASE_POINT | {"snipe_ratio": float(ratio), "n_makers": n_makers,
                             "fee_table": fee_table(tier), "seed": seed}


def _task(args):
    p, horizon_s, with_snipers = args
    return x13.simulate(p, horizon_s, with_snipers)


def run_grid(seeds, horizon_s, n_nodes, workers=None) -> dict:
    """Outcome arrays per ratio source, shaped (nodes, M, tier, seed), per outcome."""
    nodes = {s: ratio_nodes(s, n_nodes) for s in RATIO_SOURCES}
    base_keys = [(m, s) for m in MAKER_COUNTS for s in seeds]
    keys = [(src, i, mi, t, s) for src in RATIO_SOURCES for i in range(n_nodes)
            for mi in range(len(MAKER_COUNTS)) for t in range(len(FEE_TIERS)) for s in seeds]
    tasks = ([(point(0.0, m, 0, s), horizon_s, False) for m, s in base_keys]
             + [(point(nodes[src][i], MAKER_COUNTS[mi], t, s), horizon_s, True)
                for src, i, mi, t, s in keys])
    with ProcessPoolExecutor(max_workers=workers or max(1, (os.cpu_count() or 2) - 2)) as ex:
        runs = list(ex.map(_task, tasks, chunksize=2))
    base = dict(zip(base_keys, runs[:len(base_keys)]))
    shape = (n_nodes, len(MAKER_COUNTS), len(FEE_TIERS), len(seeds))
    out = {src: {} for src in RATIO_SOURCES}
    for (src, i, mi, t, s), w in zip(keys, runs[len(base_keys):]):
        row = x13.outcomes_from(w, base[(MAKER_COUNTS[mi], s)], fee_table(t), horizon_s)
        row["tax"] = -(row["maker_delta"] + row["noise_delta"])
        for k, v in row.items():
            out[src].setdefault(k, np.zeros(shape))[i, mi, t, list(seeds).index(s)] = v
    return {"nodes": {k: v.tolist() for k, v in nodes.items()}, "arrays": out}


def weighted_quantile(values, weights, qs) -> list[float]:
    """Quantiles of a discrete distribution (inverse of the step CDF, midpoint rule)."""
    v, w = np.asarray(values, float).ravel(), np.asarray(weights, float).ravel()
    order = np.argsort(v)
    v, w = v[order], w[order] / w.sum()
    cdf = np.cumsum(w) - w / 2
    return np.interp(qs, cdf, v).tolist()


def _weights(n_nodes) -> np.ndarray:
    return np.einsum("i,j,k->ijk", np.full(n_nodes, 1 / n_nodes), MAKER_PRIOR, TIER_PRIOR)


def summarise_cells(cell: dict, w: np.ndarray) -> dict:
    """Prior-weighted summary of cell means {outcome: (nodes, M, tier) array}.

    Incidence is read only in cells where a snipe happened. Elsewhere the two
    arms differ only through the scheduler's random stream (the snipers' idle
    observations reshuffle same-instant events), so maker and noise deltas are
    mean-zero noise; with retail fees on the noise traders' heavy-tailed sizes
    that noise is not small, and counting it would turn it into "incidence".
    """
    tax, md, nd = cell["tax"], cell["maker_delta"], cell["noise_delta"]
    sniped = cell["sniper_fills"] > 0
    taxed = sniped & (tax > 0)
    share = np.where(taxed, -md / np.where(taxed, tax, 1.0), np.nan)
    out = {k: weighted_quantile(cell[k], w, QS)
           for k in ("sniper_rent", "maker_delta", "noise_delta", "tax", "gap_rate")}
    out["mean"] = {k: float(np.sum(w * cell[k]))
                   for k in ("sniper_rent", "maker_delta", "noise_delta", "tax")}
    out["maker_share"] = (weighted_quantile(share[taxed], w[taxed], QS) if taxed.any()
                          else [float("nan")] * len(QS))
    tax_sniped = np.sum(w[sniped] * tax[sniped])
    out["maker_share_of_mean_tax"] = (float(np.sum(w[sniped] * -md[sniped]) / tax_sniped)
                                      if tax_sniped > 0 else float("nan"))
    out["p_makers_bear_majority"] = float(np.sum(w[sniped & (md < nd)]))
    out["p_noise_bears_majority"] = float(np.sum(w[sniped & (nd < md)]))
    out["p_no_snipe"] = float(np.sum(w[~sniped]))
    return out


def analyse_source(arrays: dict, n_nodes: int) -> dict:
    w = _weights(n_nodes)
    keys = ("sniper_rent", "maker_delta", "noise_delta", "tax", "gap_rate", "sniper_fills")
    cell = {k: arrays[k].mean(axis=-1) for k in keys}
    point_est = summarise_cells(cell, w)
    n_seeds = arrays["tax"].shape[-1]
    rng = np.random.default_rng(BOOT_SEED)
    boots = []
    for _ in range(N_BOOT):
        idx = rng.integers(0, n_seeds, n_seeds)
        boots.append(summarise_cells({k: arrays[k][..., idx].mean(axis=-1) for k in keys}, w))
    ci = {k: {"lo": float(np.nanpercentile([b[k] for b in boots], 2.5)),
              "hi": float(np.nanpercentile([b[k] for b in boots], 97.5))}
          for k in ("p_makers_bear_majority", "p_noise_bears_majority", "p_no_snipe",
                    "maker_share_of_mean_tax")}
    ci |= {f"mean_{k}": {"lo": float(np.percentile([b["mean"][k] for b in boots], 2.5)),
                         "hi": float(np.percentile([b["mean"][k] for b in boots], 97.5))}
           for k in point_est["mean"]}
    drivers = {}
    for k in ("sniper_rent", "maker_delta", "tax"):
        g = sa.grid_indices(cell[k], [np.full(n_nodes, 1 / n_nodes), MAKER_PRIOR, TIER_PRIOR])
        drivers[k] = dict(zip(("ratio", "n_makers", "fee_tier"), g["first_order"]))
        drivers[k]["interaction"] = g["interaction"]

    def by(axis, labels):
        other = tuple(a for a in range(3) if a != axis)
        wm = w.sum(axis=other)
        return {str(lab): {k: float(np.sum(w * cell[k], axis=other)[j] / wm[j])
                           for k in ("sniper_rent", "maker_delta", "noise_delta", "tax")}
                for j, lab in enumerate(labels)}
    return {"summary": point_est, "bootstrap_ci": ci, "spread_drivers_first_order": drivers,
            "by_n_makers": by(1, MAKER_COUNTS), "by_fee_tier": by(2, [f"VIP{i}" for i in range(10)]),
            "cell_means": {k: v.tolist() for k, v in cell.items()},
            "noise_identically_zero_share": float(np.mean(arrays["noise_delta"] == 0.0))}


def _print(report):
    for src, r in report["sources"].items():
        s, ci = r["summary"], r["bootstrap_ci"]
        print(f"\n[{src}] ratio nodes {np.round(report['nodes'][src], 4).tolist()}")
        for k in ("sniper_rent", "maker_delta", "noise_delta", "tax", "maker_share"):
            print(f"  {k:12s} p10..p90 " + " ".join(f"{v:9.4g}" for v in s[k]))
        print(f"  P(makers bear majority) {s['p_makers_bear_majority']:.3f} "
              f"[{ci['p_makers_bear_majority']['lo']:.3f}, {ci['p_makers_bear_majority']['hi']:.3f}]"
              f"  P(no snipe) {s['p_no_snipe']:.3f}  maker share of mean tax "
              f"{s['maker_share_of_mean_tax']:.3f}")
        print("  spread drivers:", json.dumps(r["spread_drivers_first_order"]))


def main(n_seeds=PILOT_SEEDS, horizon_s=PILOT_HORIZON_S, n_nodes=PILOT_NODES,
         workers=None, out_path=PILOT_OUT) -> dict:
    t0 = time.time()
    seeds = tuple(range(1, n_seeds + 1))
    grid = run_grid(seeds, horizon_s, n_nodes, workers)
    report = {
        "experiment": "exp14_incidence_priors", "status": STATUS,
        "seeds": list(seeds), "horizon_s": horizon_s, "ratio_nodes_per_prior": n_nodes,
        "units": "USD per second; shares dimensionless",
        "priors": {"snipe_ratio": {k: f"race_proxy_v2.json by_window_ms.0.{v}"
                                   for k, v in RATIO_SOURCES.items()},
                   "n_makers": dict(zip(MAKER_COUNTS, MAKER_PRIOR.tolist())),
                   "fee_tiers_maker_taker_bp": {f"VIP{i}": t for i, t in enumerate(FEE_TIERS)},
                   "fee_tier_prior": "uniform over VIP 0-9 for makers and snipers; "
                                     "noise traders at VIP 0"},
        "fixed": {k: v for k, v in x13.BASE_POINT.items()},
        "nodes": grid["nodes"],
        "sources": {src: analyse_source(a, n_nodes) for src, a in grid["arrays"].items()},
        "max_identity_residual": max(float(np.abs(a["identity_residual"]).max())
                                     for a in grid["arrays"].values()),
        "elapsed_s": None,
    }
    report["elapsed_s"] = round(time.time() - t0, 1)
    out_path = Path(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(report, indent=1, allow_nan=True) + "\n")
    _print(report)
    print(f"wrote {out_path} in {report['elapsed_s']}s")
    return report


if __name__ == "__main__":
    a = [int(v) if i != 1 else float(v) for i, v in enumerate(sys.argv[1:])]
    main(*a)
