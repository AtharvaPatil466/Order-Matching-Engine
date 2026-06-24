"""NoiseTrader — uninformed liquidity taker (Phase 1 baseline).

Each tick, with probability lambda_per_tick, it crosses the spread with an IOC
order on a random side: it pays the half-spread to the market maker. Over a run
its PnL is therefore negative — the control-group definition of an uninformed
trader. No fundamental-value signal, no sniping; that is the Phase 2 HFT.
"""
from __future__ import annotations

import numpy as np

import bcs_engine as be
from scheduler import Action


class NoiseTrader:
    def __init__(self, participant_id, lambda_per_tick=0.15, qty=10,
                 latency_us=0, seed=0):
        self.participant_id = int(participant_id)
        self.lambda_per_tick = float(lambda_per_tick)
        self.qty = int(qty)
        self.latency_us = int(latency_us)
        self._rng = np.random.default_rng(seed)
        self._oid = self.participant_id * 1_000_000_000
        self.inventory = 0
        self.cash = 0.0
        self.fills = 0

    def _next_oid(self):
        self._oid += 1
        return self._oid

    def act(self, t, market, fundamental):
        if self._rng.random() > self.lambda_per_tick:
            return []
        buy = self._rng.random() < 0.5
        best_bid, best_ask = market.best_bid(), market.best_ask()
        if buy:
            if best_ask <= 0:
                return []
            side, price = be.Side.Buy, best_ask
        else:
            if best_bid <= 0:
                return []
            side, price = be.Side.Sell, best_bid
        return [Action(
            participant_id=self.participant_id,
            order_id=self._next_oid(),
            side=side, price=price, quantity=self.qty,
            order_type=be.OrderType.IOC, submit_at=t + self.latency_us,
        )]

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
