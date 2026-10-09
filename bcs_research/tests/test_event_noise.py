"""Event agents driven directly through their callbacks against a fake book.

The event scheduler is being built in parallel, so these tests play its part:
they call on_start / on_signal / on_wakeup and follow the returned Wakeups.
Statistical tolerances are >= 4 standard errors of the estimator.
"""
from __future__ import annotations

import math
import sys
from pathlib import Path

import numpy as np
import pytest

_ROOT = Path(__file__).resolve().parents[1]
for _d in ("build", "agents", "simulation"):
    sys.path.insert(0, str(_ROOT / _d))

import bcs_engine as be  # noqa: E402
from contracts import Order, SignalUpdate, Wakeup  # noqa: E402
from event_noise import ReservationNoiseTrader, SlowInformedInvestor  # noqa: E402

V0 = 1_000_000
NS_PER_S = 1_000_000_000


class FakeBook:
    def __init__(self, bid, ask):
        self.bid, self.ask = bid, ask

    def best_bid(self):
        return self.bid

    def best_ask(self):
        return self.ask


def _orders(actions):
    return [a for a in actions if isinstance(a, Order)]


def _run_arrivals(agent, book, n):
    """Play the scheduler for n arrivals; return the orders and arrival times."""
    (wake,) = agent.on_start(0, book)
    orders, times = [], []
    for _ in range(n):
        times.append(wake.at_ns)
        actions = agent.on_wakeup(wake.at_ns, wake.tag, book)
        (wake,) = [a for a in actions if isinstance(a, Wakeup)]
        orders += _orders(actions)
    return orders, np.array(times)


# --- ReservationNoiseTrader ---------------------------------------------------

def _noise(eps_scale, **kw):
    return ReservationNoiseTrader(100, arrival_rate_per_s=10.0, eps_scale=eps_scale,
                                  v0=V0, qty=5, **kw)


def test_arrivals_are_poisson_at_the_configured_rate():
    # Arrange: mean gap 0.1 s; SE of the mean over 20,000 gaps is 0.7%, tolerance 3%.
    agent = _noise(eps_scale=10.0, seed=1)

    # Act
    _, times = _run_arrivals(agent, FakeBook(V0 - 50, V0 + 50), 20_000)
    gaps = np.diff(times) / NS_PER_S

    # Assert: exponential gaps have mean = sd
    assert abs(gaps.mean() - 0.1) / 0.1 < 0.03
    assert abs(gaps.std() / gaps.mean() - 1.0) < 0.03
    assert agent.arrivals == 20_000


def test_arrival_valuing_above_the_ask_buys_ioc_at_the_ask():
    # Arrange: no private shock, signal moved above the ask
    agent = _noise(eps_scale=0.0)
    book = FakeBook(V0 - 50, V0 + 50)
    agent.on_signal(0, SignalUpdate(0, V0 + 80.0), book)

    # Act
    (order,) = _orders(agent.on_wakeup(1, "arrive", book))

    # Assert
    assert (order.side, order.price, order.quantity) == (be.Side.Buy, V0 + 50, 5)
    assert order.order_type == be.OrderType.IOC


def test_arrival_valuing_below_the_bid_sells_and_inside_the_spread_leaves():
    # Arrange
    agent = _noise(eps_scale=0.0)
    book = FakeBook(V0 - 50, V0 + 50)

    # Act
    agent.on_signal(0, SignalUpdate(0, V0 - 60.0), book)
    sells = _orders(agent.on_wakeup(1, "arrive", book))
    agent.on_signal(2, SignalUpdate(2, V0 + 10.0), book)
    stays = agent.on_wakeup(3, "arrive", book)

    # Assert
    assert [(o.side, o.price) for o in sells] == [(be.Side.Sell, V0 - 50)]
    assert _orders(stays) == [] and len(stays) == 1          # only the next arrival
    assert (agent.arrivals, agent.crossings) == (2, 1)


@pytest.mark.parametrize("half_spread", [10, 40, 100])
def test_participation_falls_with_the_spread_as_the_closed_form_says(half_spread):
    # Arrange: signal at the mid, eps ~ N(0, 40^2) -> P(cross) = 2 Phi(-h / 40).
    # SE at n=20,000 is <= 0.0035; tolerance 0.015 (> 4 SE).
    agent = _noise(eps_scale=40.0, seed=2)
    book = FakeBook(V0 - half_spread, V0 + half_spread)
    expected = math.erfc(half_spread / 40.0 / math.sqrt(2))

    # Act
    orders, _ = _run_arrivals(agent, book, 20_000)

    # Assert
    assert abs(len(orders) / 20_000 - expected) < 0.015


def test_infinite_shock_scale_nests_the_fixed_crossing_probability_trader():
    # Arrange: a spread so wide no finite-shock arrival would cross
    agent = _noise(eps_scale=math.inf, seed=3)
    book = FakeBook(V0 - 10**5, V0 + 10**5)

    # Act
    orders, _ = _run_arrivals(agent, book, 20_000)
    buy_share = sum(o.side == be.Side.Buy for o in orders) / len(orders)

    # Assert: every arrival crosses, on a fair coin (SE 0.0035, tolerance 0.015)
    assert len(orders) == 20_000
    assert abs(buy_share - 0.5) < 0.015


def test_sizes_are_lognormal_around_the_median_qty():
    # Arrange: sigma_ln = 1 -> median of sizes = qty; SE of the sample median
    # in log space is ~1.25/sqrt(n) = 0.9%, tolerance 5%.
    agent = ReservationNoiseTrader(100, 10.0, math.inf, V0, qty=100, size_sigma_ln=1.0, seed=4)

    # Act
    orders, _ = _run_arrivals(agent, FakeBook(V0 - 50, V0 + 50), 20_000)
    sizes = np.array([o.quantity for o in orders])

    # Assert
    assert abs(np.median(sizes) / 100 - 1.0) < 0.05
    assert sizes.min() >= 1 and sizes.max() > 1000           # heavy right tail


def test_one_sided_book_only_allows_the_available_side():
    # Arrange: no asks resting
    agent = _noise(eps_scale=math.inf, seed=5)

    # Act
    orders, _ = _run_arrivals(agent, FakeBook(V0 - 50, 0), 200)

    # Assert
    assert orders and all(o.side == be.Side.Sell for o in orders)


def test_same_seed_gives_the_same_order_flow():
    # Arrange
    book = FakeBook(V0 - 30, V0 + 30)

    # Act
    a, ta = _run_arrivals(_noise(eps_scale=40.0, seed=6), book, 500)
    b, tb = _run_arrivals(_noise(eps_scale=40.0, seed=6), book, 500)

    # Assert
    assert a == b and (ta == tb).all()


# --- SlowInformedInvestor -----------------------------------------------------

class FakePath:
    def __init__(self, value):
        self.value, self.asked = value, []

    def value_at(self, t_ns):
        self.asked.append(t_ns)
        return self.value


def _investor(path, **kw):
    cfg = dict(info_lag_ns=5 * NS_PER_S, decision_interval_ns=60 * NS_PER_S,
               parent_qty=10, n_children=4, child_interval_ns=NS_PER_S)
    return SlowInformedInvestor(200, path, **{**cfg, **kw})


def _work_parent(agent, book, t0):
    """Run one decision and every child it schedules; return orders and wakeups."""
    actions = agent.on_wakeup(t0, "decide", book)
    orders, wakes = _orders(actions), [a for a in actions if isinstance(a, Wakeup)]
    while wakes[-1].tag == "child":
        actions = agent.on_wakeup(wakes[-1].at_ns, "child", book)
        orders += _orders(actions)
        wakes += [a for a in actions if isinstance(a, Wakeup)]
    return orders, wakes


def test_value_above_mid_is_worked_as_a_twap_of_buys_at_the_ask():
    # Arrange
    path, book = FakePath(V0 + 500.0), FakeBook(V0 - 50, V0 + 50)
    agent = _investor(path)
    t0 = 100 * NS_PER_S

    # Act
    orders, wakes = _work_parent(agent, book, t0)

    # Assert: 4 children summing to the parent, 1 s apart, then the next decision
    assert [o.quantity for o in orders] == [3, 3, 2, 2]
    assert all((o.side, o.price, o.order_type) == (be.Side.Buy, V0 + 50, be.OrderType.IOC)
               for o in orders)
    assert [w.at_ns for w in wakes] == [t0 + k * NS_PER_S for k in (1, 2, 3)] + \
        [t0 + 3 * NS_PER_S + 60 * NS_PER_S]
    assert wakes[-1].tag == "decide"


def test_value_below_mid_sells_at_the_bid_and_sees_v_with_the_info_lag():
    # Arrange
    path, book = FakePath(V0 - 500.0), FakeBook(V0 - 50, V0 + 50)
    agent = _investor(path)

    # Act
    orders, _ = _work_parent(agent, book, 100 * NS_PER_S)

    # Assert
    assert {(o.side, o.price) for o in orders} == {(be.Side.Sell, V0 - 50)}
    assert path.asked == [95 * NS_PER_S]


def test_edge_within_threshold_or_one_sided_book_waits_for_the_next_decision():
    # Arrange
    agent = _investor(FakePath(V0 + 20.0), min_edge=30.0)

    # Act
    small = agent.on_wakeup(0, "decide", FakeBook(V0 - 50, V0 + 50))
    one_sided = agent.on_wakeup(0, "decide", FakeBook(0, V0 + 50))

    # Assert
    assert small == [Wakeup(60 * NS_PER_S, "decide")]
    assert one_sided == [Wakeup(60 * NS_PER_S, "decide")]


def test_investor_starts_with_a_decision_and_rejects_bad_parents():
    # Arrange / Act
    agent = _investor(FakePath(V0))

    # Assert
    assert agent.on_start(7, FakeBook(1, 2)) == [Wakeup(7 + 60 * NS_PER_S, "decide")]
    with pytest.raises(ValueError):
        _investor(FakePath(V0), parent_qty=3, n_children=4)
