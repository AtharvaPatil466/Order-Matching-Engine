#!/usr/bin/env python3
"""Calibrate the BCSMarketMaker adverse-selection feedback (Phase 2 Step 2).

The feedback has two knobs:
  adverse_sensitivity - multiplicative widen per fill (pickoff)
  spread_decay        - per-tick pull of the half-spread back toward base

We need a regime where, WITHOUT HFTs, the spread stays near base (the market
does not self-collapse from noise fills), but WITH HFTs the pickoff rate makes
the spread diverge and endogenous crashes appear. Decay too fast -> no
amplification even with HFTs; decay too slow -> collapse in every run. This
sweep finds the usable middle.

Reports, per (sensitivity, decay):
  spread_mult_noHFT  = no-HFT spread / base spread   (want ~1, definitely < 2)
  spread_mult_HFT    = with-HFT spread / base spread (want clearly > noHFT)
  crashes_HFT        = endogenous crashes with HFTs   (want > 0)
  hft_pnl, divergence = spread_mult_HFT - spread_mult_noHFT

Run: bcs_research/.venv/bin/python bcs_research/experiments/calibrate_adverse_selection.py
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
from flash_crash_detector import count_crashes         # noqa: E402
from liquidity_gap_detector import count_liquidity_gaps  # noqa: E402

SYMBOL = 1
MM_ID = 1

CFG = dict(
    duration_us=2_000_000,
    dt_us=1000,
    n_noise=8,
    n_hft=3,
    base_half_spread=50,     # base spread = 100 ticks
    quote_qty=50,
    sigma=20.0,
    mm_latency_us=3000,
    mm_inventory_skew=0.2,
    hft_latency_us=0,
    hft_race_noise_us=100,
    hft_qty=50,
    lambda_per_tick=0.15,
    noise_qty=10,
    spread_cap_mult=10.0,
)
BASE_SPREAD_TICKS = 2 * CFG["base_half_spread"]


def _run(seed, cfg, sensitivity, decay, with_hft):
    v0 = 100 * be.PRICE_PRECISION
    h = be.EngineHarness()
    h.add_symbol(SYMBOL)
    h.start()
    fund = FundamentalValueProcess(v0=v0, sigma=cfg["sigma"], dt_us=cfg["dt_us"], seed=seed)
    mm = BCSMarketMaker(MM_ID, base_half_spread=cfg["base_half_spread"],
                        quote_qty=cfg["quote_qty"], latency_us=cfg["mm_latency_us"],
                        inventory_skew=cfg["mm_inventory_skew"],
                        adverse_sensitivity=sensitivity, spread_decay=decay,
                        spread_cap_mult=cfg["spread_cap_mult"])
    noise = [NoiseTrader(100 + i, lambda_per_tick=cfg["lambda_per_tick"],
                         qty=cfg["noise_qty"], seed=seed * 1000 + i,
                         size_sigma_ln=cfg.get("size_sigma_ln"))
             for i in range(cfg["n_noise"])]
    hfts = []
    if with_hft:
        hfts = [HFTAgent(200 + j, latency_us=cfg["hft_latency_us"],
                         race_noise_us=cfg["hft_race_noise_us"],
                         qty=cfg["hft_qty"], seed=seed * 2000 + j)
                for j in range(cfg["n_hft"])]
    sched = LatencyScheduler(h, SYMBOL, cfg["dt_us"])
    result = sched.run([mm] + noise + hfts, fund, cfg["duration_us"])
    m = compute_metrics(result, MM_ID, [a.participant_id for a in noise])
    crashes = count_crashes(result.snapshots, cfg["dt_us"], cfg["sigma"], BASE_SPREAD_TICKS)
    gaps = count_liquidity_gaps(result.snapshots, cfg["dt_us"], cfg["sigma"])
    mark = None
    for s in reversed(result.snapshots):
        if s["best_bid"] > 0 and s["best_ask"] > 0:
            mark = s["mid"]
            break
    hft_pnl = sum(a.pnl(mark if mark else v0) for a in hfts)
    h.stop()
    return {
        "spread_mult": m["time_weighted_avg_spread_ticks"] / BASE_SPREAD_TICKS,
        "crashes": crashes,
        "liquidity_gaps": gaps,
        "hft_pnl": hft_pnl,
        "mm_max_half_spread_mult": mm.max_half_spread_seen / cfg["base_half_spread"],
        # Pickoff RATE is what adverse_sensitivity is implicitly tuned against:
        # widening is applied per adverse fill EVENT, so tripling order flow
        # triples the kicks per tick at unchanged sensitivity.
        "pickoffs_per_tick": mm.pickoffs / (cfg["duration_us"] // cfg["dt_us"]),
    }


SENSITIVITIES = [0.05, 0.1, 0.2]
DECAYS = [0.02, 0.05, 0.1, 0.2]


def main(n_seeds=5, cfg=None, sensitivities=None, decays=None,
         out_name="adverse_selection_calibration.json"):
    """Sweep (sensitivity, decay) and report the no-HFT / with-HFT spread split.

    `cfg` overrides the operating point (used to re-calibrate under calibrated
    BTC order flow, where the pickoff RATE is ~4x higher and the operating-point
    sensitivity drives the no-HFT spread to the cap). `out_name` keeps each
    regime's grid in its own file.
    """
    cfg = {**CFG, **(cfg or {})}
    sensitivities = sensitivities or SENSITIVITIES
    decays = decays or DECAYS
    rows = []
    for sens in sensitivities:
        for dec in decays:
            no = [_run(s, cfg, sens, dec, False) for s in range(1, n_seeds + 1)]
            yes = [_run(s, cfg, sens, dec, True) for s in range(1, n_seeds + 1)]
            row = {
                "adverse_sensitivity": sens,
                "spread_decay": dec,
                "spread_mult_noHFT": statistics.fmean([r["spread_mult"] for r in no]),
                "spread_mult_HFT": statistics.fmean([r["spread_mult"] for r in yes]),
                "crashes_noHFT": statistics.fmean([r["crashes"] for r in no]),
                "crashes_HFT": statistics.fmean([r["crashes"] for r in yes]),
                "gaps_noHFT": statistics.fmean([r["liquidity_gaps"] for r in no]),
                "gaps_HFT": statistics.fmean([r["liquidity_gaps"] for r in yes]),
                "pickoffs_per_tick_noHFT": statistics.fmean(
                    [r["pickoffs_per_tick"] for r in no]),
                "hft_pnl": statistics.fmean([r["hft_pnl"] for r in yes]),
            }
            row["divergence"] = row["spread_mult_HFT"] - row["spread_mult_noHFT"]
            rows.append(row)

    out_dir = _ROOT / "results" / "experiments"
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / out_name).write_text(
        json.dumps({"config": cfg, "n_seeds": n_seeds, "grid": rows},
                   indent=2, sort_keys=True))

    print(f"Adverse-selection calibration — {n_seeds} seeds/cell, base spread "
          f"{BASE_SPREAD_TICKS} ticks -> {out_name}")
    print(f"{'sens':>8}{'decay':>7}{'noHFT x':>9}{'HFT x':>8}{'diverg':>8}"
          f"{'P/tick':>8}{'gap_no':>8}{'gap_HFT':>9}{'hft_pnl':>11}")
    for r in rows:
        print(f"{r['adverse_sensitivity']:>8.4f}{r['spread_decay']:>7.2f}"
              f"{r['spread_mult_noHFT']:>9.2f}{r['spread_mult_HFT']:>8.2f}"
              f"{r['divergence']:>8.2f}{r['pickoffs_per_tick_noHFT']:>8.2f}"
              f"{r['gaps_noHFT']:>8.1f}{r['gaps_HFT']:>9.1f}{r['hft_pnl']:>11.2f}")
    return rows


if __name__ == "__main__":
    main()
