"""Unit tests for the redesigned Exp 5: windows, labelling, shocks, quote events."""
import sys
import pathlib

_R = pathlib.Path(__file__).resolve().parents[1]
for _d in ("build", "agents", "simulation", "metrics", "experiments", "hawkes",
           "analysis"):
    sys.path.insert(0, str(_R / _d))

from exp5_redesigned import (ShockedFundamental, label_windows,
                             quote_event_times_us)


def test_label_windows_drops_containing_and_splits_pre_normal():
    # Window span is (end-1000, end]; lead is 500.
    windows = [(1000.0, 0.1), (2000.0, 0.2), (3000.0, 0.3), (4000.0, 0.4)]
    marks = [2400.0]  # inside (2000,3000] span -> window@3000 dropped
    pre, normal, dropped = label_windows(windows, marks, window_us=1000, lead_us=500)
    assert dropped == 1
    assert pre == [0.2]            # window@2000: mark at 2400 within lead 500
    assert normal == [0.1, 0.4]    # far windows stay background


def test_label_windows_no_marks_all_normal():
    windows = [(1000.0, 0.1), (2000.0, 0.2)]
    pre, normal, dropped = label_windows(windows, [], 1000, 500)
    assert (pre, dropped) == ([], 0)
    assert normal == [0.1, 0.2]


def test_shocked_fundamental_jumps_at_scheduled_times_and_alternates():
    f = ShockedFundamental(v0=1000.0, sigma=0.0, dt_us=1000, seed=1,
                           shock_times_us=[3000, 6000], shock_size=100.0)
    vals = [f.step(t) for t in range(1000, 9001, 1000)]
    # sigma=0 -> pure jumps: +100 at t=3000, -100 at t=6000 (alternating sign).
    assert vals[0] == 1000.0 and vals[1] == 1000.0
    assert vals[2] == 1100.0                     # jump applied at 3000
    assert vals[5] == 1000.0                     # second jump reverses
    # observe() sees the post-jump value at and after the jump tick.
    assert f.observe(3000) == 1100.0
    assert f.observe(2999) == 1000.0


def test_quote_event_times_counts_per_side_changes():
    snaps = [
        {"t": 0, "best_bid": 99, "best_ask": 101},
        {"t": 1000, "best_bid": 99, "best_ask": 101},   # no change -> 0 events
        {"t": 2000, "best_bid": 98, "best_ask": 101},   # bid moved -> 1
        {"t": 3000, "best_bid": 97, "best_ask": 102},   # both moved -> 2 (tied)
        {"t": 4000, "best_bid": 97, "best_ask": 0},     # ask vanished -> 1
    ]
    times = quote_event_times_us(snaps, dt_us=1000)
    assert len(times) == 4
    assert sorted(times) == list(times)                 # strictly ordered
    assert len(set(times)) == len(times)                # de-tied
