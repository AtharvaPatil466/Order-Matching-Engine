#!/usr/bin/env python3
"""Experiment 4 — batch auction vs continuous LOB.

Research question: does a frequent batch auction reduce the welfare losses and
liquidity fragility caused by the HFT arms race, and what batch interval is
sufficient to eliminate most of the effect?

Operating point = Exp 1 (n_hft=3, mm_latency_us=3000, sens=0.1, decay=0.1). The
swept variable is batch_interval_ticks. The 'continuous' arm reuses the verified
C++ matching engine via the Exp 1 harness (LatencyScheduler); every finite
interval runs the Python BatchAuctionScheduler, which accumulates orders and
clears at one uniform price per OrderBook::discoverUncrossPrice. There is a
methodological seam between the two matchers — the batch_interval=1 point (a
per-tick call auction) is the bridge/control closest to continuous.

Each cell runs BOTH arms (no-HFT baseline + HFT treatment) at the same interval
so welfare deltas are matched. Every metric is a bootstrap CI across seeds.

Run: bcs_research/.venv/bin/python bcs_research/experiments/exp4_batch_auction.py
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_ROOT = _HERE.parent
for _d in ("build", "agents", "simulation", "metrics", "experiments"):
    _p = str(_ROOT / _d)
    if _p not in sys.path:
        sys.path.insert(0, _p)

import bcs_engine as be                                   # noqa: E402
from fundamental_value import FundamentalValueProcess     # noqa: E402
from noise_trader import NoiseTrader                      # noqa: E402
from bcs_market_maker import BCSMarketMaker               # noqa: E402
from hft_agent import HFTAgent                            # noqa: E402
from batch_auction import BatchAuctionScheduler           # noqa: E402
from baseline_metrics import compute_metrics              # noqa: E402
from liquidity_gap_detector import count_liquidity_gaps   # noqa: E402
from welfare_decomposition import decompose_welfare, mark_price  # noqa: E402

from exp1_primary import CFG, MM_ID, SYMBOL, _run         # noqa: E402  continuous arm
from exp2_latency_sweep import _with_fill_rate            # noqa: E402
from bootstrap import bootstrap_ci_table                  # noqa: E402

BATCH_INTERVALS = [1, 5, 10, 25, 50, 100, 500]            # + "continuous"
CONTINUOUS = "continuous"

CELL_METRICS = (
    "hft_rent", "hft_rent_realized", "hft_rent_inventory",
    "mm_pnl", "nt_pnl", "zero_sum_residual",
    "liquidity_gaps", "kyle_lambda", "spread_ticks", "n_trades",
    "hft_snipes", "hft_fills", "hft_fill_rate",
)
DELTA_METRICS = ("mm_pnl", "nt_pnl", "liquidity_gaps")


def _run_batch(seed: int, cfg: dict, batch_interval: int, with_hft: bool) -> dict:
    """Mirror of exp1._run but driven by the BatchAuctionScheduler (no C++ engine)."""
    v0 = 100 * be.PRICE_PRECISION
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
    sched = BatchAuctionScheduler(SYMBOL, cfg["dt_us"], batch_interval)
    result = sched.run([mm] + noise + hfts, fund, cfg["duration_us"])

    m = compute_metrics(result, MM_ID, nt_ids)
    welfare = decompose_welfare(result, MM_ID, nt_ids, hft_ids, mark=mark_price(result))
    gaps = count_liquidity_gaps(result.snapshots, cfg["dt_us"], cfg["sigma"])
    return {
        "mm_pnl": welfare["mm_pnl"], "nt_pnl": welfare["noise_trader_pnl"],
        "hft_rent": welfare["hft_rent"], "hft_rent_per_sec": welfare["hft_rent_per_sec"],
        "hft_rent_realized": welfare["hft_rent_realized"],
        "hft_rent_inventory": welfare["hft_rent_inventory"],
        "zero_sum_residual": welfare["zero_sum_residual"],
        "liquidity_gaps": gaps, "kyle_lambda": m["kyle_lambda"],
        "spread_ticks": m["time_weighted_avg_spread_ticks"], "n_trades": m["n_trades"],
        "mm_max_half_spread": mm.max_half_spread_seen, "mm_min_qty": mm.min_quote_qty_seen,
        "hft_snipes": sum(a.snipes for a in hfts), "hft_fills": sum(a.fills for a in hfts),
    }


def _arm(interval, seed, cfg, with_hft):
    """Dispatch: continuous -> verified C++ engine; finite -> batch scheduler."""
    if interval == CONTINUOUS:
        row = _run(seed, cfg, with_hft=with_hft)
    else:
        row = _run_batch(seed, cfg, interval, with_hft=with_hft)
    return _with_fill_rate(row)


def run_cell(interval, cfg: dict, n_seeds: int, boot_seed: int) -> dict:
    seeds = range(1, n_seeds + 1)
    treat = [_arm(interval, s, cfg, True) for s in seeds]
    base = [_arm(interval, s, cfg, False) for s in seeds]
    deltas = [{k: treat[i][k] - base[i][k] for k in DELTA_METRICS}
              for i in range(n_seeds)]
    return {
        "batch_interval": interval,
        "n_seeds": n_seeds,
        "treatment_ci": bootstrap_ci_table(treat, CELL_METRICS, seed=boot_seed),
        "baseline_ci": bootstrap_ci_table(base, CELL_METRICS, seed=boot_seed),
        "delta_ci": bootstrap_ci_table(deltas, DELTA_METRICS, seed=boot_seed),
        "treatment_rows": treat,
        "baseline_rows": base,
    }


def _print_summary(report: dict) -> None:
    cont = next(c for c in report["cells"] if c["batch_interval"] == CONTINUOUS)
    cont_rent = cont["treatment_ci"]["hft_rent"]["mean"]
    print(f"Experiment 4 — batch auction vs continuous ({report['n_seeds']} seeds, "
          f"n_hft={report['config']['n_hft']})")
    cols = ("interval", "hft_rent", "rent_lo", "rent_hi", "rent_elim%",
            "dMM_pnl", "liq_gaps", "fill_rt")
    print("".join(f"{c:>11}" for c in cols))
    for cell in report["cells"]:
        t, d = cell["treatment_ci"], cell["delta_ci"]
        rent = t["hft_rent"]["mean"]
        elim = 100.0 * (1.0 - rent / cont_rent) if cont_rent else 0.0
        iv = cell["batch_interval"]
        iv_s = iv if isinstance(iv, str) else str(iv)
        vals = (iv_s, f"{rent:.3f}", f"{t['hft_rent']['lower']:.3f}",
                f"{t['hft_rent']['upper']:.3f}", f"{elim:.1f}",
                f"{d['mm_pnl']['mean']:.3f}", f"{t['liquidity_gaps']['mean']:.3f}",
                f"{t['hft_fill_rate']['mean']:.4f}")
        print("".join(f"{v:>11}" for v in vals))


def main(n_seeds: int = 100, intervals: list | None = None,  # was 20 (P3-13)
         cfg: dict | None = None, boot_seed: int = 0) -> dict:
    cfg = {**CFG, **(cfg or {})}
    intervals = (intervals or BATCH_INTERVALS) + [CONTINUOUS]
    cells = [run_cell(iv, cfg, n_seeds, boot_seed) for iv in intervals]
    report = {
        "config": cfg,
        "n_seeds": n_seeds,
        "intervals": intervals,
        "cells": cells,
    }

    out_dir = _ROOT / "results" / "experiments"
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "exp4_batch_auction.json").write_text(
        json.dumps(report, indent=2, sort_keys=True)
    )
    _print_summary(report)
    return report


if __name__ == "__main__":
    main()
