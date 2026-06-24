"""BCSMarketMaker — adverse-selection-aware quoting (Phase 2, BCS Step 6).

Unlike BaselineMarketMaker, this MM reacts to being picked off. It quotes two
sided around its latency-DELAYED view of V, so its quotes go stale.

Two mechanisms make the BCS feedback loop operate (an earlier all-fills version
produced no HFT-specific divergence and no crashes — see the calibration
negative result):

  Fix 1 (adverse-only widening): the MM widens ONLY on fills that were against
  informed flow — it sold below, or bought above, the TRUE current V (the value
  it eventually learns). Noise fills sit inside [bid, ask] around V and are not
  adverse, so they do not widen. This separates the HFT pickoff signal from the
  noise-fill background that previously saturated the response.

  Fix 2 (liquidity thins): effective quote SIZE is tied to the same widening
  state — as the half-spread widens, resting size shrinks toward min_quote_qty.
  A thin book lets the next order move the mid further (larger price impact),
  which is the amplification link that turns widening into crash dynamics:
  HFT snipes -> MM widens + thins -> next move has larger impact -> more quotes
  go stale -> more sniping.

Calibrate (adverse_sensitivity, spread_decay) so that WITHOUT HFTs the spread
sits near base; that calibration is done by the experiment driver, not here.
"""
from __future__ import annotations

import bcs_engine as be
from scheduler import Action


class BCSMarketMaker:
    def __init__(self, participant_id, base_half_spread, quote_qty=50,
                 latency_us=0, inventory_skew=0.0, requote_every=1,
                 adverse_sensitivity=0.1, spread_decay=0.05, spread_cap_mult=10.0,
                 thin_liquidity=True, min_quote_qty=1, adverse_threshold_ticks=0.0):
        self.participant_id = int(participant_id)
        self.base_half_spread = float(base_half_spread)
        self.current_half_spread = float(base_half_spread)
        self.quote_qty = int(quote_qty)
        self.latency_us = int(latency_us)
        self.inventory_skew = float(inventory_skew)
        self.requote_every = max(1, int(requote_every))
        self.adverse_sensitivity = float(adverse_sensitivity)
        self.spread_decay = float(spread_decay)
        self.spread_cap = float(base_half_spread) * float(spread_cap_mult)
        # Fix 2: liquidity thins as the half-spread widens (one coupled state).
        self.thin_liquidity = bool(thin_liquidity)
        self.min_quote_qty = max(1, int(min_quote_qty))
        # Fix 1: a fill counts as adverse only if priced beyond true V by this margin.
        self.adverse_threshold_ticks = float(adverse_threshold_ticks)
        self._fundamental = None
        self._oid = self.participant_id * 1_000_000_000
        self._tick = 0
        self.active_bid = None
        self.active_ask = None
        self.inventory = 0
        self.cash = 0.0
        self.fills = 0
        self.pickoffs = 0
        self.max_half_spread_seen = float(base_half_spread)
        self.min_quote_qty_seen = int(quote_qty)

    def _effective_qty(self):
        # Thin the resting size in proportion to how far the half-spread has
        # widened above base; floor at min_quote_qty. Constant when disabled.
        if not self.thin_liquidity:
            return self.quote_qty
        ratio = self.base_half_spread / self.current_half_spread
        return max(self.min_quote_qty, int(round(self.quote_qty * ratio)))

    def _next_oid(self):
        self._oid += 1
        return self._oid

    @property
    def current_spread_ticks(self):
        return 2.0 * self.current_half_spread

    def act(self, t, market, fundamental):
        self._fundamental = fundamental  # used by on_fill for the true-V check
        self._tick += 1
        if (self._tick - 1) % self.requote_every != 0:
            return []
        # Decay the half-spread back toward base (recovery when not picked off).
        self.current_half_spread = (self.current_half_spread * (1 - self.spread_decay)
                                    + self.base_half_spread * self.spread_decay)
        v = fundamental.observe(t, self.latency_us)
        qty = self._effective_qty()
        if qty < self.min_quote_qty_seen:
            self.min_quote_qty_seen = qty
        hs = self.current_half_spread
        skew = self.inventory * self.inventory_skew
        bid = int(round(v - hs - skew))
        ask = int(round(v + hs - skew))
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
                              side=be.Side.Buy, price=bid, quantity=qty,
                              order_type=be.OrderType.Limit, submit_at=sa))
        actions.append(Action(participant_id=self.participant_id, order_id=ask_oid,
                              side=be.Side.Sell, price=ask, quantity=qty,
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
        # Fix 1 — adverse-selection detection. Widen ONLY when the fill was
        # against informed flow: the MM sold below, or bought above, the TRUE
        # current V (the value it eventually learns). A stale quote is picked
        # off exactly in that case. Noise fills sit inside [bid, ask] around V
        # and are NOT adverse, so they leave the spread untouched.
        true_v = getattr(self._fundamental, "current_v", None)
        if true_v is None:
            return
        if is_buyer:
            adverse = (trade.price - true_v) > self.adverse_threshold_ticks
        else:
            adverse = (true_v - trade.price) > self.adverse_threshold_ticks
        if not adverse:
            return
        self.pickoffs += 1
        self.current_half_spread = min(
            self.current_half_spread * (1 + self.adverse_sensitivity),
            self.spread_cap)
        if self.current_half_spread > self.max_half_spread_seen:
            self.max_half_spread_seen = self.current_half_spread

    def pnl(self, mark_price):
        return self.cash + self.inventory * mark_price / be.PRICE_PRECISION
