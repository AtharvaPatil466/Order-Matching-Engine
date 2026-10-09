"""race_proxy: triggers, same-side clusters after a move, depth alignment."""
import pathlib
import sys

import numpy as np

_R = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(_R / "calibration"))

from fine_grain import implied_mid  # noqa: E402
from race_proxy import (WindowAccumulator, cluster_end,  # noqa: E402
                        book_at, merge_clusters, triggers)


def test_triggers_separate_own_direction_moves_from_bounce():
    # sell at bid 100.0, buy at ask 100.1 (bounce, same mid), buy lifts to 100.3,
    # buy at 100.2 (quotes fell: mid moved against the buyer -> neither)
    price = np.array([100.0, 100.1, 100.3, 100.2])
    buy = np.array([False, True, True, True])

    move, placebo = triggers(implied_mid(price, buy), buy)

    assert move.tolist() == [False, False, True, False]
    assert placebo.tolist() == [False, True, False, False]


def test_window_zero_is_the_same_millisecond():
    ts = np.array([0, 0, 5, 200])
    trig = np.array([True, False, False, False])

    same_ms = merge_clusters(trig, cluster_end(ts, 0))
    ten_ms = merge_clusters(trig, cluster_end(ts, 10))

    assert same_ms.tolist() == [1, 1, 0, 0]
    assert ten_ms.tolist() == [1, 1, 1, 0]


def test_overlapping_clusters_merge_but_touching_ones_do_not():
    ts = np.array([0, 3, 6, 9])
    overlap = merge_clusters(np.array([True, False, True, False]), cluster_end(ts, 6))
    ts2 = np.array([0, 0, 1, 1])
    touch = merge_clusters(np.array([True, False, True, False]), cluster_end(ts2, 0))
    assert overlap.tolist() == [1, 1, 1, 1]
    assert touch.tolist() == [1, 1, 2, 2]


def test_book_at_reads_top_and_first_price_from_the_last_fresh_snapshot():
    # Ladder at t=0: ask 100.1 x5, 100.2 x7. At t=100: ask 100.2 x3, 100.3 x4.
    book = {"ts_ms": np.array([0, 100]),
            "ask_px": np.array([[100.1, 100.2], [100.2, 100.3]]),
            "ask_sz": np.array([[5.0, 7.0], [3.0, 4.0]])}
    t = np.array([100, 150, 150, 500])
    first = np.array([100.2, 100.3, 100.5, 100.2])

    bk = book_at(book, t, first, True)

    assert bk["top"][0] == 5.0 and bk["at_first_px"][0] == 7.0   # strictly earlier (t=0)
    assert bk["level"][0] == 1 and bk["age_ms"][0] == 100
    assert bk["top"][1] == 3.0 and bk["at_first_px"][1] == 4.0
    assert bk["top"][2] == 3.0 and np.isnan(bk["at_first_px"][2])  # price off the ladder
    assert np.isnan(bk["top"][3]) and bk["level"][3] == -1          # 400 ms stale


def test_accumulator_measures_one_sweep_against_depth():
    # Arrange: a quiet sell, then a buy sweep of three levels in one ms
    # (the level-2 fill is the visible move), then a lone buy 500 ms later.
    tr = {"ts_ms": np.array([0, 50, 50, 50, 550]),
          "price": np.array([100.0, 100.1, 100.2, 100.3, 100.3]),
          "size": np.array([1.0, 1.0, 2.0, 3.0, 4.0]),
          "buy": np.array([False, True, True, True, True])}
    book = {"ts_ms": np.array([10]), "ask_px": np.array([[100.1, 100.2]]),
            "ask_sz": np.array([[1.0, 2.0]])}
    acc = WindowAccumulator(0)

    acc.add(tr, implied_mid(tr["price"], tr["buy"]), book)
    s = acc.summary(covered_s=1.0)

    assert s["n_races"] == 1
    assert s["race_qty_btc"]["p50"] == 6.0          # the whole ms: level-1 fill included
    assert s["race_qty_over_top_depth"]["p50"] == 6.0      # 6 BTC against 1 BTC at 100.1
    assert s["share_starting_at_snapshot_top"] == 1.0
    assert s["price_levels_per_race"]["p50"] == 3
    assert np.isclose(s["share_of_volume_in_races"], 6 / 11)
    assert s["lower_bound_races_per_s"] == 0.0               # one stamp: could be one taker


def test_lower_bound_counts_a_race_spanning_two_stamps():
    # The trigger at 50 ms and a second buyer at 51 ms cannot be one order.
    tr = {"ts_ms": np.array([0, 50, 51]),
          "price": np.array([100.0, 100.2, 100.3]),
          "size": np.array([1.0, 1.0, 1.0]),
          "buy": np.array([False, True, True])}
    acc = WindowAccumulator(1)

    acc.add(tr, implied_mid(tr["price"], tr["buy"]), None)
    s = acc.summary(covered_s=1.0)

    assert s["n_races"] == 1 and s["lower_bound_races_per_s"] == 1.0
    assert np.isclose(s["lower_bound_share_of_volume"], 2 / 3)
    assert s["race_qty_over_top_depth"] == {"n": 0}            # no book, no ratio
