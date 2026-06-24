#!/usr/bin/env python3
"""Phase 1 — no-HFT baseline (the control group).

Runs N independent seeds of a market with one BaselineMarketMaker and several
NoiseTraders (NO HFT agent) and writes results/baseline/baseline_metrics.json.
The BCS treatment (Phase 2+) is measured against these numbers. By construction
there is no endogenous-crash mechanism, so endogenous_crashes == 0.

Run:  bcs_research/.venv/bin/python bcs_research/experiments/run_baseline.py
"""
from __future__ import annotations

import json
import statistics
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_ROOT = _HERE.parent
for _d in ("build", "agents", "simulation", "metrics"):
    _p = str(_ROOT / _d)
    if _p not in sys.path:
        sys.path.insert(0, _p)

import bcs_engine as be                              # noqa: E402
from fundamental_value import FundamentalValueProcess  # noqa: E402
from noise_trader import NoiseTrader                 # noqa: E402
from market_maker import BaselineMarketMaker         # noqa: E402
from scheduler import LatencyScheduler               # noqa: E402
from baseline_metrics import compute_metrics         # noqa: E402

SYMBOL = 1
MM_ID = 1

DEFAULTS = dict(
    duration_us=2_000_000,   # 2s of sim time at dt=1ms -> 2000 ticks
    dt_us=1000,
    n_noise=8,
    half_spread=100,         # fixed-point ticks -> $0.01 half-spread
    quote_qty=50,
    sigma=2.0,               # small: spread cost dominates inventory MTM noise
    inventory_skew=0.2,
    lambda_per_tick=0.15,
    noise_qty=10,
)


def run_one(seed, cfg):
    v0 = 100 * be.PRICE_PRECISION
    h = be.EngineHarness()
    h.add_symbol(SYMBOL)
    h.start()
    fundamental = FundamentalValueProcess(v0=v0, sigma=cfg["sigma"],
                                          dt_us=cfg["dt_us"], seed=seed)
    mm = BaselineMarketMaker(MM_ID, half_spread=cfg["half_spread"],
                             quote_qty=cfg["quote_qty"],
                             inventory_skew=cfg["inventory_skew"])
    noise = [NoiseTrader(100 + i, lambda_per_tick=cfg["lambda_per_tick"],
                         qty=cfg["noise_qty"], seed=seed * 1000 + i)
             for i in range(cfg["n_noise"])]
    sched = LatencyScheduler(h, SYMBOL, cfg["dt_us"])
    result = sched.run([mm] + noise, fundamental, cfg["duration_us"])
    metrics = compute_metrics(result, MM_ID, [a.participant_id for a in noise])
    h.stop()
    return metrics


def aggregate(n_seeds=20, cfg=None):
    cfg = {**DEFAULTS, **(cfg or {})}
    per_seed = [run_one(seed, cfg) for seed in range(1, n_seeds + 1)]
    numeric = [k for k, v in per_seed[0].items() if isinstance(v, (int, float))]
    summary = {}
    for k in numeric:
        vals = [m[k] for m in per_seed]
        summary[k] = {
            "mean": float(statistics.fmean(vals)),
            "std": float(statistics.pstdev(vals)) if len(vals) > 1 else 0.0,
            "min": float(min(vals)),
            "max": float(max(vals)),
        }
    return {"config": cfg, "n_seeds": n_seeds, "summary": summary,
            "per_seed": per_seed}


def main():
    report = aggregate()
    out_dir = _ROOT / "results" / "baseline"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / "baseline_metrics.json"
    out_path.write_text(json.dumps(report, indent=2, sort_keys=True))
    s = report["summary"]
    print(f"Wrote {out_path}  ({report['n_seeds']} seeds)")
    for k in ("time_weighted_avg_spread_ticks", "n_trades", "total_volume",
              "noise_trader_pnl_per_sec", "mm_pnl_per_sec", "kyle_lambda",
              "reversals_per_sec", "endogenous_crashes"):
        if k in s:
            print(f"  {k}: mean={s[k]['mean']:.4f} std={s[k]['std']:.4f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
