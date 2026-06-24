"""Unit tests for baseline_metrics.compute_metrics over a synthetic SimResult."""
import sys, pathlib
_R = pathlib.Path(__file__).resolve().parents[1]
for _d in ("build", "simulation", "metrics"):
    sys.path.insert(0, str(_R / _d))

import json

import pytest

from scheduler import SimResult
from baseline_metrics import compute_metrics, PP


class StubAgent:
    """Minimal agent: fixed pnl regardless of mark, fixed inventory."""

    def __init__(self, participant_id, inventory, pnl_value):
        self.participant_id = participant_id
        self.inventory = inventory
        self._pnl_value = pnl_value

    def pnl(self, mark_price):
        return self._pnl_value


def _snapshot(t, mid, signed_volume):
    return {
        "t": t,
        "best_bid": mid - 50,
        "best_ask": mid + 50,
        "mid": mid,
        "v": mid,
        "tick_volume": abs(signed_volume),
        "signed_volume": signed_volume,
    }


MM_ID = 1
NT_IDS = [2, 3]

# mids step +10, -10, +20 → returns +,-,+ → two reversals; spread fixed at 100 ticks.
MIDS = [1_000_000, 1_000_010, 1_000_000, 1_000_020]
SIGNED = [0, 10, -10, 20]
DT_US = 1000
DURATION_US = 4000  # 4 snapshots → duration_seconds == 0.004


def _make_result():
    snaps = [
        _snapshot(i * DT_US, mid, sv)
        for i, (mid, sv) in enumerate(zip(MIDS, SIGNED))
    ]
    trades = [
        {"t": 1000, "price": 1_000_000, "quantity": 10, "buyer_id": 2,
         "seller_id": 1, "aggressor_buy": True},
        {"t": 3000, "price": 1_000_010, "quantity": 20, "buyer_id": 1,
         "seller_id": 3, "aggressor_buy": False},
    ]
    agents = [
        StubAgent(MM_ID, inventory=5, pnl_value=5.0),
        StubAgent(2, inventory=-3, pnl_value=-2.0),
        StubAgent(3, inventory=-2, pnl_value=-3.0),
    ]
    return SimResult(snaps, trades, agents, DURATION_US, DT_US)


def test_time_weighted_avg_spread():
    m = compute_metrics(_make_result(), MM_ID, NT_IDS)
    assert m["time_weighted_avg_spread_ticks"] == 100
    assert m["time_weighted_avg_spread_dollars"] == pytest.approx(0.01)


def test_price_reversals():
    m = compute_metrics(_make_result(), MM_ID, NT_IDS)
    assert m["price_reversals"] == 2


def _make_kyle_result():
    # Lagged construction: forward 1-tick mid change equals the signed flow at
    # the START tick, so the k=1 regression slope is exactly 1.
    # sv=[10,-10,20,0]; mids cumulate sv: each mid[i+1]-mid[i] == sv[i].
    mids = [1_000_000, 1_000_010, 1_000_000, 1_000_020]
    signed = [10, -10, 20, 0]
    snaps = [_snapshot(i * DT_US, mid, sv)
             for i, (mid, sv) in enumerate(zip(mids, signed))]
    agents = [StubAgent(MM_ID, 0, 0.0)]
    return SimResult(snaps, [], agents, DURATION_US, DT_US)


def test_kyle_lambda_lagged_perfectly_linear():
    m = compute_metrics(_make_kyle_result(), MM_ID, NT_IDS, kyle_horizon_ticks=1)
    assert m["kyle_lambda"] == pytest.approx(1.0)
    assert m["kyle_lambda_r2"] == pytest.approx(1.0)
    assert m["kyle_lambda_horizon_ticks"] == 1


def test_pnl_aggregation():
    m = compute_metrics(_make_result(), MM_ID, NT_IDS)
    assert m["mm_pnl_dollars"] == 5.0
    assert m["noise_trader_pnl_dollars"] == -5.0
    assert m["mm_pnl_per_sec"] == pytest.approx(5.0 / m["duration_seconds"])
    assert m["mm_inventory_final"] == 5


def test_endogenous_crashes_and_json_serializable():
    m = compute_metrics(_make_result(), MM_ID, NT_IDS)
    assert m["endogenous_crashes"] == 0
    # Must not raise — every value is a plain JSON number.
    json.dumps(m)


def test_all_one_sided_book_yields_zero_spreads():
    snaps = [
        {"t": i * DT_US, "best_bid": 0, "best_ask": 0, "mid": 0,
         "v": 1_000_000, "tick_volume": 0, "signed_volume": 0}
        for i in range(4)
    ]
    result = SimResult(snaps, [], [StubAgent(MM_ID, 0, 0.0)], DURATION_US, DT_US)
    m = compute_metrics(result, MM_ID, NT_IDS)
    assert m["time_weighted_avg_spread_ticks"] == 0.0
    assert m["time_weighted_avg_spread_rel"] == 0.0
    assert m["volume_weighted_avg_spread_rel"] == 0.0
    assert m["price_reversals"] == 0
    assert m["kyle_lambda"] == 0.0
