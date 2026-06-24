"""LatencyScheduler — orders agent actions by submit time and drives the engine.

The C++ engine has no latency model; this scheduler supplies it. Each tick it
steps the fundamental value, lets every agent emit Actions (each stamped with
submit_at = now + the agent's latency), then submits every action whose
submit_at has arrived, in submit-time order. Fills are drained after each
submit and routed back to the buyer/seller agents by participant id.

This is the layer the BCS submission "race" (Phase 2) plugs into: the race is
just many agents stamping near-identical submit_at times, resolved here in
time order. The baseline uses zero latency, so it degenerates to: each tick the
market maker requotes, then noise takers hit the fresh quotes.
"""
from __future__ import annotations

import heapq
from dataclasses import dataclass, field

import bcs_engine as be


@dataclass
class Action:
    """One intent from an agent. kind == 'order' submits; 'cancel' cancels."""
    participant_id: int
    submit_at: int                  # microseconds; scheduler submits when reached
    kind: str = "order"             # "order" | "cancel"
    order_id: int = 0
    side: object = None             # be.Side (order only)
    price: int = 0                  # fixed-point (order only)
    quantity: int = 0               # order only
    order_type: object = None       # be.OrderType (order only)
    tif: object = field(default=None)
    cancel_order_id: int = 0        # cancel only


class MarketView:
    """Read-only book view for agents. No market-data latency in the baseline."""

    def __init__(self, harness, symbol):
        self._h = harness
        self._sym = symbol

    def best_bid(self):
        return self._h.best_bid(self._sym)

    def best_ask(self):
        return self._h.best_ask(self._sym)

    def mid(self):
        return self._h.mid(self._sym)


@dataclass
class SimResult:
    snapshots: list   # per-tick dicts: t, best_bid, best_ask, mid, v, tick_volume, signed_volume
    trades: list      # per-fill dicts: t, price, quantity, buyer_id, seller_id, aggressor_buy
    agents: list
    duration_us: int
    dt_us: int


class LatencyScheduler:
    def __init__(self, harness, symbol, dt_us):
        self._h = harness
        self._sym = symbol
        self.dt_us = int(dt_us)
        self.market = MarketView(harness, symbol)
        self._pending = []          # heap of (submit_at, seq, Action)
        self._seq = 0
        self._by_id = {}
        self.snapshots = []
        self.trades = []
        self._tick_volume = 0
        self._tick_signed = 0

    def _enqueue(self, action):
        heapq.heappush(self._pending, (action.submit_at, self._seq, action))
        self._seq += 1

    def _submit_due(self, t):
        while self._pending and self._pending[0][0] <= t:
            _, _, a = heapq.heappop(self._pending)
            if a.kind == "cancel":
                self._h.submit_cancel(self._sym, a.cancel_order_id)
            else:
                tif = a.tif if a.tif is not None else be.TimeInForce.GTC
                self._h.submit_order(self._sym, a.order_id, a.participant_id,
                                     a.side, a.price, a.quantity, a.order_type, tif)
            for tr in self._h.drain_trades(self._sym):
                self._route_fill(t, tr)

    def _route_fill(self, t, tr):
        signed = tr.quantity if tr.aggressor_side == be.Side.Buy else -tr.quantity
        self._tick_volume += tr.quantity
        self._tick_signed += signed
        self.trades.append({
            "t": t, "price": tr.price, "quantity": tr.quantity,
            "buyer_id": tr.buyer_id, "seller_id": tr.seller_id,
            "aggressor_buy": tr.aggressor_side == be.Side.Buy,
        })
        buyer = self._by_id.get(tr.buyer_id)
        seller = self._by_id.get(tr.seller_id)
        if buyer is not None:
            buyer.on_fill(tr, True)
        if seller is not None:
            seller.on_fill(tr, False)

    def _record(self, t, v):
        self.snapshots.append({
            "t": t,
            "best_bid": self._h.best_bid(self._sym),
            "best_ask": self._h.best_ask(self._sym),
            "mid": self._h.mid(self._sym),
            "v": v,
            "tick_volume": self._tick_volume,
            "signed_volume": self._tick_signed,
        })

    def run(self, agents, fundamental, duration_us):
        for ag in agents:
            self._by_id[ag.participant_id] = ag
        t = 0
        duration_us = int(duration_us)
        while t < duration_us:
            v = fundamental.step(t)
            self._tick_volume = 0
            self._tick_signed = 0
            for ag in agents:
                for action in ag.act(t, self.market, fundamental):
                    self._enqueue(action)
            self._submit_due(t)
            self._record(t, v)
            t += self.dt_us
        return SimResult(self.snapshots, self.trades, list(agents),
                         duration_us, self.dt_us)
