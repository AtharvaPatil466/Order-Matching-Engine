"""BatchAuctionScheduler — frequent batch auction on the verified C++ engine.

The continuous arm (LatencyScheduler) routes every order through the verified
C++ matcher, which matches immediately. A batch auction instead ACCUMULATES
orders for `batch_interval_ticks` and then clears them all at ONE uniform
price. Both arms share the same model-checked matching substrate: this
scheduler parks the book in TradingState.AuctionOpen (orders rest instead of
matching on arrival), lets Limit orders accumulate in the engine book, buffers
IOC/Market orders in Python for the current window, then flushes the buffer
and calls OrderBook::uncross() — the code path Auction.tla proves
SingleClearingPrice over — every batch_interval ticks. Unfilled flushed
remainders are cancelled (true to IOC), which also keeps the resting Limit
book two-sided between uncrosses (no spurious empty-book "gap" artifact).

Why a batch neutralizes the HFT race (BCS): submission TIME within an interval
is irrelevant — only price-time priority among accumulated orders at the
uncross matters. The market maker posts resting Limit quotes; its
(latency-delayed) cancel of a stale quote lands in the same batch before the
uncross, so the stale quote is gone by clearing time and the HFT's IOC finds
nothing to pick off at the stale price.

discover_uncross_price below is the RETIRED Python re-implementation of the
clearing rule, kept as a cross-check. It agrees with the engine on executable
volume but can pick a different clearing price when several prices tie on max
volume (tie-break cascade divergence — pinned in
tests/test_batch_auction.py::test_python_matcher_divergence_pinned and
reported in the paper, §4.4).

Produces the same SimResult shape as LatencyScheduler, so all existing metrics
(welfare decomposition, liquidity gaps, Kyle's lambda) apply unchanged.
"""
from __future__ import annotations

import heapq

import bcs_engine as be
from scheduler import Action, LatencyScheduler, SimResult  # noqa: F401  (Action re-exported for agents)


def discover_uncross_price(buys, sells, reference=None):
    """Uniform clearing price replicating OrderBook::discoverUncrossPrice.

    RETIRED from the simulation path — clearing now runs through the engine's
    own uncross() via the pybind bridge. Kept as the reference implementation
    the engine is cross-checked against; see the module docstring for the
    known tie-break divergence.

    buys / sells: iterables of (price, qty). Candidate prices are the populated
    limit levels on either side. Returns (price, volume, buy_surplus); price is
    None when nothing crosses. Tie-break cascade: maximize executable volume,
    minimize imbalance, minimize distance to `reference` (if > 0), then market
    pressure (buy surplus clears higher, sell surplus lower).
    """
    buys = [(int(p), int(q)) for p, q in buys if q > 0]
    sells = [(int(p), int(q)) for p, q in sells if q > 0]
    if not buys and not sells:
        return (None, 0, True)

    candidates = sorted({p for p, _ in buys} | {p for p, _ in sells})
    have_best = False
    best_price = None
    best_vol = 0
    best_imb = 0
    best_buy_surplus = True

    for p in candidates:
        cum_buy = sum(q for bp, q in buys if bp >= p)
        cum_sell = sum(q for sp, q in sells if sp <= p)
        vol = min(cum_buy, cum_sell)
        imb = abs(cum_buy - cum_sell)
        buy_surplus = cum_buy >= cum_sell

        if not have_best:
            better = True
        elif vol != best_vol:
            better = vol > best_vol
        elif imb != best_imb:
            better = imb < best_imb
        elif reference is not None and reference > 0:
            d_cur = abs(p - reference)
            d_best = abs(best_price - reference)
            if d_cur != d_best:
                better = d_cur < d_best
            else:
                better = (p > best_price) if best_buy_surplus else (p < best_price)
        else:
            better = (p > best_price) if best_buy_surplus else (p < best_price)

        if better:
            have_best = True
            best_price, best_vol, best_imb, best_buy_surplus = p, vol, imb, buy_surplus

    if best_vol == 0:
        return (None, 0, best_buy_surplus)
    return (best_price, best_vol, best_buy_surplus)


class BatchAuctionScheduler(LatencyScheduler):
    """LatencyScheduler with clearing deferred to a per-interval engine uncross.

    Same latency-queue semantics as the parent; only the clearing differs:
    the book is parked in AuctionOpen for the whole run, so nothing matches
    on arrival, and every batch_interval ticks the accumulated book is
    uncrossed at one uniform price by the verified engine.
    """

    def __init__(self, harness, symbol, dt_us, batch_interval_ticks):
        super().__init__(harness, symbol, dt_us)
        self.batch_interval = max(1, int(batch_interval_ticks))
        self._aggressive = []   # due IOC/Market Actions; current window only
        harness.set_trading_state(symbol, be.TradingState.AuctionOpen)

    def _submit_due(self, t):
        # Limits rest in the parked engine book. IOC/Market orders are held
        # back until the uncross so they never post visible liquidity
        # mid-window (they take part in clearing, not in quoting).
        while self._pending and self._pending[0][0] <= t:
            _, _, a = heapq.heappop(self._pending)
            if a.kind == "cancel":
                self._h.submit_cancel(self._sym, a.cancel_order_id)
            elif a.order_type == be.OrderType.Limit:
                self._h.submit_order(self._sym, a.order_id, a.participant_id,
                                     a.side, a.price, a.quantity,
                                     be.OrderType.Limit, be.TimeInForce.GTC)
            else:
                self._aggressive.append(a)

    def _uncross(self, t):
        # Flush the window's aggressive orders as parked limits, clear at one
        # uniform price through the verified engine, then cancel unfilled
        # remainders (IOC semantics: this window only). Cancelling a fully
        # filled id is a harmless OrderNotFound reject.
        flushed = [a.order_id for a in self._aggressive]
        for a in self._aggressive:
            self._h.submit_order(self._sym, a.order_id, a.participant_id,
                                 a.side, a.price, a.quantity,
                                 be.OrderType.Limit, be.TimeInForce.GTC)
        self._aggressive = []
        self._h.uncross(self._sym)
        for tr in self._h.drain_trades(self._sym):
            self._route_fill(t, tr)
        for oid in flushed:
            self._h.submit_cancel(self._sym, oid)

    def run(self, agents, fundamental, duration_us):
        for ag in agents:
            self._by_id[ag.participant_id] = ag
        t = 0
        duration_us = int(duration_us)
        tick = 0
        while t < duration_us:
            v = fundamental.step(t)
            self._tick_volume = 0
            self._tick_signed = 0
            for ag in agents:
                for action in ag.act(t, self.market, fundamental):
                    self._enqueue(action)
            self._submit_due(t)
            tick += 1
            if tick % self.batch_interval == 0:
                self._uncross(t)
            self._record(t, v)
            t += self.dt_us
        self._h.set_trading_state(self._sym, be.TradingState.Continuous)
        return SimResult(self.snapshots, self.trades, list(agents),
                         duration_us, self.dt_us)
