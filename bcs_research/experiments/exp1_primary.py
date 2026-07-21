#!/usr/bin/env python3
"""Experiment 1 (primary) — the BCS arms-race result on the verified engine.

Two arms, identical market parameters, differing ONLY in the presence of HFTs:
  baseline  = BCSMarketMaker (latency-disadvantaged, adverse-selection-aware)
              + noise traders
  treatment = baseline + N HFTAgents (latency advantage; snipe stale quotes)

The MM is the adverse-selection BCSMarketMaker (widens only on informed fills;
thins liquidity as it widens). PRIMARY effect = the welfare transfer (BCS Step
8): HFT rent extracted, MM PnL eroded, noise-trader losses. Endogenous
instability is measured as LIQUIDITY GAPS (one-sided/empty-book episodes with V
flat, then recovery) rather than mid-based flash crashes, because the thinning
mechanism produces gaps, not mid dislocations.

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
from bcs_market_maker import BCSMarketMaker            # noqa: E402
from hft_agent import HFTAgent                         # noqa: E402
from scheduler import LatencyScheduler                 # noqa: E402
from baseline_metrics import compute_metrics           # noqa: E402
from liquidity_gap_detector import count_liquidity_gaps  # noqa: E402
from welfare_decomposition import decompose_welfare, welfare_delta, mark_price  # noqa: E402

SYMBOL = 1
MM_ID = 1

CFG = dict(
    duration_us=3_000_000,
    dt_us=1000,
    n_noise=8,
    n_hft=3,
    base_half_spread=50,         # ticks; base spread = 100 ticks = $0.01
    quote_qty=50,
    sigma=20.0,
    mm_latency_us=3000,          # MM observes + posts late -> stale quotes
    mm_inventory_skew=0.2,
    adverse_sensitivity=0.1,     # calibrated: no-HFT spread ~1.1x base (stable)
    spread_decay=0.1,
    spread_cap_mult=10.0,
    min_quote_qty=1,
    hft_latency_us=0,            # HFT sees V immediately (latency advantage)
    hft_race_noise_us=100,
    hft_qty=50,
    hft_cost_bps=0.0,
    lambda_per_tick=0.15,
    noise_qty=10,
)


def _run(seed, cfg, with_hft):
    v0 = 100 * be.PRICE_PRECISION
    h = be.EngineHarness()
    h.add_symbol(SYMBOL)
    h.start()
    fund = FundamentalValueProcess(v0=v0, sigma=cfg["sigma"], dt_us=cfg["dt_us"], seed=seed)
    mm = BCSMarketMaker(MM_ID, base_half_spread=cfg["base_half_spread"],
                        quote_qty=cfg["quote_qty"], latency_us=cfg["mm_latency_us"],
                        inventory_skew=cfg["mm_inventory_skew"],
                        adverse_sensitivity=cfg["adverse_sensitivity"],
                        spread_decay=cfg["spread_decay"],
                        spread_cap_mult=cfg["spread_cap_mult"],
                        min_quote_qty=cfg["min_quote_qty"])
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
    nt_ids = [a.participant_id for a in noise]
    hft_ids = [a.participant_id for a in hfts]
    sched = LatencyScheduler(h, SYMBOL, cfg["dt_us"])
    result = sched.run([mm] + noise + hfts, fund, cfg["duration_us"])

    m = compute_metrics(result, MM_ID, nt_ids)
    welfare = decompose_welfare(result, MM_ID, nt_ids, hft_ids, mark=mark_price(result))
    gaps = count_liquidity_gaps(result.snapshots, cfg["dt_us"], cfg["sigma"])
    h.stop()
    return {
        # welfare (primary)
        "mm_pnl": welfare["mm_pnl"],
        "nt_pnl": welfare["noise_trader_pnl"],
        "hft_rent": welfare["hft_rent"],
        "hft_rent_realized": welfare["hft_rent_realized"],
        "hft_rent_inventory": welfare["hft_rent_inventory"],
        "hft_rent_per_sec": welfare["hft_rent_per_sec"],
        "zero_sum_residual": welfare["zero_sum_residual"],
        # instability + quality (secondary)
        "liquidity_gaps": gaps,
        "kyle_lambda": m["kyle_lambda"],
        "spread_ticks": m["time_weighted_avg_spread_ticks"],
        "n_trades": m["n_trades"],
        "mm_max_half_spread": mm.max_half_spread_seen,
        "mm_min_qty": mm.min_quote_qty_seen,
        "hft_snipes": sum(a.snipes for a in hfts),
        "hft_fills": sum(a.fills for a in hfts),
    }


def _agg(rows):
    keys = rows[0].keys()
    return {k: float(statistics.fmean([r[k] for r in rows])) for k in keys}


def main(n_seeds=100, cfg=None):  # was 10 (P3-13): n>=100 for narrower CIs
    cfg = {**CFG, **(cfg or {})}
    base = _agg([_run(s, cfg, with_hft=False) for s in range(1, n_seeds + 1)])
    treat = _agg([_run(s, cfg, with_hft=True) for s in range(1, n_seeds + 1)])
    delta = welfare_delta(
        {"mm_pnl": base["mm_pnl"], "noise_trader_pnl": base["nt_pnl"],
         "hft_pnl": base["hft_rent"], "hft_rent": base["hft_rent"],
         "noise_trader_loss": -base["nt_pnl"], "mm_pnl_per_sec": 0.0,
         "hft_rent_per_sec": base["hft_rent_per_sec"]},
        {"mm_pnl": treat["mm_pnl"], "noise_trader_pnl": treat["nt_pnl"],
         "hft_pnl": treat["hft_rent"], "hft_rent": treat["hft_rent"],
         "noise_trader_loss": -treat["nt_pnl"], "mm_pnl_per_sec": 0.0,
         "hft_rent_per_sec": treat["hft_rent_per_sec"]})
    report = {"config": cfg, "n_seeds": n_seeds, "baseline": base,
              "treatment": treat, "welfare_delta": delta}

    out_dir = _ROOT / "results" / "experiments"
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "exp1_primary.json").write_text(json.dumps(report, indent=2, sort_keys=True))

    print(f"Experiment 1 (primary) — {n_seeds} seeds, n_hft={cfg['n_hft']}, "
          f"sens={cfg['adverse_sensitivity']}, decay={cfg['spread_decay']}")
    print(f"{'metric':<20}{'baseline':>14}{'treatment':>14}")
    for k in ("hft_rent", "mm_pnl", "nt_pnl", "liquidity_gaps", "kyle_lambda",
              "spread_ticks", "mm_max_half_spread", "mm_min_qty", "n_trades",
              "hft_snipes", "hft_fills", "zero_sum_residual"):
        print(f"{k:<20}{base[k]:>14.4f}{treat[k]:>14.4f}")
    print("\nWelfare transfer (treatment - baseline):")
    for k, v in delta.items():
        print(f"  {k:<22}{v:>14.4f}")
    return report


if __name__ == "__main__":
    main()
