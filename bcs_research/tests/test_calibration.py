"""Unit tests for the calibration harness moment math and inversion."""
import math
import sys
import pathlib

_R = pathlib.Path(__file__).resolve().parents[1]
for _d in ("calibration",):
    sys.path.insert(0, str(_R / _d))

from targets import TAU_S, book_moments, trade_moments
from fit import fit_params


def test_trade_moments_arrival_rate_and_buyer_fraction():
    # Arrange: 101 trades, one per 100 ms -> 10 s span, 10.1 trades/s.
    ts = [i * 100_000_000 for i in range(101)]
    sizes = [0.01] * 101
    ibm = [True] * 25 + [False] * 76        # False = aggressive buy

    m = trade_moments([(ts, sizes, ibm)])

    assert abs(m["lambda_per_s"] - 10.1) < 1e-9
    assert abs(m["buyer_fraction"] - 76 / 101) < 1e-9
    assert m["size_p50"] == 0.01


def test_trade_moments_sums_durations_per_file():
    f = ([0, 10_000_000_000], [1.0, 2.0], [True, False])    # 10 s each
    m = trade_moments([f, f])
    assert abs(m["duration_s"] - 20.0) < 1e-9
    assert abs(m["lambda_per_s"] - 4 / 20) < 1e-9


def test_trade_moments_ignores_the_gap_between_files():
    # Two 10 s files an hour apart. Per-file spans total 20 s, so the rate is
    # 4/20; charging the idle hour to the denominator would give 4/3620.
    early = ([0, 10_000_000_000], [1.0, 2.0], [True, False])
    late = ([3_600_000_000_000, 3_610_000_000_000], [1.0, 2.0], [True, False])

    m = trade_moments([early, late])

    assert abs(m["duration_s"] - 20.0) < 1e-9
    assert abs(m["lambda_per_s"] - 0.2) < 1e-9


def test_book_moments_spread_and_vol():
    # Arrange: mid alternates +-0.05% each second; spread constant 1.0 on mid 100.
    n = 200
    ts = [i * 1_000_000_000 for i in range(n)]
    mid = [100.0 * (1.0005 if i % 2 else 0.9995) for i in range(n)]
    spread = [1.0] * n
    depth = [2.0] * n

    m = book_moments(ts, mid, spread, depth, depth)

    assert abs(m["spread_p50_bp"] - 1.0 / 100.0 * 1e4) < 1.0   # ~100 bp
    assert m["top_depth_p50"] == 2.0
    assert m["rv_1s_bp"] > 0
    assert m["n_snapshots"] == n


def test_book_moments_drops_returns_spanning_a_gap():
    # Two 100 s blocks of steady +1 bp/s drift, an hour apart, across which
    # the mid doubles. The cross-gap jump must not enter the vol.
    def block(start_s, base):
        ts = [(start_s + i) * 1_000_000_000 for i in range(100)]
        mid = [base * (1.0001 ** i) for i in range(100)]
        return ts, mid

    ts_a, mid_a = block(0, 100.0)
    ts_b, mid_b = block(3600, 200.0)
    ts, mid = ts_a + ts_b, mid_a + mid_b
    flat = [1.0] * len(ts)

    m = book_moments(ts, mid, flat, flat, flat)

    # Every kept return is exactly +1 bp, so the stdev is ~0; the 2x jump
    # would swamp it if the gap-spanning pair were included.
    assert m["rv_1s_bp"] < 1e-6


def test_fit_params_inversion():
    targets = {"lambda_per_s": 6.2, "rv_1s_bp": 0.33}
    f = fit_params(targets, n_noise=8, v0=1_000_000)
    assert abs(f["lambda_per_tick"] - 6.2 * TAU_S / 8) < 1e-12
    assert abs(f["sigma"] - 0.33 * math.sqrt(TAU_S) * 1e-4 * 1_000_000) < 1e-9


def test_size_model_matches_p50_p99_and_reports_p90_gap():
    from fit import size_model
    targets = {"size_p50": 0.003, "size_p90": 0.201, "size_p99": 2.214,
               "top_depth_p50": 6.1565}
    sm = size_model(targets)
    # p99/p50 reproduced exactly by construction.
    assert abs(math.exp(2.326348 * sm["sigma_ln"]) - 2.214 / 0.003) < 1e-6
    assert sm["quote_qty_units"] == round(6.1565 / 0.003)
    # Two-quantile lognormal under-predicts p90 -- disclosed, not hidden.
    assert sm["p90_ratio_model"] < sm["p90_ratio_real"]


def test_noise_trader_lognormal_sizes():
    for _d in ("agents", "simulation", "build"):
        p = str(_R / _d)
        if p not in sys.path:
            sys.path.insert(0, p)
    from noise_trader import NoiseTrader

    nt = NoiseTrader(100, qty=10, size_sigma_ln=2.84, seed=7)
    draws = [nt._draw_qty() for _ in range(4000)]
    assert min(draws) >= 1                              # integer floor
    assert sorted(draws)[2000] <= 30                    # median near qty=10
    assert max(draws) > 500                             # heavy tail present
    nt2 = NoiseTrader(100, qty=10, size_sigma_ln=2.84, seed=7)
    assert draws[:50] == [nt2._draw_qty() for _ in range(50)]  # seeded determinism

    # Default path unchanged: constant qty, no RNG consumed by sizing.
    const = NoiseTrader(101, qty=10, seed=7)
    assert [const._draw_qty() for _ in range(5)] == [10] * 5
