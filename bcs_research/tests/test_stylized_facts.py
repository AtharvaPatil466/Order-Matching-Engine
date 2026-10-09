"""stylized_facts: one source-agnostic yardstick, checked on synthetic markets."""
import math
import pathlib
import sys

import numpy as np

_R = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(_R / "analysis"))

from stylized_facts import combine, segment_stats  # noqa: E402


def _market(seconds=600, seed=0, mid0=100.0, spread=0.01):
    """10 Hz quotes on a random walk; one trade every 100 ms, alternating side."""
    rng = np.random.default_rng(seed)
    qt = np.arange(seconds * 10) / 10
    mid = mid0 * np.exp(np.cumsum(rng.standard_normal(qt.size)) * 1e-4 * math.sqrt(0.1))
    t = qt[1:] - 0.05
    buy = np.arange(t.size) % 2 == 0
    return t, mid[:-1], np.ones(t.size), buy, qt, mid - spread / 2, mid + spread / 2


def test_spread_vol_and_activity_on_a_known_market():
    # Arrange: 1 bp/sqrt(s) random-walk mid, 1 bp spread, 10 trades/s.
    facts = combine([segment_stats(*_market())])

    assert abs(facts["spread_bp"]["p50"] - 1.0) < 0.02       # 1 bp at mid 100
    assert abs(facts["returns"]["1.0"]["sigma_bp_per_sqrt_s"] - 1.0) < 0.1
    assert abs(facts["returns"]["0.1"]["lag1_autocorrelation"]) < 0.05
    assert facts["trades_per_s"]["p50"] == 10
    assert facts["interarrival_ms"]["p50"] == 100 and facts["interarrival_ms"]["cv"] < 1e-6


def test_alternating_signs_have_lag_one_autocorrelation_minus_one():
    facts = combine([segment_stats(*_market())])
    assert math.isclose(facts["trade_sign_autocorrelation"]["1"], -1.0, abs_tol=1e-3)
    assert math.isclose(facts["trade_sign_autocorrelation"]["2"], 1.0, abs_tol=1e-3)


def test_segments_never_produce_a_return_across_the_gap():
    # Two flat markets at 100 and 200: pooled, every return is still zero.
    def flat(level):
        t, px, q, buy, qt, _, _ = _market(120)
        return t, px, q, buy, qt, np.full(qt.size, level - 0.005), np.full(qt.size, level + 0.005)

    facts = combine([segment_stats(*flat(100.0)), segment_stats(*flat(200.0))])

    assert facts["returns"]["1.0"]["sigma_bp_per_sqrt_s"] == 0.0


def test_impact_is_positive_when_buying_moves_the_mid_up():
    # Arrange: one trade per second; the mid moves 1 bp per unit of signed size.
    seconds = 400
    rng = np.random.default_rng(3)
    side = rng.integers(0, 2, seconds).astype(bool)
    size = rng.uniform(0.5, 1.5, seconds)
    signed = np.where(side, size, -size)
    qt = np.arange(seconds * 10 + 1) / 10
    mid = 100 * np.exp(np.r_[0.0, np.cumsum(np.repeat(signed * 1e-4 / 10, 10))])
    t = np.arange(seconds) + 0.5

    facts = combine([segment_stats(t, mid[5::10][:seconds], size, side,
                                   qt, mid - 0.005, mid + 0.005)])

    assert facts["impact_bp_per_sd_of_signed_flow"]["0"] > 0.9
    deciles = facts["impact_bp_by_flow_decile_same_second"]
    assert deciles[0] < 0 < deciles[-1]
