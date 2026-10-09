"""EventMaker — the minimal BCS liquidity provider for the event model.

Why minimal: BCS's mechanism needs exactly one thing from the maker. When
public news arrives it tries to CANCEL its now-stale quotes, and that cancel
races the snipers' IOCs to the engine. Skew, fading, multiple levels or an
optimised spread would blur what the race tests measure, so they belong to a
later lane that extends this one.

Behaviour: one bid and one ask, `half_spread` either side of the latest signal
it has observed. On every observed update it cancels the quotes it still
believes rest and posts a fresh pair, in one message burst, so the cancels
reach the engine ahead of the requote (event_scheduler.py). It quotes nothing
before its first observation: it has no value to quote around.

ponytail: a fully filled side stays empty until the next update; requote on
fill when a strategy needs continuous two-sided presence.
"""
from __future__ import annotations

import bcs_engine as be
from contracts import Cancel, Order


class EventMaker:
    def __init__(self, participant_id, half_spread, qty, md_latency_ns=0, order_latency_ns=0):
        self.participant_id = int(participant_id)
        self.md_latency_ns = int(md_latency_ns)
        self.order_latency_ns = int(order_latency_ns)
        self.half_spread = float(half_spread)
        self.qty = int(qty)
        self._oid = self.participant_id * 1_000_000_000
        self._live = {}   # order id -> quantity it believes still rests

    def _next_oid(self):
        self._oid += 1
        return self._oid

    def on_start(self, t_ns, view):
        return []

    def on_signal(self, t_ns, update, view):
        actions = [Cancel(oid) for oid in self._live]
        bid = max(1, int(round(update.value - self.half_spread)))
        ask = max(bid + 1, int(round(update.value + self.half_spread)))
        bid_oid, ask_oid = self._next_oid(), self._next_oid()
        self._live = {bid_oid: self.qty, ask_oid: self.qty}
        return actions + [
            Order(bid_oid, be.Side.Buy, bid, self.qty, be.OrderType.Limit),
            Order(ask_oid, be.Side.Sell, ask, self.qty, be.OrderType.Limit),
        ]

    def on_wakeup(self, t_ns, tag, view):
        return []

    def on_fill(self, t_ns, fill, view):
        # A fill on a quote it has already replaced is old news: ignore it.
        left = self._live.get(fill.order_id)
        if left is None:
            return []
        if left > fill.quantity:
            self._live[fill.order_id] = left - fill.quantity
        else:
            del self._live[fill.order_id]
        return []
