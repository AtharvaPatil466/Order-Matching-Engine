"""EventBatchScheduler — BCS frequent batch auctions on the nanosecond event clock.

Why it exists: the tick model's batch arm (batch_auction.py) clears on a
100 ms grid and holds IOCs back until the window closes, then submits them
after every limit that arrived in the window. That re-times them: an IOC
loses price-time priority to limits that arrived after it, and same-price
IOCs fill in arrival order, so a latency race survives inside the batch
(paper/batch_interval1_note.md). BCS's design (QJE 2015, §VII) is
different: orders and cancels arrive at any time in an interval of length
tau; at the boundary everything outstanding clears at one uniform price;
submission time inside the interval does not matter.

Semantics implemented here, on top of EventScheduler's clock and latencies:
  - Interval k is the half-open window [k*tau, (k+1)*tau). Its uncross runs at
    (k+1)*tau BEFORE any arrival at that exact nanosecond, so an arrival at a
    boundary belongs to the next batch, deterministically. (The uncross sits
    at boundary - 0.5 ns on the heap, so the parent's same-instant shuffle
    never mixes it with arrivals.)
  - Cancels take effect on arrival. The book is sealed (below), so applying a
    cancel on arrival or at the boundary yields the same auction; what
    matters, and what BCS and every call-auction venue guarantee, is that a
    cancel received during interval k is honoured before uncross k. Applying
    it on arrival is also what the engine does natively in AuctionOpen.
  - New orders are held until the boundary, then submitted to the engine in
    an order set by `rationing` and cleared by the engine's own uncross():
      "time"   arrival order — the engine's native price-time rule;
      "random" a seeded shuffle of the window's orders (own RNG stream, so
               the event stream is identical across rationing arms).
    Orders resting from earlier batches stay ahead of both: rationing acts on
    the window only, as specified.
  - IOC means "this auction only": it enters the uncross as a GTC limit and
    its remainder is cancelled immediately afterwards, so it never rests and
    is never visible. (The engine rejects IOC outright in AuctionOpen.)
    Market orders go to the engine natively, which parks them and cancels
    their remainder itself.
  - Sealed book: agents (and the snapshots) see the book as it stood right
    after the last uncross, BCS's information policy — orders inside an
    interval are not displayed.
  - Clearing price, `price_rule`:
      "engine"   the engine's cascade: max volume, min imbalance, nearest the
                 reference price, then pressure. The reference is seeded by
                 the session's FIRST limit order and is not updated by
                 uncross() (verified in tests/test_event_batch.py), so ties
                 resolve toward the opening price, not the last print.
      "pressure" the same cascade without the reference step (buy surplus
                 clears at the highest tied price, sell surplus at the
                 lowest). This is BCS's Bertrand outcome for a stale quote
                 facing several bidders (Prop 6(iii)); see PREREGISTRATION P9.
    The engine cannot do "pressure" itself, so the scheduler reprices: the
    engine still decides WHO trades and HOW MUCH; only the price moves, to
    the retired discover_uncross_price (reference=None) on the pre-uncross
    book. A reprice happens only when that rule clears the same volume and
    its price lies between every filled seller's limit and every filled
    buyer's limit, which makes the engine's fills exactly the fills at the
    new price (priority order does not depend on the price); otherwise the
    engine price stands and the batch is counted in `reprice_refused`.
    ponytail: when the tie is an interval where supply equals demand, BCS
    take its midpoint; this rule takes the cascade's end of it.
  - A refused order is reported to its sender's on_reject from the boundary
    (when the engine first sees it), a refused cancel from its arrival.
  - No aggressor in an auction (the engine stamps every uncross trade
    aggressor=Buy). For fee purposes the order that arrived later is the
    taker; an order resting from an earlier batch is always the maker.

Pro-rata rationing is left out. The engine's uncross only fills price-time;
emulating pro-rata means splitting every order into unit lots, interleaving
them by size, and cancelling and re-splitting carried-over orders each batch,
which is exact only to one lot and multiplies engine traffic by the lot
size. With equal-size snipers "random" already gives each the pro-rata share
in expectation, and rationing never moves the clearing price or the volume.

ponytail: a participant's own crossing buy and sell in one batch are removed
by the engine's self-trade prevention before the cross; model an investor
population as many participants if that matters (exp11 does).
"""
from __future__ import annotations

import random

import bcs_engine as be
from batch_auction import discover_uncross_price
from contracts import Cancel, Fill, TradeRecord
from event_scheduler import (_NO_CAUSE, _NOT_FOUND, _RESTS_WHEN_UNFILLED, BookSnapshot,
                             EventScheduler, RaceArrival)

_UNCROSS = 6          # heap event kind, after the parent's six
_HALF_NS = 0.5
_GTC = be.TimeInForce.GTC
_LIMIT = be.OrderType.Limit
_IOC = be.OrderType.IOC
_MARKET = be.OrderType.Market
_BOOK_DEPTH = 1_000_000   # snapshot depth: every level
RATIONING = ("time", "random")
PRICE_RULES = ("engine", "pressure")


class _SealedView:
    """The book as published after the last uncross."""

    def __init__(self):
        self.bid = 0
        self.ask = 0

    def best_bid(self):
        return self.bid

    def best_ask(self):
        return self.ask

    def mid(self):
        return (self.bid + self.ask) // 2 if self.bid > 0 and self.ask > 0 else 0


class EventBatchScheduler(EventScheduler):
    """EventScheduler whose engine book clears only at batch boundaries."""

    def __init__(self, harness, symbol, interval_ns, rationing="time", price_rule="engine",
                 seed=0, snapshot_every_ns=10_000_000, race_jitter_ns=None):
        super().__init__(harness, symbol, seed, snapshot_every_ns, race_jitter_ns)
        if int(interval_ns) <= 0:
            raise ValueError("interval_ns must be positive")
        if rationing not in RATIONING:
            raise ValueError(f"rationing must be one of {RATIONING}")
        if price_rule not in PRICE_RULES:
            raise ValueError(f"price_rule must be one of {PRICE_RULES}")
        self._tau = int(interval_ns)
        self._rationing = rationing
        self._price_rule = price_rule
        self._sealed = _SealedView()

    # run() installs a live MarketView as self._view; agents here see the
    # sealed book instead, so the assignment is ignored.
    @property
    def _view(self):
        return self._sealed

    @_view.setter
    def _view(self, _live):
        pass

    def run(self, agents, path):
        self._h.set_trading_state(self._sym, be.TradingState.AuctionOpen)
        self._window = {}           # order id -> (agent, Order, race slot, arrival rank)
        self._limit = {}            # order id -> limit price, every order submitted
        self._rank = 0
        self._armed = False
        self._dirty = False         # a cancel changed the book since the last publish
        self._ration_rng = random.Random(f"ration-{self._seed}")
        self.n_batches = 0          # boundaries that cleared a non-empty window
        self.repriced = 0
        self.reprice_refused = 0
        return super().run(agents, path)

    def _push(self, t, kind, agent, data, cause):
        if not self._armed:         # first push of the run: start the clock
            self._armed = True
            super()._push(self._tau - _HALF_NS, _UNCROSS, None, None, _NO_CAUSE)
        super()._push(t, kind, agent, data, cause)

    def _dispatch(self, ev):
        if ev[2] == _UNCROSS:
            self._uncross(int(ev[0] + _HALF_NS))
        else:
            super()._dispatch(ev)

    def _snapshot(self, t):
        return BookSnapshot(t, self._sealed.bid, self._sealed.ask)

    # --- arrivals -----------------------------------------------------------

    def _arrive(self, t, agent, burst, cause):
        log = self._races.get(cause)
        pid = agent.participant_id
        for m in burst:
            if isinstance(m, Cancel):
                outcome = self._cancel(t, agent, m.order_id)
                if log is not None:
                    log.append(RaceArrival(t, pid, "cancel", m.order_id, outcome))
                continue
            if m.order_type == _MARKET and self._price_rule == "pressure":
                raise ValueError("price_rule='pressure' cannot see parked market orders")
            if m.order_id in self._window:
                raise ValueError(f"duplicate order id {m.order_id} in one batch")
            slot = None
            if log is not None:
                log.append(RaceArrival(t, pid, m.order_type.name.lower(), m.order_id, "pending"))
                slot = (log, len(log) - 1)
            self._window[m.order_id] = (agent, m, slot, self._rank)
            self._rank += 1

    def _cancel(self, t, agent, oid):
        held = self._window.pop(oid, None)
        if held is not None:
            _settle(held[2], "cancelled")   # never reached the auction
            return "cancelled"
        ok, reason, _ = self._h.submit_cancel(self._sym, oid)
        self._dirty = self._dirty or ok
        if not ok:
            self._reject(t, agent, oid, reason.name)
        return "cancelled" if ok else ("too_late" if reason == _NOT_FOUND else reason.name)

    # --- the boundary -------------------------------------------------------

    def _uncross(self, b):
        self._push(b + self._tau - _HALF_NS, _UNCROSS, None, None, _NO_CAUSE)
        if self._window:
            self._clear(b)
        elif not self._dirty:
            return          # nothing arrived: the published book still stands
        self._sealed.bid = self._h.best_bid(self._sym)
        self._sealed.ask = self._h.best_ask(self._sym)
        self._dirty = False

    def _clear(self, b):
        entries = list(self._window.values())
        self._window = {}
        if self._rationing == "random":
            self._ration_rng.shuffle(entries)
        rejected = self._submit(entries)
        for agent, m, _, _ in entries:
            if m.order_id in rejected:      # the engine first saw it at the boundary
                self._reject(b, agent, m.order_id, rejected[m.order_id])
        target = self._pressure_target() if self._price_rule == "pressure" else None
        self._h.uncross(self._sym)
        trades = self._h.drain_trades(self._sym)
        price = self._clearing_price(trades, target)
        rank = {e[1].order_id: e[3] for e in entries}
        filled = self._record(b, trades, price, rank)
        for agent, m, slot, _ in entries:
            if m.order_type == _IOC and m.order_id not in rejected:
                if filled.get(m.order_id, 0) < m.quantity:
                    self._h.submit_cancel(self._sym, m.order_id)
            _settle(slot, rejected.get(m.order_id) or _outcome(m, filled.get(m.order_id, 0)))
        self.n_batches += 1

    def _submit(self, entries):
        """Hand the window to the engine; return {order id: reject reason name}."""
        rejected = {}
        for agent, m, _, _ in entries:
            otype = _LIMIT if m.order_type == _IOC else m.order_type
            self._limit[m.order_id] = m.price
            tif = m.tif if m.tif is not None else _GTC
            ok, reason, _ = self._h.submit_order(self._sym, m.order_id, agent.participant_id,
                                                 m.side, m.price, m.quantity, otype, tif)
            # The parked book refuses an out-of-band price after admission;
            # fold that in (and fail the run on it), as the continuous path does.
            ok, reason = self._refused_at_book(ok, reason, agent.participant_id, m)
            if not ok:
                rejected[m.order_id] = reason.name
        return rejected

    def _pressure_target(self):
        """(price, volume) of the reference-free cascade on the pre-uncross book."""
        snap = self._h.snapshot(self._sym, _BOOK_DEPTH)
        price, vol, _ = discover_uncross_price([(lv.price, lv.total_quantity) for lv in snap.bids],
                                               [(lv.price, lv.total_quantity) for lv in snap.asks],
                                               reference=None)
        return None if price is None else (price, vol)

    def _clearing_price(self, trades, target):
        """The price every trade of this batch is recorded at."""
        if not trades:
            return None
        engine_price = trades[0].price
        if target is None or target[0] == engine_price:
            return engine_price
        price, vol = target
        lo = max(self._limit[t.sell_order_id] for t in trades)
        hi = min(self._limit[t.buy_order_id] for t in trades)
        if vol != sum(t.quantity for t in trades) or not lo <= price <= hi:
            self.reprice_refused += 1
            return engine_price
        self.repriced += 1
        return price

    def _record(self, b, trades, price, rank):
        """Record and deliver this batch's trades; return filled qty per window order."""
        filled = {}
        for tr in trades:
            # The later arrival is the taker; an order resting from an
            # earlier batch (no rank) is always the maker.
            buy_aggr = rank.get(tr.buy_order_id, -1) > rank.get(tr.sell_order_id, -1)
            self._trades.append(TradeRecord(b, price, tr.quantity, tr.buyer_id, tr.seller_id,
                                            buy_aggr))
            self._deliver(tr.buyer_id, Fill(b, tr.buy_order_id, price, tr.quantity, True,
                                            not buy_aggr))
            self._deliver(tr.seller_id, Fill(b, tr.sell_order_id, price, tr.quantity, False,
                                             buy_aggr))
            for oid in (tr.buy_order_id, tr.sell_order_id):
                filled[oid] = filled.get(oid, 0) + tr.quantity
        return filled


def _outcome(m, filled):
    if filled >= m.quantity:
        return "filled"
    if filled:
        return "partial"
    return "rested" if m.order_type in _RESTS_WHEN_UNFILLED else "missed"


def _settle(slot, outcome):
    if slot is not None:
        log, i = slot
        log[i] = log[i]._replace(outcome=outcome)
