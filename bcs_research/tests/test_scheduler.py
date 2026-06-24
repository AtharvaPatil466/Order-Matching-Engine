"""Tests for the LatencyScheduler against the real bcs_engine.

Covers three behaviours:
  1. submit_at ordering — an action stamped for a future tick stays out of the
     book until that tick's submit_at is reached.
  2. fill routing — a resting maker and a crossing taker both get on_fill,
     with the correct is_buyer flag, and the trade is recorded once.
  3. snapshot shape — every snapshot has the agreed keys and count.
"""
import sys
import pathlib

_R = pathlib.Path(__file__).resolve().parents[1]
for _d in ("build", "simulation"):
    sys.path.insert(0, str(_R / _d))

import bcs_engine as be  # noqa: E402
from scheduler import Action, LatencyScheduler  # noqa: E402

SYM = 1
P = be.PRICE_PRECISION
DT = 1000


def make_harness():
    h = be.EngineHarness()
    h.add_symbol(SYM)
    h.start(disable_circuit_breaker=True)
    return h


class ConstFundamental:
    """Trivial fundamental: constant value, no market-data latency."""

    def __init__(self, value):
        self._v = value

    def step(self, t):
        return self._v

    def observe(self, t, latency_us=0):
        return self._v


class FutureBuyAgent:
    """Emits one buy Limit on tick 0, stamped to land at submit_at = 2 * DT."""

    def __init__(self, participant_id):
        self.participant_id = participant_id
        self.inventory = 0
        self.fills = []
        self._emitted = False

    def act(self, t, market, fundamental):
        if self._emitted:
            return []
        self._emitted = True
        return [Action(
            participant_id=self.participant_id,
            submit_at=2 * DT,
            kind="order",
            order_id=1,
            side=be.Side.Buy,
            price=100 * P,
            quantity=10,
            order_type=be.OrderType.Limit,
        )]

    def on_fill(self, trade, is_buyer):
        self.inventory += trade.quantity if is_buyer else -trade.quantity
        self.fills.append((trade, is_buyer))


class OneShotAgent:
    """Emits a single pre-built action on a chosen tick, then nothing."""

    def __init__(self, participant_id, fire_tick, action):
        self.participant_id = participant_id
        self.inventory = 0
        self.fills = []
        self._fire_tick = fire_tick
        self._action = action

    def act(self, t, market, fundamental):
        if t == self._fire_tick:
            return [self._action]
        return []

    def on_fill(self, trade, is_buyer):
        self.inventory += trade.quantity if is_buyer else -trade.quantity
        self.fills.append((trade, is_buyer))


def _snap_at(result, t):
    for s in result.snapshots:
        if s["t"] == t:
            return s
    raise AssertionError(f"no snapshot at t={t}")


def test_submit_at_delays_order_until_future_tick():
    # Arrange
    h = make_harness()
    sched = LatencyScheduler(h, SYM, DT)
    agent = FutureBuyAgent(participant_id=10)
    fund = ConstFundamental(100 * P)

    # Act
    result = sched.run([agent], fund, duration_us=4 * DT)

    # Assert — order not in book before its submit_at (2*DT), present after.
    assert _snap_at(result, 0)["best_bid"] == 0
    assert _snap_at(result, DT)["best_bid"] == 0
    assert _snap_at(result, 2 * DT)["best_bid"] > 0
    assert _snap_at(result, 3 * DT)["best_bid"] > 0


def test_fill_routing_to_buyer_and_seller():
    # Arrange
    h = make_harness()
    sched = LatencyScheduler(h, SYM, DT)

    maker_action = Action(
        participant_id=10, submit_at=0, kind="order", order_id=1,
        side=be.Side.Sell, price=100 * P, quantity=5,
        order_type=be.OrderType.Limit,
    )
    taker_action = Action(
        participant_id=20, submit_at=DT, kind="order", order_id=2,
        side=be.Side.Buy, price=100 * P, quantity=5,
        order_type=be.OrderType.IOC,
    )
    maker = OneShotAgent(participant_id=10, fire_tick=0, action=maker_action)
    taker = OneShotAgent(participant_id=20, fire_tick=DT, action=taker_action)
    fund = ConstFundamental(100 * P)

    # Act
    result = sched.run([maker, taker], fund, duration_us=3 * DT)

    # Assert — both sides notified with correct is_buyer.
    assert len(maker.fills) == 1
    assert maker.fills[0][1] is False          # maker is the seller
    assert len(taker.fills) == 1
    assert taker.fills[0][1] is True           # taker is the buyer
    assert maker.inventory == -5
    assert taker.inventory == 5

    assert len(result.trades) == 1
    tr = result.trades[0]
    assert tr["buyer_id"] == 20
    assert tr["seller_id"] == 10
    assert tr["quantity"] == 5
    assert tr["aggressor_buy"] is True


def test_snapshot_shape_and_count():
    # Arrange
    h = make_harness()
    sched = LatencyScheduler(h, SYM, DT)
    fund = ConstFundamental(100 * P)
    duration = 5 * DT

    # Act
    result = sched.run([], fund, duration_us=duration)

    # Assert
    expected_keys = {
        "t", "best_bid", "best_ask", "mid", "v", "tick_volume", "signed_volume",
    }
    assert len(result.snapshots) == duration // DT
    for s in result.snapshots:
        assert set(s.keys()) == expected_keys
