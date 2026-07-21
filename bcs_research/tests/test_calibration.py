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


def test_trade_moments_sums_durations_across_days():
    day = ([0, 10_000_000_000], [1.0, 2.0], [True, False])  # 10 s each
    m = trade_moments([day, day])
    assert abs(m["duration_s"] - 20.0) < 1e-9
    assert abs(m["lambda_per_s"] - 4 / 20) < 1e-9


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


def test_fit_params_inversion():
    targets = {"lambda_per_s": 6.2, "rv_1s_bp": 0.33}
    f = fit_params(targets, n_noise=8, v0=1_000_000)
    assert abs(f["lambda_per_tick"] - 6.2 * TAU_S / 8) < 1e-12
    assert abs(f["sigma"] - 0.33 * math.sqrt(TAU_S) * 1e-4 * 1_000_000) < 1e-9
