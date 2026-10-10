"""Tests for EventScheduler against the real bcs_engine.

Scripted agents pin the scheduler's own semantics (contracts.py): when each
callback fires, when each message reaches the engine, how exact ties are
broken, that one callback's burst keeps its order, that zero latency cannot
jump the news, and what a snapshot means. The maker-vs-sniper race itself is
in test_race.py, which reuses this file's ListPath and run().
"""
import bisect
import itertools
import sys
from pathlib import Path

_R = Path(__file__).resolve().parents[1]
for _d in ("build", "simulation", "agents"):
    if str(_R / _d) not in sys.path:
        sys.path.insert(0, str(_R / _d))

import pytest  # noqa: E402

import bcs_engine as be  # noqa: E402
from contracts import Cancel, Fill, Order, SignalUpdate, TradeRecord, Wakeup  # noqa: E402
from event_scheduler import BookSnapshot, EventScheduler, RaceArrival  # noqa: E402

SYM = 1
P = be.PRICE_PRECISION
V0 = 100 * P
BUY, SELL = be.Side.Buy, be.Side.Sell
LIMIT, IOC = be.OrderType.Limit, be.OrderType.IOC


class ListPath:
    """Tiny deterministic FundamentalPath over an explicit list of updates."""

    def __init__(self, updates, duration_ns, v0=V0):
        self.duration_ns = duration_ns
        self._updates = tuple(updates)
        self._times = [u.t_ns for u in self._updates]
        self._v0 = v0

    def value_at(self, t_ns):
        i = bisect.bisect_right(self._times, t_ns) - 1
        return self._updates[i].value if i >= 0 else self._v0

    def updates(self):
        return self._updates


def run(agents, path, seed=0, **kwargs):
    h = be.EngineHarness()
    h.add_symbol(SYM)
    h.start(disable_circuit_breaker=True)
    return EventScheduler(h, SYM, seed=seed, **kwargs).run(agents, path)


class Scripted:
    """An EventAgent whose reactions are plain functions; records every callback."""

    def __init__(self, pid, md=0, order=0, **script):
        self.participant_id = pid
        self.md_latency_ns = md
        self.order_latency_ns = order
        self.script = script
        self.calls = []

    def _call(self, name, t_ns, payload, view):
        self.calls.append((name, t_ns, payload))
        fn = self.script.get(name)
        return fn(t_ns, payload, view) if fn else []

    def on_start(self, t_ns, view):
        return self._call("start", t_ns, None, view)

    def on_signal(self, t_ns, update, view):
        return self._call("signal", t_ns, update, view)

    def on_wakeup(self, t_ns, tag, view):
        return self._call("wakeup", t_ns, tag, view)

    def on_fill(self, t_ns, fill, view):
        return self._call("fill", t_ns, fill, view)

    def times(self, name):
        return [t for n, t, _ in self.calls if n == name]


def test_callbacks_and_arrivals_land_exactly_at_their_latencies():
    # Arrange: a resting seller posts on the first update; a taker wakes at
    # 50 us and lifts it. Every latency is distinct so a mix-up shows.
    updates = [SignalUpdate(1_000, V0, is_jump=True), SignalUpdate(5_000, V0 + P),
               SignalUpdate(9_000, V0)]
    seller = Scripted(1, md=1_234, order=567,
                      signal=lambda t, u, v: [Order(11, SELL, V0, 5, LIMIT)] if u.t_ns == 1_000 else [])
    taker = Scripted(2, md=89, order=10,
                     start=lambda t, _, v: [Wakeup(50_000, "go")],
                     wakeup=lambda t, tag, v: [Order(21, BUY, V0, 5, IOC)])

    # Act
    res = run([seller, taker], ListPath(updates, 1_000_000))

    # Assert
    assert seller.times("signal") == [u.t_ns + 1_234 for u in updates]
    assert taker.times("signal") == [u.t_ns + 89 for u in updates]
    assert taker.calls[-2] == ("wakeup", 50_000, "go")
    assert res.races[0].arrivals == (RaceArrival(1_000 + 1_234 + 567, 1, "limit", 11, "rested"),)
    executed = 50_000 + 10
    assert res.trades == [TradeRecord(executed, V0, 5, 2, 1, True)]
    assert seller.calls[-1] == ("fill", executed + 1_234, Fill(executed, 11, V0, 5, False, True))
    assert taker.calls[-1] == ("fill", executed + 89, Fill(executed, 21, V0, 5, True, False))


def test_race_jitter_delays_each_burst_within_its_bound_and_only_for_that_agent():
    # Arrange: both agents send a (harmless) cancel on every jump.
    updates = [SignalUpdate(k * 100_000, V0, is_jump=True) for k in range(200)]
    jittered = Scripted(1, order=1_000, signal=lambda t, u, v: [Cancel(0)])
    steady = Scripted(2, order=1_000, signal=lambda t, u, v: [Cancel(0)])

    # Act
    res = run([jittered, steady], ListPath(updates, 200 * 100_000), race_jitter_ns={1: 500})

    # Assert
    delays = {pid: [a.arrival_ns - r.update.t_ns for r in res.races for a in r.arrivals
                    if a.participant_id == pid] for pid in (1, 2)}
    assert all(1_000 <= d <= 1_500 for d in delays[1]) and len(set(delays[1])) > 50
    assert set(delays[2]) == {1_000}


def test_exact_arrival_ties_are_broken_by_seeded_permutation_not_registration_order():
    # Arrange: one lot is posted every ms; two identical buyers' IOCs for it
    # arrive at the same nanosecond, so only the tie-break decides who fills.
    # 400 races: 4 sigma of a fair coin is +-40 wins around 200.
    n_races, ids = 400, itertools.count(1)
    every = lambda t: Wakeup(t + 1_000_000)  # noqa: E731
    seller = Scripted(1, start=lambda t, _, v: [Wakeup(0)],
                      wakeup=lambda t, _, v: [Order(next(ids), SELL, V0, 1, LIMIT), every(t)])
    buyers = [Scripted(pid, order=100, start=lambda t, _, v: [Wakeup(500)],
                       wakeup=lambda t, _, v: [Order(next(ids), BUY, V0, 1, IOC), every(t)])
              for pid in (2, 3)]

    # Act
    res = run([seller, *buyers], ListPath([], n_races * 1_000_000), seed=5)

    # Assert
    assert len(res.trades) == n_races
    for pid in (2, 3):
        wins = sum(tr.buyer_id == pid for tr in res.trades)
        assert 160 < wins < 240


def test_one_callbacks_burst_keeps_its_order_while_rival_bursts_are_shuffled():
    # Arrange: on each jump two agents with equal latency each post, cancel,
    # and post again — a reordered burst would show a "too_late" cancel.
    ids = itertools.count(1)

    def post_cancel_post(t, u, v):
        a, b = next(ids), next(ids)
        return [Order(a, BUY, V0 - P, 1, LIMIT), Cancel(a), Order(b, BUY, V0 - 2 * P, 1, LIMIT)]

    agents = [Scripted(pid, order=1_000, signal=post_cancel_post) for pid in (1, 2)]
    updates = [SignalUpdate(k * 10_000, V0, is_jump=True) for k in range(50)]

    # Act
    res = run(agents, ListPath(updates, 50 * 10_000), seed=3)

    # Assert
    firsts = set()
    for race in res.races:
        a = race.arrivals
        assert [x.outcome for x in a] == ["rested", "cancelled", "rested"] * 2
        assert len({x.participant_id for x in a[:3]}) == 1 == len({x.participant_id for x in a[3:]})
        firsts.add(a[0].participant_id)
    assert firsts == {1, 2}


def test_zero_latency_orders_land_after_every_agent_has_observed_the_same_update():
    # Arrange / Act: across seeds, a zero-latency poster's order must never be
    # visible to a zero-latency watcher reacting to the same update.
    seen = []
    for seed in range(20):
        poster = Scripted(1, signal=lambda t, u, v: [Order(1, SELL, V0, 1, LIMIT)])
        watcher = Scripted(2, signal=lambda t, u, v: seen.append(v.best_ask()) or [])
        res = run([poster, watcher], ListPath([SignalUpdate(0, V0)], 10),
                  seed=seed, snapshot_every_ns=5)

        # Assert: the order did land, at the same instant
        assert res.snapshots[0] == BookSnapshot(0, 0, V0)
    assert seen == [0] * 20


def test_snapshot_reports_the_book_after_every_event_at_or_before_its_time():
    # Arrange: two quotes posted at start arrive at 2 us.
    quoter = Scripted(1, order=2_000, start=lambda t, _, v: [
        Order(1, BUY, V0 - P, 1, LIMIT), Order(2, SELL, V0 + P, 1, LIMIT)])

    # Act
    res = run([quoter], ListPath([], 5_000), snapshot_every_ns=1_000)

    # Assert
    quoted = (V0 - P, V0 + P)
    assert res.snapshots == [BookSnapshot(0, 0, 0), BookSnapshot(1_000, 0, 0),
                             BookSnapshot(2_000, *quoted), BookSnapshot(3_000, *quoted),
                             BookSnapshot(4_000, *quoted)]


def test_wakeup_in_the_past_is_rejected():
    agent = Scripted(1, start=lambda t, _, v: [Wakeup(5_000)], wakeup=lambda t, _, v: [Wakeup(t - 1)])
    with pytest.raises(ValueError, match="before now"):
        run([agent], ListPath([], 10_000))


def test_duplicate_participant_ids_are_rejected():
    with pytest.raises(ValueError, match="unique"):
        run([Scripted(1), Scripted(1)], ListPath([], 1))


def test_path_updates_out_of_order_are_rejected():
    updates = [SignalUpdate(5_000, V0), SignalUpdate(1_000, V0)]
    with pytest.raises(ValueError, match="ascend"):
        run([Scripted(1)], ListPath(updates, 10_000))


def test_one_agents_bursts_arrive_in_send_order_even_when_jitter_exceeds_their_gap():
    # Arrange: an order on one update and its cancel on the next, 1 us apart,
    # with 6 us of jitter per burst. A real session is FIFO, so the cancel must
    # never overtake (or tie and be shuffled ahead of) the order it targets.
    updates = [SignalUpdate(1_000, V0, is_jump=True), SignalUpdate(2_000, V0, is_jump=True)]

    def react(t, u, view):
        return [Order(11, SELL, V0, 5, LIMIT)] if u.t_ns == 1_000 else [Cancel(11)]

    for seed in range(50):
        agent = Scripted(1, signal=react)

        # Act
        res = run([agent], ListPath(updates, 1_000_000), seed=seed,
                  race_jitter_ns={1: 6_000})

        # Assert
        order, cancel = (r.arrivals[0] for r in res.races)
        assert cancel.arrival_ns > order.arrival_ns, seed
        assert cancel.outcome == "cancelled", seed
        assert res.snapshots[-1].best_ask == 0, seed


def test_an_order_the_book_refuses_after_admission_fails_the_run_loudly():
    # Arrange: the book centres a fixed price band on the first price it sees
    # (OrderBook::ensurePriceRange). An order far outside it is accepted at
    # admission, then refused with OutOfPriceRange when it reaches the book.
    # Logged as "rested", it would silently drop the agent from the book.
    updates = [SignalUpdate(1_000, V0), SignalUpdate(2_000, V0)]

    def react(t, u, view):
        price = V0 if u.t_ns == 1_000 else V0 + 10_000_000
        return [Order(11 + u.t_ns, SELL, price, 5, LIMIT)]

    # Act / Assert
    with pytest.raises(RuntimeError, match="OutOfPriceRange"):
        run([Scripted(1, signal=react)], ListPath(updates, 1_000_000))
