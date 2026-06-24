#!/usr/bin/env python3
"""Experiment 1 (partial) — do latency-advantaged HFTs move the two numbers a
no-HFT baseline pins at zero: Kyle's lambda and endogenous crashes?

Two arms, identical market parameters, differing ONLY in the presence of HFTs:
  baseline  = BaselineMarketMaker (latency-disadvantaged) + noise traders
  treatment = baseline + N HFTAgents (latency advantage; snipe stale quotes)

The MM is the UNCHANGED BaselineMarketMaker (no adverse-selection feedback yet
— that is Step 2). Expectation: lambda goes positive and the HFTs extract PnL
from the MM (adverse selection). Crash AMPLIFICATION needs the Step-2 MM, so
crashes may remain near zero here; this run validates the HFT agent itself.

Run: bcs_research/.venv/bin/python bcs_research/experiments/exp1_primary.py
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

import bcs_engine as be                                # noqa: E402
from fundamental_value import FundamentalValueProcess  # noqa: E402
from noise_trader import NoiseTrader                   # noqa: E402
from market_maker import BaselineMarketMaker           # noqa: E402
from hft_agent import HFTAgent                         # noqa: E402
from scheduler import LatencyScheduler                 # noqa: E402
from baseline_metrics import compute_metrics           # noqa: E402
from flash_crash_detector import count_crashes         # noqa: E402

SYMBOL = 1
MM_ID = 1

CFG = dict(
    duration_us=3_000_000,
    dt_us=1000,
    n_noise=8,
    n_hft=3,
    half_spread=50,          # ticks; base spread = 100 ticks = $0.01
    quote_qty=50,
    sigma=20.0,              # large enough that V drifts ~half_spread within MM latency
    mm_latency_us=3000,      # MM observes + posts late -> stale quotes
    mm_inventory_skew=0.2,
    hft_latency_us=0,        # HFT sees V immediately (latency advantage)
    hft_race_noise_us=100,
    hft_qty=50,
    hft_cost_bps=0.0,
    lambda_per_tick=0.15,
    noise_qty=10,
)


def _mark(result):
    for s in reversed(result.snapshots):
        if s["best_bid"] > 0 and s["best_ask"] > 0:
            return s["mid"]
    return result.snapshots[-1]["v"] if result.snapshots else 0.0


def _run(seed, cfg, with_hft):
    v0 = 100 * be.PRICE_PRECISION
    h = be.EngineHarness()
    h.add_symbol(SYMBOL)
    h.start()
    fund = FundamentalValueProcess(v0=v0, sigma=cfg["sigma"], dt_us=cfg["dt_us"], seed=seed)
    mm = BaselineMarketMaker(MM_ID, half_spread=cfg["half_spread"], quote_qty=cfg["quote_qty"],
                             latency_us=cfg["mm_latency_us"], inventory_skew=cfg["mm_inventory_skew"])
    noise = [NoiseTrader(100 + i, lambda_per_tick=cfg["lambda_per_tick"],
                         qty=cfg["noise_qty"], seed=seed * 1000 + i)
             for i in range(cfg["n_noise"])]
    hfts = []
    if with_hft:
        hfts = [HFTAgent(200 + j, latency_us=cfg["hft_latency_us"],
                         race_noise_us=cfg["hft_race_noise_us"],
                         transaction_cost_bps=cfg["hft_cost_bps"],
                         qty=cfg["hft_qty"], seed=seed * 2000 + j)
                for j in range(cfg["n_hft"])]
    sched = LatencyScheduler(h, SYMBOL, cfg["dt_us"])
    result = sched.run([mm] + noise + hfts, fund, cfg["duration_us"])
    m = compute_metrics(result, MM_ID, [a.participant_id for a in noise])
    crashes = count_crashes(result.snapshots, cfg["dt_us"], cfg["sigma"], 2 * cfg["half_spread"])
    mark = _mark(result)
    hft_pnl = sum(a.pnl(mark) for a in hfts)
    out = {
        "kyle_lambda": m["kyle_lambda"],
        "crashes": crashes,
        "n_trades": m["n_trades"],
        "spread_ticks": m["time_weighted_avg_spread_ticks"],
        "mm_pnl": m["mm_pnl_dollars"],
        "nt_pnl": m["noise_trader_pnl_dollars"],
        "hft_pnl": hft_pnl,
        "hft_snipes": sum(a.snipes for a in hfts),
        "hft_fills": sum(a.fills for a in hfts),
    }
    h.stop()
    return out


def _agg(rows):
    keys = rows[0].keys()
    return {k: float(statistics.fmean([r[k] for r in rows])) for k in keys}


def main(n_seeds=10, cfg=None):
    cfg = {**CFG, **(cfg or {})}
    base = _agg([_run(s, cfg, with_hft=False) for s in range(1, n_seeds + 1)])
    treat = _agg([_run(s, cfg, with_hft=True) for s in range(1, n_seeds + 1)])
    report = {"config": cfg, "n_seeds": n_seeds, "baseline": base, "treatment": treat}

    out_dir = _ROOT / "results" / "experiments"
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "exp1_partial.json").write_text(json.dumps(report, indent=2, sort_keys=True))

    print(f"Experiment 1 (partial) — {n_seeds} seeds, n_hft={cfg['n_hft']}, "
          f"mm_latency={cfg['mm_latency_us']}us, sigma={cfg['sigma']}")
    print(f"{'metric':<16}{'baseline':>14}{'treatment':>14}")
    for k in ("kyle_lambda", "crashes", "spread_ticks", "n_trades",
              "mm_pnl", "nt_pnl", "hft_pnl", "hft_snipes", "hft_fills"):
        print(f"{k:<16}{base[k]:>14.4f}{treat[k]:>14.4f}")
    return report


if __name__ == "__main__":
    main()
