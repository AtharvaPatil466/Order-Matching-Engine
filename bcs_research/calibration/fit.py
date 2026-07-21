#!/usr/bin/env python3
"""Closed-form fit of the sim's environment parameters to the BTC targets.

Round one fits ONLY the directly invertible environment parameters:

- lambda_per_tick = lambda_per_s * TAU_S / n_noise   (per noise trader)
- sigma           = rv_1s_bp * sqrt(TAU_S) * 1e-4 * v0   (fixed-point / sqrt-tick)

then runs the no-HFT baseline at the fitted parameters and prints target vs
simulated moments. What is NOT fitted is printed as a documented gap, not
hidden: the stylized MM quotes ~1 bp half-spreads by construction while the
perp trades at ~0.015 bp with top-depth-to-median-trade ratios in the
hundreds, and sim sizes are constants while real sizes are heavy-tailed
(p99/p50 ~ 500x). Matching those needs a richer MM/size model, not a knob.

Caveat on arrival units: real aggTrades aggregate a taker order's fills at
one price; sim trades are individual fills. The comparison treats both as
"taker events" — adequate at round-one precision.

Run: bcs_research/.venv/bin/python bcs_research/calibration/fit.py
"""
from __future__ import annotations

import json
import math
import statistics
import sys
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
for _d in ("build", "agents", "simulation", "metrics", "calibration"):
    _p = str(_ROOT / _d)
    if _p not in sys.path:
        sys.path.insert(0, _p)

from targets import TAU_S, OUT  # noqa: E402


def fit_params(targets: dict, n_noise: int, v0: int) -> dict:
    return {
        "lambda_per_tick": targets["lambda_per_s"] * TAU_S / n_noise,
        "sigma": targets["rv_1s_bp"] * math.sqrt(TAU_S) * 1e-4 * v0,
    }


def sim_moments(result, v0: int) -> dict:
    """Same moment definitions as targets.py, on a SimResult, real units."""
    dur_s = len(result.snapshots) * TAU_S
    two_sided = [s for s in result.snapshots if s["best_bid"] > 0 and s["best_ask"] > 0]
    spread_bp = sorted((s["best_ask"] - s["best_bid"]) / s["mid"] * 1e4
                       for s in two_sided)
    per_sec = max(1, round(1.0 / TAU_S))
    mids = [s["mid"] for i, s in enumerate(result.snapshots)
            if s["mid"] > 0 and i % per_sec == per_sec - 1]
    lr = [math.log(mids[i + 1] / mids[i]) for i in range(len(mids) - 1)]
    return {
        "lambda_per_s": len(result.trades) / dur_s,
        "spread_p50_bp": spread_bp[len(spread_bp) // 2] if spread_bp else float("nan"),
        "rv_1s_bp": statistics.stdev(lr) * 1e4 if len(lr) > 1 else float("nan"),
    }


def _baseline_run(seed: int, cfg: dict):
    """exp1's no-HFT baseline, returning the SimResult (exp1._run returns metrics)."""
    import bcs_engine as be
    from fundamental_value import FundamentalValueProcess
    from noise_trader import NoiseTrader
    from bcs_market_maker import BCSMarketMaker
    from scheduler import LatencyScheduler

    h = be.EngineHarness()
    h.add_symbol(1)
    h.start()
    v0 = 100 * be.PRICE_PRECISION
    fund = FundamentalValueProcess(v0=v0, sigma=cfg["sigma"], dt_us=cfg["dt_us"], seed=seed)
    mm = BCSMarketMaker(1, base_half_spread=cfg["base_half_spread"],
                        quote_qty=cfg["quote_qty"], latency_us=cfg["mm_latency_us"],
                        inventory_skew=cfg["mm_inventory_skew"],
                        adverse_sensitivity=cfg["adverse_sensitivity"],
                        spread_decay=cfg["spread_decay"],
                        spread_cap_mult=cfg["spread_cap_mult"],
                        min_quote_qty=cfg["min_quote_qty"])
    noise = [NoiseTrader(100 + i, lambda_per_tick=cfg["lambda_per_tick"],
                         qty=cfg["noise_qty"], seed=seed * 1000 + i)
             for i in range(cfg["n_noise"])]
    result = LatencyScheduler(h, 1, cfg["dt_us"]).run([mm] + noise, fund, cfg["duration_us"])
    h.stop()
    return result


def main(n_seeds: int = 5) -> dict:
    sys.path.insert(0, str(_ROOT / "experiments"))
    import bcs_engine as be
    from exp1_primary import CFG

    targets = json.loads((OUT / "targets_btcusdt.json").read_text())
    v0 = 100 * be.PRICE_PRECISION
    fitted = fit_params(targets, CFG["n_noise"], v0)
    cfg = {**CFG, **{"lambda_per_tick": fitted["lambda_per_tick"],
                     "sigma": fitted["sigma"]}}

    sims = [sim_moments(_baseline_run(s, cfg), v0) for s in range(1, n_seeds + 1)]
    sim = {k: statistics.fmean(r[k] for r in sims) for k in sims[0]}

    print(f"fitted: lambda_per_tick {fitted['lambda_per_tick']:.4f} "
          f"(was {CFG['lambda_per_tick']}), sigma {fitted['sigma']:.2f} "
          f"(was {CFG['sigma']})")
    print(f"{'moment':>16} {'target':>12} {'sim@fitted':>12}")
    for k in ("lambda_per_s", "rv_1s_bp", "spread_p50_bp"):
        print(f"{k:>16} {targets[k]:>12.4g} {sim[k]:>12.4g}")
    print("documented gaps (not fitted): spread level "
          f"(real {targets['spread_p50_bp']:.3g} bp vs sim {sim['spread_p50_bp']:.3g} bp), "
          f"depth/median-trade (real {targets['top_depth_p50'] / targets['size_p50']:.0f}x "
          f"vs sim {CFG['quote_qty'] / CFG['noise_qty']:.0f}x), "
          f"size tail (real p99/p50 {targets['size_p99'] / targets['size_p50']:.0f}x vs sim 1x)")

    report = {"targets_file": "targets_btcusdt.json", "n_seeds": n_seeds,
              "tau_s": TAU_S, "fitted": fitted,
              "prior": {k: CFG[k] for k in ("lambda_per_tick", "sigma")},
              "sim_moments_at_fit": sim,
              "targets": {k: targets[k] for k in ("lambda_per_s", "rv_1s_bp",
                                                  "spread_p50_bp", "top_depth_p50",
                                                  "size_p50", "size_p99")}}
    (OUT / "fit_btcusdt.json").write_text(json.dumps(report, indent=2, sort_keys=True))
    return report


if __name__ == "__main__":
    main()
