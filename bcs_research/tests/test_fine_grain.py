"""fine_grain: implied mid, grid sampling and what a coarse tick collapses."""
import pathlib
import sys

import numpy as np

_R = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(_R / "calibration"))

from fine_grain import (IntervalAccumulator, implied_mid, moved,  # noqa: E402
                        on_grid, same_price_gaps, tick_losses)


def test_implied_mid_removes_bid_ask_bounce():
    # A buy at the ask 100.1 and a sell at the bid 100.0 share one mid.
    mid = implied_mid(np.array([100.1, 100.0]), np.array([True, False]))
    assert np.allclose(mid, [100.05, 100.05])
    assert not moved(mid).any()


def test_on_grid_samples_previous_tick():
    ts = np.array([0, 3, 25])
    vals = np.array([1.0, 2.0, 3.0])
    # grid 0, 10, 20 -> last value at or before each point
    assert on_grid(ts, vals, 10).tolist() == [1.0, 2.0, 2.0]


def test_tick_losses_counts_trades_sharing_a_bucket_with_a_move():
    # Arrange: bucket 0 holds a move and a follow-on trade; bucket 1 one quiet trade.
    ts = np.array([0, 10, 20, 150])
    size = np.array([1.0, 1.0, 2.0, 4.0])
    mid = np.array([100.0, 100.0, 100.3, 100.3])   # move at the third record

    loss = tick_losses(ts, size, mid, 100)

    assert loss["n_move"] == 3 and loss["vol_move"] == 4.0
    assert loss["n_multi"] == 3
    assert np.isclose(loss["path"], 0.3) and np.isclose(loss["net"], 0.3)


def test_tick_losses_path_lost_inside_a_bucket():
    # Up two ticks and back inside one 100 ms bucket: the grid sees no move.
    ts = np.array([0, 5, 9, 200])
    mid = np.array([100.0, 100.2, 100.0, 100.0])
    loss = tick_losses(ts, np.ones(4), mid, 100)
    assert np.isclose(loss["path"], 0.4)
    assert np.isclose(loss["net"], 0.0)


def test_accumulator_flags_clustering_against_poisson():
    # Arrange: 1000 records, all crammed into 10 of 1000 buckets.
    ts = np.repeat(np.arange(0, 10_000, 1000), 100)
    ts = np.r_[ts, 9_999]
    mid = np.full(ts.size, 100.0)
    acc = IntervalAccumulator(10)

    acc.add(ts, np.ones(ts.size), mid)
    s = acc.summary()

    assert s["records_per_bucket_fano"] > 50          # Poisson would be 1
    assert s["p_empty"] > s["p_empty_if_poisson"]
    assert s["sigma_bp_per_sqrt_s"] == 0.0


def test_same_price_gaps_only_pairs_same_side_same_price():
    tr = {"ts_ms": np.array([0, 50, 160, 170]),
          "price": np.array([1.0, 1.0, 1.0, 1.1]),
          "buy": np.array([True, False, False, False])}
    assert same_price_gaps(tr).tolist() == [110]
