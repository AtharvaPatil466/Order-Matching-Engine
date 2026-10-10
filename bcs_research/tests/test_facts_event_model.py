"""The event-model stylized-facts run: the adapter, the verdict rule, determinism.

The adapter is what makes the event model comparable to the tape at all, so
it is pinned on a hand-built result whose correct arrays can be written down:
ms flooring, aggTrade merging by (price, side, 100 ms from the first fill),
and one-sided snapshots dropped.
"""
import math
import sys
from pathlib import Path

_R = Path(__file__).resolve().parents[1]
if str(_R / "experiments") not in sys.path:
    sys.path.insert(0, str(_R / "experiments"))

import numpy as np  # noqa: E402
import pytest  # noqa: E402

import facts_event_model as fem  # noqa: E402
from contracts import TradeRecord  # noqa: E402
from event_scheduler import BookSnapshot, EventSimResult  # noqa: E402

MS = 1_000_000


def _tiny_result():
    trades = [
        TradeRecord(1_000 * MS + 400_000, 100, 2, 9, 1, True),   # opens buy@100, t=1000 ms
        TradeRecord(1_050 * MS, 100, 3, 9, 1, True),             # +50 ms, same key: merges
        TradeRecord(1_060 * MS, 99, 1, 1, 9, False),             # sell@99: own record
        TradeRecord(1_100 * MS + 900_000, 100, 4, 9, 1, True),   # +100 ms from first: merges
        TradeRecord(1_101 * MS, 100, 5, 9, 1, True),             # +101 ms: new record
        TradeRecord(1_101 * MS, 101, 6, 9, 1, True),             # other price: own record
    ]
    snaps = [BookSnapshot(0, 0, 0), BookSnapshot(100 * MS, 99, 100),
             BookSnapshot(200 * MS, 99, 0), BookSnapshot(300 * MS, 98, 101)]
    return EventSimResult(trades, snaps, (), 400 * MS, None, [], 0)


def test_adapter_merges_fills_into_aggtrade_records_on_ms_stamps():
    a = fem.sim_arrays(_tiny_result())
    np.testing.assert_array_equal(a["t"], [1.000, 1.060, 1.101, 1.101])
    np.testing.assert_array_equal(a["px"], [100, 99, 100, 101])
    np.testing.assert_array_equal(a["qty"], [9, 1, 5, 6])
    np.testing.assert_array_equal(a["buy"], [True, False, True, True])


def test_adapter_keeps_raw_fills_when_asked():
    a = fem.sim_arrays(_tiny_result(), aggregate=False)
    np.testing.assert_array_equal(a["t"], [1.000, 1.050, 1.060, 1.100, 1.101, 1.101])
    np.testing.assert_array_equal(a["qty"], [2, 3, 1, 4, 5, 6])


def test_adapter_keeps_only_two_sided_quotes_in_seconds():
    a = fem.sim_arrays(_tiny_result())
    np.testing.assert_array_equal(a["qt"], [0.1, 0.3])
    np.testing.assert_array_equal(a["bid"], [99, 98])
    np.testing.assert_array_equal(a["ask"], [100, 101])


def test_verdict_rule():
    assert fem.verdict(1.0, 5.0, 1.9, 0.0) == "matches"
    assert fem.verdict(1.0, 5.0, -1.0, 0.0) == "closer than tick model"
    assert fem.verdict(1.0, 1.5, 3.0, 0.0) == "misses"
    assert fem.verdict(0.0, 0.5, 0.03, 0.05) == "matches"          # inside the floor
    assert fem.verdict(float("nan"), 1.0, 1.0, 0.0) == "n/a"


def test_config_is_the_documented_calibration():
    c = fem.CFG
    assert c["path"]["v0"] * c["tick_bp"] * 1e-4 == pytest.approx(1.0)   # 1 unit = 1 tick
    spread_ticks = 2 * c["makers"]["half_spread_bp"] / c["tick_bp"]
    assert spread_ticks == pytest.approx(1.08, abs=0.01)                  # BCS ~ one tick
    assert c["noise"]["trade_rate_per_s"] == pytest.approx(16.28, abs=0.01)
    assert c["snipers"]["md_latency_ns"] + c["snipers"]["order_latency_ns"] \
        < c["makers"]["md_latency_ns"] + c["makers"]["order_latency_ns"]


def test_a_short_run_is_deterministic_per_seed_and_trades_at_the_tape_rate():
    a, b, c = (fem.run(fem.CFG, s, horizon_s=20) for s in (3, 3, 4))
    assert a.trades == b.trades and a.snapshots == b.snapshots
    assert a.trades != c.trades
    rate = len(a.trades) / 20
    assert 10 < rate < 25 and not math.isnan(rate)
