"""EventSniper — the BCS stale-quote sniper for the event model.

It does nothing but react to news: on observing a SignalUpdate it compares the
new value with the CURRENT book (contracts.py: book data has no latency) and,
if a resting quote is stale by more than `threshold` — its cost — sends one
IOC at that stale price:
  - value - best_ask > threshold  -> buy IOC at best_ask
  - best_bid - value > threshold  -> sell IOC at best_bid
Whether the IOC fills is decided by the race at the engine: if the maker's
cancel arrives first the IOC finds nothing at that price and dies.

`threshold` is in engine fixed-point price units, like the quotes it compares.
"""
from __future__ import annotations

import bcs_engine as be
from contracts import Order


class EventSniper:
    def __init__(self, participant_id, qty, threshold=0, md_latency_ns=0, order_latency_ns=0):
        self.participant_id = int(participant_id)
        self.md_latency_ns = int(md_latency_ns)
        self.order_latency_ns = int(order_latency_ns)
        self.qty = int(qty)
        self.threshold = float(threshold)
        self._oid = self.participant_id * 1_000_000_000

    def _ioc(self, side, price):
        self._oid += 1
        return [Order(self._oid, side, price, self.qty, be.OrderType.IOC)]

    def on_start(self, t_ns, view):
        return []

    def on_signal(self, t_ns, update, view):
        ask = view.best_ask()
        if ask > 0 and update.value - ask > self.threshold:
            return self._ioc(be.Side.Buy, ask)
        bid = view.best_bid()
        if bid > 0 and bid - update.value > self.threshold:
            return self._ioc(be.Side.Sell, bid)
        return []

    def on_wakeup(self, t_ns, tag, view):
        return []

    def on_fill(self, t_ns, fill, view):
        return []
