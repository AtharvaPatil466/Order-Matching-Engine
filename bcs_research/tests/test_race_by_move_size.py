"""race_by_move_size: thresholding races and moves by the size of the move.

The synthetic tape sits near 10,000 USD, where one 0.1 USD exchange tick is
~0.1 bp, so every move below is a whole number of ticks and lands well away
from a threshold edge.
"""
import pathlib
import sys

import numpy as np

_R = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(_R / "calibration"))

from fine_grain import implied_mid  # noqa: E402
from race_by_move_size import (LABELS, WINDOWS_MS, day_intervals,  # noqa: E402
                               exceed, hour_counts, model_snipeable,
                               operating_point_moves)
from race_proxy import WindowAccumulator  # noqa: E402

# label order: any, 0.1, 0.25, 0.5, 1.0, 1.7, 2.5, 5.0, fee + half-spread


def _tape():
    """Three races and one lone jump, all > 100 ms apart.

    A  ms 100   buy race, +2 ticks (0.2 bp): level-1 fill, then two lifts
    B  ms 1000  buy race, +30 ticks (3.0 bp); one more buyer at ms 1001
                (+1 tick) joins it from W = 1 ms and makes it a lower-bound race
    C  ms 2000  sell race, -7 ticks from the ms-1001 mid (0.7 bp)
    D  ms 3000  ONE buy record, +20 ticks (2.0 bp): a move, not a race
    """
    rows = [  # ts_ms, price, buy, size
        (0, 10000.05, True, 1.0),
        (100, 10000.05, True, 1.0), (100, 10000.15, True, 1.0), (100, 10000.25, True, 1.0),
        (1000, 10000.25, True, 2.0), (1000, 10003.25, True, 2.0), (1001, 10003.35, True, 1.0),
        (2000, 10003.15, False, 1.0), (2000, 10002.55, False, 1.0),
        (3000, 10004.65, True, 5.0),
        (4000, 10004.55, False, 1.0),
    ]
    ts, price, buy, size = map(np.array, zip(*rows))
    return {"ts_ms": ts.astype(np.int64), "price": price.astype(float),
            "buy": buy.astype(bool), "size": size.astype(float)}


def _w(window_ms):
    return WINDOWS_MS.index(window_ms)


def test_exceed_counts_and_weights_each_threshold_row():
    move = np.array([0.05, 0.3, 1.8, np.nan])
    hs = np.full(4, 0.2)                       # 1.8 - 0.2 < 1.7: fails fee + half-spread

    counts = exceed(move, hs)
    weighted = exceed(move, hs, weights=np.array([1.0, 2.0, 4.0, 8.0]))

    assert counts.tolist() == [4, 2, 2, 1, 1, 1, 0, 0, 0]
    assert weighted.tolist() == [15, 6, 6, 4, 4, 4, 0, 0, 0]
    assert len(LABELS) == counts.size


def test_races_and_moves_are_thresholded_by_the_move_they_follow():
    c = hour_counts(_tape())

    # W = 0: A (0.2), B (3.0), C (0.7); D is a lone record, so a move only.
    assert c["races"][_w(0)].tolist() == [3, 3, 2, 2, 1, 1, 1, 0, 1]
    assert c["episodes"][_w(0)].tolist() == [5, 4, 3, 3, 2, 2, 1, 0, 2]   # + D, + the 1-tick ms-1001 lift
    assert c["lb_races"][_w(0)].tolist() == [0] * 9                         # one stamp each
    assert c["races_vol"][_w(0)].tolist() == [9, 9, 6, 6, 4, 4, 4, 0, 4]
    # W = 1: the ms-1001 buyer joins B (now 3.1 bp) and B spans two stamps.
    assert c["races"][_w(1)].tolist() == [3, 3, 2, 2, 1, 1, 1, 0, 1]
    assert c["episodes"][_w(1)].tolist() == [4, 4, 3, 3, 2, 2, 1, 0, 2]
    assert c["lb_races"][_w(1)].tolist() == [1, 1, 1, 1, 1, 1, 1, 0, 1]
    assert c["lb_vol"][_w(1)].tolist() == [5, 5, 5, 5, 5, 5, 5, 0, 5]
    assert c["vol_all"] == 17.0 and c["covered_s"] == 4.0


def test_grid_moves_count_every_nonzero_return_by_size():
    c = hour_counts(_tape())
    grid = dict(zip((10, 100, 1000), c["grid"]))

    # 10 ms: 0.2, 3.0, 0.1 (one tick, just under 0.1 bp), 0.7, 2.0
    assert grid[10].tolist() == [5, 4, 3, 3, 2, 2, 1, 0, 2]
    # 1 s: 3.2 (A and B in one second), 0.6, 2.0
    assert grid[1000].tolist() == [3, 3, 3, 3, 2, 2, 1, 0, 2]


def test_the_any_row_reproduces_race_proxy_exactly():
    rng = np.random.default_rng(7)
    n = 4000
    buy = rng.random(n) < 0.5
    steps = rng.choice([-2, -1, 0, 0, 0, 1, 2, 40], size=n) * 0.1
    tr = {"ts_ms": np.cumsum(rng.integers(0, 4, size=n)).astype(np.int64),
          "buy": buy, "size": rng.exponential(1.0, size=n),
          "price": np.round(60000 + np.cumsum(steps), 1)}
    c = hour_counts(tr)

    for k, w in enumerate(WINDOWS_MS):
        acc = WindowAccumulator(w)
        acc.add(tr, implied_mid(tr["price"], tr["buy"]), None)
        assert c["races"][k, 0] == acc.n_races
        assert c["lb_races"][k, 0] == acc.n_lb
        assert np.isclose(c["races_vol"][k, 0], acc.vol_in)
        assert np.isclose(c["lb_vol"][k, 0], acc.vol_lb)


def test_hour_counts_and_bootstrap_are_deterministic():
    tr = _tape()
    a, b = hour_counts(tr), hour_counts(tr)
    num = np.arange(12.0).reshape(6, 2)
    den = np.arange(1.0, 7.0)

    assert all(np.array_equal(a[k], b[k]) for k in a)
    assert day_intervals(num, den, seed=3) == day_intervals(num, den, seed=3)


def test_day_intervals_bracket_the_pooled_ratio():
    num = np.array([[1.0], [2.0], [3.0], [10.0]])
    den = np.array([1.0, 1.0, 1.0, 1.0])

    est, lo, hi = day_intervals(num, den, seed=0)

    assert est == [4.0]
    assert lo[0] <= 4.0 <= hi[0] and lo[0] >= 1.0 and hi[0] <= 10.0


def test_model_snipeable_rate_is_lambda_times_laplace_tail():
    fit = {"jump_intensity_per_s": 0.2, "jump_scale_bp_laplace_mle": 0.705}

    rate = model_snipeable(fit, half_spread_bp=0.0)

    assert rate[0] == 0.2
    assert np.isclose(rate[LABELS.index("1.7")], 0.2 * np.exp(-1.7 / 0.705))
    assert np.isclose(rate[-1], rate[LABELS.index("1.7")])


def test_operating_point_moves_reduce_to_the_laplace_tail_at_10_ms():
    # One jump at most in 10 ms and no diffusion: the move rate IS lambda*exp(-h/b).
    op = {"jump_intensity_per_s": 0.2, "jump_scale_bp": 0.705, "sigma_bp_per_sqrt_s": 0.0}

    rate = operating_point_moves(op, 10, half_spread_bp=0.0, draws=200_000)
    again = operating_point_moves(op, 10, half_spread_bp=0.0, draws=200_000)

    assert np.allclose(rate, 0.2 * np.exp(-np.r_[0.0, 0.1, 0.25, 0.5, 1.0, 1.7, 2.5, 5.0, 1.7]
                                          / 0.705), rtol=0.03, atol=2e-5)  # MC error at 5 bp
    assert np.array_equal(rate, again)
