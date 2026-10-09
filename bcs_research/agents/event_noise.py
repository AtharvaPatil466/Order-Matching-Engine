"""Uninformed and slow-informed flow for the event model.

The tick model's NoiseTrader crosses with a fixed per-tick probability, and
paper §4.9 bolted a spread elasticity onto that probability. Real uninformed
traders differ in what they think the asset is worth, so participation
should FALL OUT of the spread rather than be dialled in.
ReservationNoiseTrader does that: each arrival values the asset at the last
public signal plus a private shock, and trades only if the quote is on the
right side of that value. A wider spread turns more arrivals away, with no
elasticity knob. An infinite shock scale nests the old fixed-probability
trader: every arrival then crosses, on a fair coin.

SlowInformedInvestor is the participant the tick model lacked: a
long-horizon investor who learns the true V late and works a parent order as
a TWAP, adding directional flow that is neither noise nor a snipe.

Both implement EventAgent (simulation/contracts.py) and assume nothing about
the scheduler beyond that contract.
"""
from __future__ import annotations

import math

import numpy as np

import bcs_engine as be
from contracts import Order, Wakeup

NS_PER_S = 1_000_000_000


def _lognormal_qty(rng: np.random.Generator, median: int, sigma_ln: float | None) -> int:
    """NoiseTrader's size draw. sigma_ln comes from calibration/fit.py
    size_model() (2.80 for BTCUSDT); None keeps a constant size."""
    if sigma_ln is None:
        return median
    return max(1, round(median * math.exp(sigma_ln * rng.standard_normal())))


class _Ids:
    """Order ids in the participant's own block, as NoiseTrader numbers them."""

    def __init__(self, participant_id: int):
        self._next = participant_id * 1_000_000_000

    def __call__(self) -> int:
        self._next += 1
        return self._next


def _ioc_at_touch(oid: int, is_buy: bool, bid: int, ask: int, qty: int) -> Order | None:
    price = ask if is_buy else bid
    if price <= 0:
        return None
    return Order(oid, be.Side.Buy if is_buy else be.Side.Sell, price, qty, be.OrderType.IOC)


class ReservationNoiseTrader:
    """A Poisson population of uninformed traders with private reservation values.

    Arrivals come at `arrival_rate_per_s` (self-scheduled Wakeups). Arrival i
    values the asset at s + eps_i, s the last signal it observed (v0 before
    any) and eps_i ~ N(0, eps_scale^2) in fixed-point price units. It buys
    at the ask if that value >= ask, sells at the bid if <= bid, and
    otherwise leaves. With the signal at the mid and half-spread h, the
    crossing probability per arrival is 2 * Phi(-h / eps_scale).

    `arrivals` and `crossings` count participation, which the trade list
    cannot show because a turned-away arrival leaves no trace.
    """

    def __init__(self, participant_id: int, arrival_rate_per_s: float, eps_scale: float,
                 v0: float, qty: int = 1, size_sigma_ln: float | None = None,
                 md_latency_ns: int = 0, order_latency_ns: int = 0, seed: int = 0):
        if arrival_rate_per_s <= 0 or eps_scale < 0:
            raise ValueError("arrival_rate_per_s must be > 0 and eps_scale >= 0")
        self.participant_id = int(participant_id)
        self.md_latency_ns = int(md_latency_ns)
        self.order_latency_ns = int(order_latency_ns)
        self.arrival_rate_per_s = float(arrival_rate_per_s)
        self.eps_scale = float(eps_scale)
        self.qty = int(qty)
        self.size_sigma_ln = size_sigma_ln
        self._signal = float(v0)
        self._rng = np.random.default_rng(seed)
        self._oid = _Ids(self.participant_id)
        self.arrivals = 0
        self.crossings = 0

    def _next_arrival(self, t_ns: int) -> Wakeup:
        gap = self._rng.exponential(NS_PER_S / self.arrival_rate_per_s)
        return Wakeup(t_ns + max(1, round(gap)), "arrive")

    def on_start(self, t_ns, view):
        return [self._next_arrival(t_ns)]

    def on_signal(self, t_ns, update, view):
        self._signal = update.value
        return []

    def on_fill(self, t_ns, fill, view):
        return []

    def on_wakeup(self, t_ns, tag, view):
        nxt = self._next_arrival(t_ns)
        self.arrivals += 1
        # An infinite scale gives +-inf: always crosses, side on the sign.
        value = self._signal + self.eps_scale * self._rng.standard_normal()
        bid, ask = view.best_bid(), view.best_ask()
        if ask > 0 and value >= ask:
            is_buy = True
        elif bid > 0 and value <= bid:
            is_buy = False
        else:
            return [nxt]
        self.crossings += 1
        qty = _lognormal_qty(self._rng, self.qty, self.size_sigma_ln)
        return [nxt, _ioc_at_touch(self._oid(), is_buy, bid, ask, qty)]


class SlowInformedInvestor:
    """Learns V late and works it as a TWAP of IOC child orders.

    Every `decision_interval_ns` it forecasts V as path.value_at(t - info_lag_ns)
    plus N(0, forecast_noise^2). If that differs from the mid by more than
    `min_edge` it opens a parent order of `parent_qty` on that side and sends
    `n_children` near-equal IOC children at the touch, `child_interval_ns`
    apart, then waits for the next decision.

    It reads the path's true V directly. contracts.py reserves value_at for
    accounting; this investor is the one deliberate exception, because
    knowing V late (including any component the public signal misses, as in
    CorrelatedSignalPath) is what makes it informed rather than noise.

    ponytail: children are IOC at the touch and any unfilled remainder is
    dropped, with no catch-up and no limit price. Add a participation cap
    or resting children if its impact on the book matters.
    """

    def __init__(self, participant_id: int, path, info_lag_ns: int,
                 decision_interval_ns: int, parent_qty: int, n_children: int,
                 child_interval_ns: int, forecast_noise: float = 0.0,
                 min_edge: float = 0.0, md_latency_ns: int = 0,
                 order_latency_ns: int = 0, seed: int = 0):
        if n_children < 1 or parent_qty < n_children:
            raise ValueError("need 1 <= n_children <= parent_qty")
        if decision_interval_ns <= 0 or child_interval_ns <= 0:
            raise ValueError("decision and child intervals must be positive")
        self.participant_id = int(participant_id)
        self.md_latency_ns = int(md_latency_ns)
        self.order_latency_ns = int(order_latency_ns)
        self.path = path
        self.info_lag_ns = int(info_lag_ns)
        self.decision_interval_ns = int(decision_interval_ns)
        self.parent_qty = int(parent_qty)
        self.n_children = int(n_children)
        self.child_interval_ns = int(child_interval_ns)
        self.forecast_noise = float(forecast_noise)
        self.min_edge = float(min_edge)
        self._rng = np.random.default_rng(seed)
        self._oid = _Ids(self.participant_id)
        self._is_buy = True
        self._sent = 0                  # children sent of the current parent

    def on_start(self, t_ns, view):
        return [Wakeup(t_ns + self.decision_interval_ns, "decide")]

    def on_signal(self, t_ns, update, view):
        return []

    def on_fill(self, t_ns, fill, view):
        return []

    def on_wakeup(self, t_ns, tag, view):
        if tag == "child":
            return self._child(t_ns, view)
        bid, ask = view.best_bid(), view.best_ask()
        idle = [Wakeup(t_ns + self.decision_interval_ns, "decide")]
        if bid <= 0 or ask <= 0:
            return idle
        forecast = (self.path.value_at(t_ns - self.info_lag_ns)
                    + self.forecast_noise * self._rng.standard_normal())
        edge = forecast - (bid + ask) / 2.0
        if abs(edge) <= self.min_edge:
            return idle
        self._is_buy = edge > 0
        self._sent = 0
        return self._child(t_ns, view)

    def _child(self, t_ns, view):
        base, extra = divmod(self.parent_qty, self.n_children)
        qty = base + (1 if self._sent < extra else 0)
        self._sent += 1
        if self._sent < self.n_children:
            actions = [Wakeup(t_ns + self.child_interval_ns, "child")]
        else:
            actions = [Wakeup(t_ns + self.decision_interval_ns, "decide")]
        order = _ioc_at_touch(self._oid(), self._is_buy, view.best_bid(), view.best_ask(), qty)
        return actions if order is None else actions + [order]
