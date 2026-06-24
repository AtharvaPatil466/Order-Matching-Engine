"""HFTAgent — latency-advantaged stale-quote sniper (Phase 2, BCS Step 5).

At each tick the HFT observes the fundamental value V with a SMALL latency (its
advantage) and the current best bid/ask. When V has moved beyond a resting
quote by more than transaction cost, the quote is stale and the HFT races an
aggressive IOC to pick it off:
  - V_obs > best_ask + cost  -> ask is stale-low  -> buy IOC at best_ask
  - V_obs < best_bid - cost  -> bid is stale-high -> sell IOC at best_bid
The race is a random submission jitter in [0, race_noise_us]; with several HFTs
the lowest jitter wins and the others' IOCs find the quote already gone.

The counterparty is the market maker, which observes V with a LARGER latency
and is therefore pickable off — that gap is the BCS arbitrage.
"""
from __future__ import annotations

import numpy as np

import bcs_engine as be
from scheduler import Action


class HFTAgent:
    def __init__(self, participant_id, latency_us=0, race_noise_us=0,
                 transaction_cost_bps=0.0, qty=50, seed=0):
        self.participant_id = int(participant_id)
        self.latency_us = int(latency_us)
        self.race_noise_us = int(race_noise_us)
        self.transaction_cost = float(transaction_cost_bps) / 10000.0
        self.qty = int(qty)
        self._rng = np.random.default_rng(seed)
        self._oid = self.participant_id * 1_000_000_000
        self.inventory = 0
        self.cash = 0.0
        self.fills = 0
        self.snipes = 0          # IOC orders sent (opportunities acted on)

    def _next_oid(self):
        self._oid += 1
        return self._oid

    def _jitter(self):
        if self.race_noise_us <= 0:
            return 0
        return int(self._rng.uniform(0, self.race_noise_us))

    def act(self, t, market, fundamental):
        v = fundamental.observe(t, self.latency_us)
        best_bid, best_ask = market.best_bid(), market.best_ask()

        # Buy snipe: ask is stale-low (true value above the ask + cost).
        if best_ask > 0 and (v - best_ask) > self.transaction_cost * best_ask:
            self.snipes += 1
            return [Action(participant_id=self.participant_id,
                           order_id=self._next_oid(),
                           side=be.Side.Buy, price=best_ask, quantity=self.qty,
                           order_type=be.OrderType.IOC,
                           submit_at=t + self.latency_us + self._jitter())]

        # Sell snipe: bid is stale-high.
        if best_bid > 0 and (best_bid - v) > self.transaction_cost * best_bid:
            self.snipes += 1
            return [Action(participant_id=self.participant_id,
                           order_id=self._next_oid(),
                           side=be.Side.Sell, price=best_bid, quantity=self.qty,
                           order_type=be.OrderType.IOC,
                           submit_at=t + self.latency_us + self._jitter())]
        return []

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
