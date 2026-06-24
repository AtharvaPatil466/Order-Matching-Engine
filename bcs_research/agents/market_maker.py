"""BaselineMarketMaker — two-sided quoting around a (possibly delayed) view of V.

Baseline = NO adverse-selection feedback loop (that is the Phase 2
BCSMarketMaker). It cancels and reposts both quotes each tick around its
observation of the fundamental value, skewing against inventory. With zero
latency and no HFT present it simply earns the half-spread from noise takers.
"""
from __future__ import annotations

import bcs_engine as be
from scheduler import Action


class BaselineMarketMaker:
    def __init__(self, participant_id, half_spread, quote_qty=50,
                 latency_us=0, inventory_skew=0.0, requote_every=1):
        self.participant_id = int(participant_id)
        self.half_spread = int(half_spread)
        self.quote_qty = int(quote_qty)
        self.latency_us = int(latency_us)
        self.inventory_skew = float(inventory_skew)
        self.requote_every = max(1, int(requote_every))
        self._oid = self.participant_id * 1_000_000_000
        self._tick = 0
        self.active_bid = None
        self.active_ask = None
        self.inventory = 0
        self.cash = 0.0
        self.fills = 0

    def _next_oid(self):
        self._oid += 1
        return self._oid

    def act(self, t, market, fundamental):
        self._tick += 1
        if (self._tick - 1) % self.requote_every != 0:
            return []
        v = fundamental.observe(t, self.latency_us)
        skew = self.inventory * self.inventory_skew
        bid = int(round(v - self.half_spread - skew))
        ask = int(round(v + self.half_spread - skew))
        if bid < 1:
            bid = 1
        if ask <= bid:
            ask = bid + 1
        sa = t + self.latency_us
        actions = []
        if self.active_bid is not None:
            actions.append(Action(participant_id=self.participant_id, kind="cancel",
                                  cancel_order_id=self.active_bid, submit_at=sa))
        if self.active_ask is not None:
            actions.append(Action(participant_id=self.participant_id, kind="cancel",
                                  cancel_order_id=self.active_ask, submit_at=sa))
        bid_oid = self._next_oid()
        ask_oid = self._next_oid()
        actions.append(Action(participant_id=self.participant_id, order_id=bid_oid,
                              side=be.Side.Buy, price=bid, quantity=self.quote_qty,
                              order_type=be.OrderType.Limit, submit_at=sa))
        actions.append(Action(participant_id=self.participant_id, order_id=ask_oid,
                              side=be.Side.Sell, price=ask, quantity=self.quote_qty,
                              order_type=be.OrderType.Limit, submit_at=sa))
        self.active_bid = bid_oid
        self.active_ask = ask_oid
        return actions

    def on_fill(self, trade, is_buyer):
        notional = trade.price * trade.quantity / be.PRICE_PRECISION
        if is_buyer:
            self.inventory += trade.quantity
            self.cash -= notional
        else:
            self.inventory -= trade.quantity
            self.cash += notional
        self.fills += 1

    def pnl(self, mark_price):
        return self.cash + self.inventory * mark_price / be.PRICE_PRECISION
