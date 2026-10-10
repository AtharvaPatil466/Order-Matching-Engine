"""EventScheduler — a nanosecond discrete-event clock in which a maker's cancel
and a sniper's IOC genuinely race to the engine.

Why it exists: the tick model (scheduler.py) steps every agent on a 100 ms
grid, so a maker reacting to news and a sniper reacting to the same news land
in the same tick and the "race" is a jitter draw inside it (paper §5.2). BCS's
core event is different: news arrives, the maker sends a CANCEL for its stale
quote, the snipers send IOCs at it, and whichever message reaches the matching
engine first decides who eats the jump. Here every event carries its own
integer-nanosecond timestamp, so that race is decided by latencies alone.

Semantics, exactly as written in contracts.py:
  - An agent observes SignalUpdate u at u.t_ns + md_latency_ns (on_signal).
  - Each counterparty of a fill observes it at execution time + its own
    md_latency_ns (on_fill). Fill.t_ns is the execution time.
  - Orders/Cancels returned from a callback at t arrive at the engine at
    t + order_latency_ns + jitter, jitter ~ U{0..race_jitter_ns[pid]} drawn
    from the scheduler's seeded RNG (0 when the agent has no entry).
  - A Wakeup fires at its at_ns, with no latency.
  - Events due at the same nanosecond run in a seeded random permutation,
    never registration order. Events created AT an instant with zero delay
    form a fresh batch at that same instant (a delta cycle), run after
    everything already due then — so a zero-latency agent still cannot act on
    news before every agent has had the chance to observe it.
  - One callback's Orders/Cancels travel as one message burst: they arrive
    together and apply in the order returned, as a session's messages keep
    their order on a real venue. A maker's requote can therefore never
    overtake, and trade against, its own stale quote. Successive bursts from
    one agent also arrive in send order (FIFO), however wide its jitter.
    ponytail: a burst is applied atomically; another participant's message at
    the identical nanosecond cannot interleave inside it.

Race log: for every update flagged is_jump, the engine arrivals that agents'
on_signal reactions to THAT update produced, in the order the engine applied
them, each with its outcome. "Did the maker's cancel beat the IOC" is then a
lookup, and race win rates a count. Only on_signal reactions are attributed;
fill- or wakeup-driven orders never enter a race.

No P&L here: accounting consumes `trades`.
"""
from __future__ import annotations

import heapq
import random
from dataclasses import dataclass
from typing import Mapping, NamedTuple, Sequence

import bcs_engine as be
from contracts import Cancel, Fill, Order, SignalUpdate, TradeRecord, Wakeup
from scheduler import MarketView

_START, _SIGNAL, _WAKEUP, _FILL, _ARRIVE = range(5)
_NO_CAUSE = -1
_BUY = be.Side.Buy
_GTC = be.TimeInForce.GTC
_NOT_FOUND = be.RejectReason.OrderNotFound
_OUT_OF_RANGE = be.RejectReason.OutOfPriceRange
_RESTS_WHEN_UNFILLED = frozenset({be.OrderType.Limit, be.OrderType.PostOnly})


class BookSnapshot(NamedTuple):
    """Top of book after every event at or before t_ns. 0 means an empty side."""
    t_ns: int
    best_bid: int
    best_ask: int


class RaceArrival(NamedTuple):
    """One engine arrival in a race.

    kind: "cancel", or the order type in lower case ("ioc", "limit", ...).
    outcome: "filled" | "partial" | "missed" (unfilled IOC/FOK/market) |
    "rested" (unfilled limit) | "cancelled" | "too_late" (cancel of an order
    already gone) | an engine RejectReason name.
    """
    arrival_ns: int
    participant_id: int
    kind: str
    order_id: int
    outcome: str


@dataclass(frozen=True)
class Race:
    update_index: int
    update: SignalUpdate
    arrivals: tuple   # RaceArrival, in engine order


@dataclass(frozen=True)
class EventSimResult:
    trades: list          # TradeRecord, in execution order
    snapshots: list       # BookSnapshot every snapshot_every_ns over [0, duration)
    races: tuple          # Race, one per is_jump update, ascending
    duration_ns: int
    path: object          # the FundamentalPath
    agents: list
    n_events: int         # events dispatched; for throughput sizing

    @property
    def last_mid(self) -> float:
        """Last two-sided mid in the snapshots (fixed point); 0.0 if never two-sided.
        The mid mark accounting.decompose takes beside the true-V mark."""
        for s in reversed(self.snapshots):
            if s.best_bid > 0 and s.best_ask > 0:
                return (s.best_bid + s.best_ask) / 2.0
        return 0.0


class EventScheduler:
    """Runs one simulation on a started harness. One run per harness: the
    engine's book carries over, so a second run would start mid-session."""

    def __init__(self, harness, symbol, seed=0, snapshot_every_ns=10_000_000,
                 race_jitter_ns: Mapping[int, int] | None = None):
        if snapshot_every_ns <= 0:
            raise ValueError("snapshot_every_ns must be positive")
        jitter = dict(race_jitter_ns or {})
        if any(j < 0 for j in jitter.values()):
            raise ValueError("race_jitter_ns values must be non-negative")
        self._h = harness
        self._sym = symbol
        self._seed = seed
        self._every = int(snapshot_every_ns)
        self._jitter = jitter

    def run(self, agents: Sequence, path) -> EventSimResult:
        self._check_agents(agents)
        self._rng = random.Random(self._seed)
        self._view = MarketView(self._h, self._sym)
        self._by_id = {a.participant_id: a for a in agents}
        self._updates = list(path.updates())
        self._duration = int(path.duration_ns)
        self._heap = []
        self._seq = 0
        self._trades = []
        self._last_arrival = {}
        self._races ={i: [] for i, u in enumerate(self._updates) if u.is_jump}
        snapshots = []

        for a in agents:
            self._push(0, _START, a, None, _NO_CAUSE)
            if self._updates:
                self._push(self._updates[0].t_ns + a.md_latency_ns, _SIGNAL, a, 0, _NO_CAUSE)

        heap, pop, n_events, next_snap = self._heap, heapq.heappop, 0, 0
        while heap and heap[0][0] < self._duration:
            t = heap[0][0]
            batch = [pop(heap)]
            while heap and heap[0][0] == t:
                batch.append(pop(heap))
            if len(batch) > 1:
                self._rng.shuffle(batch)
            while next_snap < t:
                snapshots.append(self._snapshot(next_snap))
                next_snap += self._every
            for ev in batch:
                self._dispatch(ev)
            n_events += len(batch)
        while next_snap < self._duration:
            snapshots.append(self._snapshot(next_snap))
            next_snap += self._every

        races = tuple(Race(i, self._updates[i], tuple(log)) for i, log in self._races.items())
        return EventSimResult(self._trades, snapshots, races, self._duration, path,
                              list(agents), n_events)

    def _check_agents(self, agents):
        ids = [a.participant_id for a in agents]
        if len(set(ids)) != len(ids):
            raise ValueError(f"participant ids must be unique, got {ids}")
        if any(a.md_latency_ns < 0 or a.order_latency_ns < 0 for a in agents):
            raise ValueError("latencies must be non-negative")
        unknown = set(self._jitter) - set(ids)
        if unknown:
            raise ValueError(f"race_jitter_ns names unknown participants {sorted(unknown)}")

    def _push(self, t, kind, agent, data, cause):
        if t < self._duration:
            heapq.heappush(self._heap, (t, self._seq, kind, agent, data, cause))
            self._seq += 1

    def _snapshot(self, t):
        return BookSnapshot(t, self._h.best_bid(self._sym), self._h.best_ask(self._sym))

    def _dispatch(self, ev):
        t, _, kind, agent, data, cause = ev
        if kind == _ARRIVE:
            self._arrive(t, agent, data, cause)
            return
        if kind == _SIGNAL:
            self._chain_next_signal(agent, data)
            actions = agent.on_signal(t, self._updates[data], self._view)
            cause = data
        elif kind == _FILL:
            actions = agent.on_fill(t, data, self._view)
        elif kind == _WAKEUP:
            actions = agent.on_wakeup(t, data, self._view)
        else:
            actions = agent.on_start(t, self._view)
        if actions:
            self._schedule(t, agent, actions, cause)

    def _chain_next_signal(self, agent, i):
        # One pending observation per agent keeps the heap small however long
        # the path is; update i+1 is queued when update i is observed.
        nxt = i + 1
        if nxt == len(self._updates):
            return
        if self._updates[nxt].t_ns < self._updates[i].t_ns:
            raise ValueError("path updates must ascend in t_ns")
        self._push(self._updates[nxt].t_ns + agent.md_latency_ns, _SIGNAL, agent, nxt, _NO_CAUSE)

    def _schedule(self, t, agent, actions, cause):
        burst = []
        for a in actions:
            if isinstance(a, Wakeup):
                if a.at_ns < t:
                    raise ValueError(f"Wakeup at {a.at_ns} is before now ({t})")
                self._push(a.at_ns, _WAKEUP, agent, a.tag, _NO_CAUSE)
            elif isinstance(a, (Order, Cancel)):
                burst.append(a)
            else:
                raise TypeError(f"unknown action {a!r}")
        if burst:
            pid = agent.participant_id
            jitter = self._jitter.get(pid, 0)
            arrive = t + agent.order_latency_ns + (self._rng.randint(0, jitter) if jitter else 0)
            # A session is FIFO: when jitter is wider than the gap between two
            # of one agent's bursts, the later one must not overtake (or tie,
            # and be shuffled ahead of) the earlier, or its cancels would land
            # before the orders they target and leave those orders orphaned.
            last = self._last_arrival.get(pid, -1)
            if arrive <= last:
                arrive = last + 1
            self._last_arrival[pid] = arrive
            self._push(arrive, _ARRIVE, agent, burst, cause)

    def _arrive(self, t, agent, burst, cause):
        log = self._races.get(cause)
        pid = agent.participant_id
        for m in burst:
            if isinstance(m, Cancel):
                ok, reason, _ = self._h.submit_cancel(self._sym, m.order_id)
                kind = "cancel"
                outcome = "cancelled" if ok else ("too_late" if reason == _NOT_FOUND else reason.name)
            else:
                tif = m.tif if m.tif is not None else _GTC
                ok, reason, _ = self._h.submit_order(self._sym, m.order_id, pid, m.side, m.price,
                                                     m.quantity, m.order_type, tif)
                filled = self._record_trades(t)
                ok, reason = self._refused_at_book(ok, reason, pid, m)
                kind = m.order_type.name.lower()
                if not ok:
                    outcome = reason.name
                elif filled >= m.quantity:
                    outcome = "filled"
                elif filled:
                    outcome = "partial"
                else:
                    outcome = "rested" if m.order_type in _RESTS_WHEN_UNFILLED else "missed"
            if log is not None:
                log.append(RaceArrival(t, pid, kind, m.order_id, outcome))

    def _refused_at_book(self, ok, reason, pid, order):
        """Fold in a refusal the book made AFTER admission (submit said ok).

        OutOfPriceRange means the price lies outside the fixed band the book
        centred on its first price; the order silently leaves the agent off the
        book, so the run no longer models what it claims to and must not finish.
        """
        late = self._h.drain_rejects(self._sym)
        if not late:
            return ok, reason
        reason = late[-1][1]
        if reason == _OUT_OF_RANGE:
            raise RuntimeError(
                f"participant {pid}'s order {order.order_id} at price {order.price} was refused "
                "with OutOfPriceRange: it lies outside the fixed price band the engine centred "
                "on the first price it saw. Keep the path within that band.")
        return False, reason

    def _record_trades(self, t):
        """Record the trades the order just arriving produced; return its filled qty."""
        filled = 0
        for tr in self._h.drain_trades(self._sym):
            buy_aggr = tr.aggressor_side == _BUY
            self._trades.append(TradeRecord(t, tr.price, tr.quantity, tr.buyer_id,
                                            tr.seller_id, buy_aggr))
            self._deliver(tr.buyer_id, Fill(t, tr.buy_order_id, tr.price, tr.quantity,
                                            True, not buy_aggr))
            self._deliver(tr.seller_id, Fill(t, tr.sell_order_id, tr.price, tr.quantity,
                                             False, buy_aggr))
            filled += tr.quantity
        return filled

    def _deliver(self, pid, fill):
        agent = self._by_id.get(pid)
        if agent is not None:
            self._push(fill.t_ns + agent.md_latency_ns, _FILL, agent, fill, _NO_CAUSE)
