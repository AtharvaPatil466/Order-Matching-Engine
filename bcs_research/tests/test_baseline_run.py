"""End-to-end Phase 1 baseline: agents + scheduler driving the real engine.

These run the full pipeline through the compiled bcs_engine and assert the
control-group properties: the market functions and is two-sided, the quoted
spread equals twice the half-spread, MM/NT PnL is exactly zero-sum (they trade
only with each other), noise traders lose to the spread, and there are no
endogenous crashes by construction. conftest.py puts the packages on sys.path.
"""
import json

import bcs_engine as be
from fundamental_value import FundamentalValueProcess
from noise_trader import NoiseTrader
from market_maker import BaselineMarketMaker
from scheduler import LatencyScheduler
from baseline_metrics import compute_metrics

SYM = 1
MM_ID = 1


def _run(seed=1, duration_us=200_000, dt_us=1000, n_noise=6,
         half_spread=100, sigma=2.0, inventory_skew=0.2):
    v0 = 100 * be.PRICE_PRECISION
    h = be.EngineHarness()
    h.add_symbol(SYM)
    h.start()
    fund = FundamentalValueProcess(v0=v0, sigma=sigma, dt_us=dt_us, seed=seed)
    mm = BaselineMarketMaker(MM_ID, half_spread=half_spread, quote_qty=50,
                             inventory_skew=inventory_skew)
    noise = [NoiseTrader(100 + i, lambda_per_tick=0.2, qty=10, seed=seed * 1000 + i)
             for i in range(n_noise)]
    sched = LatencyScheduler(h, SYM, dt_us)
    result = sched.run([mm] + noise, fund, duration_us)
    metrics = compute_metrics(result, MM_ID, [a.participant_id for a in noise])
    h.stop()
    return result, metrics


def test_market_functions_and_is_two_sided():
    result, metrics = _run()
    assert metrics["n_trades"] > 0
    assert metrics["total_volume"] > 0
    two_sided = [s for s in result.snapshots
                 if s["best_bid"] > 0 and s["best_ask"] > 0]
    assert len(two_sided) > 0.9 * len(result.snapshots)


def test_quoted_spread_is_twice_half_spread():
    _, metrics = _run(half_spread=100)
    # MM is the only liquidity provider; inventory skew shifts both quotes
    # equally so the spread is exactly 2 * half_spread on every tick.
    assert metrics["time_weighted_avg_spread_ticks"] == 200.0
    assert metrics["time_weighted_avg_spread_dollars"] == 0.02


def test_pnl_is_zero_sum_mm_vs_noise():
    _, metrics = _run()
    assert abs(metrics["mm_pnl_dollars"] + metrics["noise_trader_pnl_dollars"]) < 1e-6


def test_noise_traders_lose_to_spread():
    # Small sigma -> spread cost dominates inventory MTM noise -> NT loses,
    # MM (the counterparty) gains the same amount.
    _, metrics = _run(duration_us=400_000, sigma=2.0)
    assert metrics["noise_trader_pnl_dollars"] < 0.0
    assert metrics["mm_pnl_dollars"] > 0.0


def test_no_endogenous_crashes_and_serializable():
    _, metrics = _run()
    assert metrics["endogenous_crashes"] == 0
    json.dumps(metrics)  # metrics must be JSON-serializable
