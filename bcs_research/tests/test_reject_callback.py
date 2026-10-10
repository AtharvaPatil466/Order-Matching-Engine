"""The optional on_reject callback (contracts.py) and the post-only maker built on it.

Why it matters: two equally fast plain-limit makers cross each other's stale
quotes after every move, which is not how real makers behave (they use
post-only). A post-only quote the engine refuses never rests, so a maker that
cannot learn of the refusal keeps a phantom order in the book it believes it
has. These tests pin when and with what the scheduler reports a refusal, on
the continuous and the batch clock, and that a post-only StrategicMaker never
trades against another maker while its believed book stays the engine's.
"""
import sys
from collections import Counter
from pathlib import Path

_R = Path(__file__).resolve().parents[1]
for _d in ("build", "simulation", "agents", "tests"):
    if str(_R / _d) not in sys.path:
        sys.path.insert(0, str(_R / _d))

import pytest  # noqa: E402

import bcs_engine as be  # noqa: E402
from contracts import Cancel, Order, SignalUpdate, Wakeup  # noqa: E402
from event_batch import EventBatchScheduler  # noqa: E402
from event_sniper import EventSniper  # noqa: E402
from strategic_maker import MakerPolicy, StrategicMaker  # noqa: E402
from test_event_scheduler import SYM, V0, ListPath, Scripted  # noqa: E402
from test_race import PoissonTaker, market_path  # noqa: E402
from test_strategic_maker import believed_book, engine_book, run_on_engine  # noqa: E402

BUY, SELL = be.Side.Buy, be.Side.Sell
LIMIT, IOC, POST_ONLY = be.OrderType.Limit, be.OrderType.IOC, be.OrderType.PostOnly
QTY = 10


class Hears(Scripted):
    """A Scripted agent that also defines on_reject and records each call."""

    def on_reject(self, t_ns, order_id, reason_name, view):
        return self._call("reject", t_ns, (order_id, reason_name), view)


def _resting_ask(price=V0):
    return Scripted(1, start=lambda t, _, v: [Order(11, SELL, price, QTY, LIMIT)])


def test_post_only_reject_reaches_the_sender_at_arrival_plus_md_latency():
    # Arrange: an ask rests at V0. On a jump the sender posts a post-only bid
    # at V0 (crosses: refused) and one at V0 - 1 (rests). Its arrival is
    # jittered, so only the race log knows the exact engine time.
    md = 3_000
    sender = Hears(2, md=md, order=1_000,
                   signal=lambda t, u, v: [Order(21, BUY, V0, QTY, POST_ONLY),
                                           Order(22, BUY, V0 - 1, QTY, POST_ONLY)])
    path = ListPath([SignalUpdate(10_000, V0, is_jump=True)], 100_000)

    # Act
    res, h = run_on_engine([_resting_ask(), sender], path, seed=4, race_jitter_ns={2: 5_000})

    # Assert
    race, = res.races
    refused, rested = race.arrivals
    assert (refused.order_id, refused.outcome) == (21, "PostOnlyWouldCross")
    assert rested.outcome == "rested"
    assert [c for c in sender.calls if c[0] == "reject"] == [
        ("reject", refused.arrival_ns + md, (21, "PostOnlyWouldCross"))]
    assert engine_book(h) == {(BUY, V0 - 1): QTY, (SELL, V0): QTY}


def test_a_too_late_cancel_is_reported_as_order_not_found():
    # Arrange: the sender cancels an order id the engine never had.
    sender = Hears(2, md=700, order=300, start=lambda t, _, v: [Wakeup(5_000)],
                   wakeup=lambda t, tag, v: [Cancel(999)])

    # Act
    run_on_engine([sender], ListPath([], 50_000))

    # Assert: sent at 5 us, arrives at 5.3 us, learned at 6 us.
    assert [c for c in sender.calls if c[0] == "reject"] == [("reject", 6_000, (999, "OrderNotFound"))]


def test_agents_without_on_reject_get_no_reject_event():
    # Arrange: the same refused post-only bid, from an agent with and without the callback.
    def sender(cls):
        return cls(2, md=100, order=100, start=lambda t, _, v: [Wakeup(1_000)],
                   wakeup=lambda t, tag, v: [Order(21, BUY, V0, QTY, POST_ONLY)])

    # Act
    deaf, _ = run_on_engine([_resting_ask(), sender(Scripted)], ListPath([], 50_000))
    hears, _ = run_on_engine([_resting_ask(), sender(Hears)], ListPath([], 50_000))

    # Assert: exactly one extra event, the reject, and nothing else differs.
    assert hears.n_events == deaf.n_events + 1
    assert hears.trades == deaf.trades == []


def test_batch_reports_a_refused_order_from_its_boundary_and_a_refused_cancel_from_arrival():
    # Arrange: tau = 1 ms. The ask arrives in batch 0 and rests from the first
    # boundary. In batch 1 the sender's crossing post-only bid is held until
    # the boundary at 2 ms, and its cancel of an unknown id lands at 1.2 ms.
    tau, md = 1_000_000, 4_000
    plan = {1_200_000: [Cancel(999)], 1_500_000: [Order(21, BUY, V0, QTY, POST_ONLY)]}
    sender = Hears(2, md=md, start=lambda t, _, v: [Wakeup(at) for at in plan],
                   wakeup=lambda t, tag, v: plan[t])
    h = be.EngineHarness()
    h.add_symbol(SYM)
    h.start(disable_circuit_breaker=True)

    # Act
    EventBatchScheduler(h, SYM, tau).run([_resting_ask(), sender], ListPath([], 5 * tau))

    # Assert
    assert [c for c in sender.calls if c[0] == "reject"] == [
        ("reject", 1_200_000 + md, (999, "OrderNotFound")),
        ("reject", 2 * tau + md, (21, "PostOnlyWouldCross"))]


# --- the post-only StrategicMaker ------------------------------------------

M1, M2, SNIPER, TAKER = 1, 2, 3, 4
HALF = 200


@pytest.mark.parametrize("requote_after_fill,book", [
    (False, {(SELL, V0 + 600): QTY}),
    (True, {(BUY, V0 + 200): QTY, (SELL, V0 + 600): QTY}),
], ids=["waits", "requotes"])
def test_a_refused_quote_is_dropped_and_the_requote_policy_decides_what_follows(
        requote_after_fill, book):
    # Arrange: a rival ask rests at V0 + 100. The maker centres on V0 + 400, so
    # its bid at V0 + 200 crosses it: sent at 1 us, refused at 2 us, learned at
    # 3 us. The rival pulls its ask at 2.5 us, so a requote at 3 us can rest.
    rival = Scripted(9, start=lambda t, _, v: [Order(91, SELL, V0 + 100, QTY, LIMIT),
                                               Wakeup(2_500)],
                     wakeup=lambda t, tag, v: [Cancel(91)])
    policy = MakerPolicy(half_spread=HALF, qty=QTY, post_only=True,
                         requote_after_fill=requote_after_fill)
    maker = StrategicMaker(M1, policy, md_latency_ns=1_000, order_latency_ns=1_000)

    # Act
    _, h = run_on_engine([rival, maker], ListPath([SignalUpdate(0, V0 + 400)], 20_000))

    # Assert
    assert maker.rejects == 1
    assert believed_book(maker) == engine_book(h) == book


def _two_makers(post_only, seed=5):
    """Two equally fast makers, a faster sniper and a slow Poisson taker, 20 s."""
    policy = MakerPolicy(half_spread=HALF, qty=QTY, post_only=post_only)
    makers = [StrategicMaker(pid, policy, md_latency_ns=50_000, order_latency_ns=50_000)
              for pid in (M1, M2)]
    agents = makers + [
        EventSniper(SNIPER, QTY, threshold=HALF, md_latency_ns=5_000, order_latency_ns=5_000),
        PoissonTaker(TAKER, 16.0, 2, seed, md_latency_ns=1_000_000, order_latency_ns=1_000_000)]
    res, h = run_on_engine(agents, market_path(20 * 10**9, seed), seed=seed)
    return res, h, makers


def test_post_only_makers_never_trade_with_each_other_and_believe_the_engine_book():
    # Act
    plain, _, _ = _two_makers(post_only=False)
    res, h, makers = _two_makers(post_only=True)

    # Assert: plain limits cross each other's stale quotes; post-only never does.
    def maker_on_maker(r):
        return sum({tr.buyer_id, tr.seller_id} == {M1, M2} for tr in r.trades)

    assert maker_on_maker(plain) > 10
    assert maker_on_maker(res) == 0
    # The run refused quotes, and still traded with the sniper and the taker.
    assert min(m.rejects for m in makers) > 5
    traded = Counter(p for tr in res.trades for p in (tr.buyer_id, tr.seller_id))
    assert traded[SNIPER] > 0 and traded[TAKER] > 0
    # Only the makers rest orders, so together they believe exactly the engine book.
    assert believed_book(makers[0]) or believed_book(makers[1])
    combined = Counter(believed_book(makers[0])) + Counter(believed_book(makers[1]))
    assert dict(combined) == engine_book(h)
