#!/usr/bin/env python3
"""Research step 4 — competing makers with heterogeneous latencies.

Why this exists. In every experiment so far a SINGLE latency-disadvantaged
BCSMarketMaker supplies the whole top of book, so when its quote is sniped there
is no competitor to replenish it: one maker absorbs the entire race. That is why
paper §4.6 reports the calibrated 4.42 bp as an UPPER bound, not an estimate.

Real books are not like that. Step 3a measured top-of-book replenishment on 7.2
days of BTCUSDT-perp: half-recovery median 0.31 s with a p25 at the 0.10 s
snapshot floor — at least a quarter of depletions replenish inside 100 ms. A lone
maker carrying 300 ms of quote staleness (mm_latency_us=3000 = 3 sim-ticks, one
tick = 100 ms by the paper's convention) cannot produce that fast quartile; a set
of makers with heterogeneous latencies can. This driver replaces the lone maker
with N makers whose latencies span 100-500 ms, splitting the aggregate quote so
top-of-book DEPTH is held constant, and measures how HFT rent and its incidence
move as competitive supply replaces the single absorber.

What it does and does NOT claim (plan step 4). It removes the single-maker
absorption bias. It does not turn the rent into an estimate: the race structure
(hft_latency_us, hft_race_noise_us) stays a swept design parameter, since
measuring real races needs failed-order message data of the kind Aquilina,
Budish & O'Neill (2022) obtained from a regulator. Maker count N is itself a
design parameter, not calibrated — Binance's depth feed publishes quantity per
level with no order counts (step 3a) — so N is swept and the latency SET is
anchored on step 3a's recovery quantiles.

N=1 is left at exactly the paper's single maker (3 sim-ticks), so the count sweep
NESTS §4.6's published upper bound as its first row.

Run: bcs_research/.venv/bin/python bcs_research/experiments/exp4_competing_makers.py [n_seeds]
"""
from __future__ import annotations

import json
import statistics
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_ROOT = _HERE.parent
for _d in ("build", "agents", "simulation", "metrics", "experiments"):
    _p = str(_ROOT / _d)
    if _p not in sys.path:
        sys.path.insert(0, _p)

from exp1_primary import CFG, _run                # noqa: E402
from run_calibrated import calibrated_cfg         # noqa: E402

MAKER_COUNTS = (1, 2, 3, 5)

# Latency set per maker count, in microseconds. 1 sim-tick = 1000 us, read as
# 100 ms real (paper convention). Anchored on step 3a: fastest maker 1 tick
# (100 ms = the p25 recovery quartile a lone 3-tick maker cannot reach), median
# maker 3 ticks (300 ms = the real median), slowest 5 ticks. N=1 stays at the
# single maker's 3 ticks so the sweep nests the published upper bound.
LAT_FAST_US = 1000
LAT_SLOW_US = 5000
LAT_SINGLE_US = 3000


def latency_set(n: int) -> list[int]:
    if n == 1:
        return [LAT_SINGLE_US]
    step = (LAT_SLOW_US - LAT_FAST_US) / (n - 1)
    return [round(LAT_FAST_US + step * i) for i in range(n)]


def _mean(xs) -> float:
    return float(statistics.fmean(xs)) if xs else 0.0


def run_count(n: int, cfg: dict, n_seeds: int) -> dict:
    lats = latency_set(n)
    c = {**cfg, "mm_latencies_us": lats}
    base = [_run(s, c, with_hft=False, per_maker=True) for s in range(1, n_seeds + 1)]
    treat = [_run(s, c, with_hft=True, per_maker=True) for s in range(1, n_seeds + 1)]

    rent = _mean([r["hft_rent"] for r in treat])
    notional = _mean([r["total_notional"] for r in treat])
    # Per-maker PnL averaged across seeds, keyed by that maker's latency (us).
    maker_pnl = {str(lats[i]): _mean([r["maker_pnls"][i] for r in treat])
                 for i in range(n)}
    return {
        "n_makers": n,
        "latencies_us": lats,
        "hft_rent": rent,
        "hft_rent_bp": 1e4 * rent / notional if notional else 0.0,
        "mm_pnl_total": _mean([r["mm_pnl"] for r in treat]),
        "nt_pnl": _mean([r["nt_pnl"] for r in treat]),
        "maker_pnl_by_latency": maker_pnl,
        "zero_sum_residual": _mean([r["zero_sum_residual"] for r in treat]),
        "liquidity_gaps_base": _mean([r["liquidity_gaps"] for r in base]),
        "liquidity_gaps_treat": _mean([r["liquidity_gaps"] for r in treat]),
        "hft_snipes": _mean([r["hft_snipes"] for r in treat]),
        "traded_notional": notional,
    }


def main(n_seeds: int = 100, hft_qty=None,
         out_name: str = "exp4_competing_makers.json") -> dict:
    cfg = {**CFG, **calibrated_cfg(hft_qty=hft_qty)}
    ratio = cfg["hft_qty"] / cfg["quote_qty"]
    print(f"Competing makers — calibrated flow, snipe/quote ratio {ratio:.4f}, "
          f"{n_seeds} seeds")
    print(f"{'N':>3}  {'latencies (us)':<24}{'rent':>10}{'rent bp':>10}"
          f"{'gaps':>8}{'residual':>12}")
    rows = []
    for n in MAKER_COUNTS:
        row = run_count(n, cfg, n_seeds)
        rows.append(row)
        print(f"{n:>3}  {str(row['latencies_us']):<24}{row['hft_rent']:>10.2f}"
              f"{row['hft_rent_bp']:>10.3f}{row['liquidity_gaps_treat']:>8.2f}"
              f"{row['zero_sum_residual']:>12.2e}", flush=True)

    report = {"config": cfg, "n_seeds": n_seeds,
              "snipe_quote_ratio": ratio, "counts": rows}
    out_dir = _ROOT / "results" / "experiments"
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / out_name).write_text(json.dumps(report, indent=2, sort_keys=True))
    print(f"\nwrote results/experiments/{out_name}")
    return report


if __name__ == "__main__":
    main(n_seeds=int(sys.argv[1]) if len(sys.argv) > 1 else 100)
