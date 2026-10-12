"""Batch-auction semantics: the tick model's defects, the engine's clearing facts,
and the event-model EventBatchScheduler that fixes the former on top of the latter.

Part A pins what is wrong (and what is not) with how the tick model's
BatchAuctionScheduler handles IOCs. Those tests document behaviour; the tick
scheduler is deliberately left as it is so published results keep reproducing.
Part B pins how the engine's uncross() prices a batch. Part C tests the event
scheduler, above all BCS's property: inside one interval submission time does
not matter, so a cancel that arrives any time before the boundary beats a
sniper that arrived earlier.
"""
import sys
from pathlib import Path

_R = Path(__file__).resolve().parents[1]
for _d in ("build", "simulation", "agents", "metrics"):
    if str(_R / _d) not in sys.path:
        sys.path.insert(0, str(_R / _d))

import pytest  # noqa: E402

import bcs_engine as be  # noqa: E402
from batch_auction import Action, BatchAuctionScheduler, discover_uncross_price  # noqa: E402
from contracts import Cancel, Order, SignalUpdate, Wakeup  # noqa: E402
from event_batch import EventBatchScheduler  # noqa: E402
from event_maker import EventMaker  # noqa: E402
from event_scheduler import EventScheduler  # noqa: E402
from event_sniper import EventSniper  # noqa: E402

BUY, SELL = be.Side.Buy, be.Side.Sell
LIMIT, IOC = be.OrderType.Limit, be.OrderType.IOC
GTC = be.TimeInForce.GTC
NOT_FOUND = be.RejectReason.OrderNotFound


def _harness(auction=False):
    h = be.EngineHarness()
    h.add_symbol(1)
    h.start(disable_circuit_breaker=True)
    if auction:
        h.set_trading_state(1, be.TradingState.AuctionOpen)
    return h


# --- Part A: the tick model's IOC-in-batch handling ---------------------------

def _tick(interval_ticks=5):
    h = _harness()
    return BatchAuctionScheduler(h, 1, dt_us=1000, batch_interval_ticks=interval_ticks), h


def _act(pid, oid, side, price, qty, otype, at):
    return Action(participant_id=pid, submit_at=at, order_id=oid, side=side, price=price,
                  quantity=qty, order_type=otype)


def test_tick_defect_ioc_loses_time_priority_to_a_later_limit():
    # The IOC arrives at t=0, the competing limit at t=2000, both bid 100 for
    # one 10-lot ask. The IOC is held back and submitted after every limit,
    # so the later limit fills: neither price-time nor any batch rule.
    sched, h = _tick()
    sched._enqueue(_act(3, 1, SELL, 100, 10, LIMIT, 0))
    sched._enqueue(_act(1, 2, BUY, 100, 10, IOC, 0))
    sched._submit_due(0)
    sched._enqueue(_act(2, 3, BUY, 100, 10, LIMIT, 2000))
    sched._submit_due(2000)
    sched._uncross(4000)
    assert [tr["buyer_id"] for tr in sched.trades] == [2]
    h.stop()


@pytest.mark.parametrize("first_enqueued", [1, 2])
def test_tick_defect_same_price_iocs_still_race_inside_the_batch(first_enqueued):
    # Same price, same window: the earlier submit_at always fills, so the
    # latency race survives the batch; an exact tie goes to whoever was
    # enqueued first (registration order), never to a random draw.
    for at, winner in (({1: 50, 2: 10}, 2), ({1: 10, 2: 10}, first_enqueued)):
        sched, h = _tick()
        sched._enqueue(_act(3, 1, SELL, 100, 10, LIMIT, 0))
        order = (first_enqueued, 3 - first_enqueued)
        for pid in order:
            sched._enqueue(_act(pid, 10 + pid, BUY, 100, 10, IOC, at[pid]))
        sched._submit_due(1000)
        sched._uncross(1000)
        assert [tr["buyer_id"] for tr in sched.trades] == [winner]
        h.stop()


def test_tick_ioc_remainder_never_rests_after_the_uncross():
    # Not a defect: the flushed GTC limit is cancelled before anyone can see it.
    sched, h = _tick()
    sched._enqueue(_act(3, 1, SELL, 100, 3, LIMIT, 0))
    sched._enqueue(_act(1, 2, BUY, 100, 10, IOC, 0))
    sched._submit_due(0)
    sched._uncross(0)
    assert sum(tr["quantity"] for tr in sched.trades) == 3
    assert h.best_bid(1) == 0
    assert h.submit_cancel(1, 2)[1] == NOT_FOUND
    h.stop()


def test_tick_cancel_mid_window_beats_an_earlier_ioc():
    # Not a defect: this is BCS's property, and the tick batch has it.
    sched, h = _tick()
    sched._enqueue(_act(3, 1, SELL, 100, 10, LIMIT, 0))
    sched._enqueue(_act(1, 2, BUY, 100, 10, IOC, 1000))
    sched._submit_due(1000)
    sched._enqueue(Action(participant_id=3, submit_at=3000, kind="cancel", cancel_order_id=1))
    sched._submit_due(3000)
    sched._uncross(4000)
    assert sched.trades == []
    h.stop()


def test_tick_batch_sniper_ioc_outbids_the_makers_bid_for_noise_flow():
    # The interval-1 mechanism (paper/batch_interval1_note.md). The maker has
    # requoted up (bid 95, ask 107). A sniper's buy IOC priced at the stale
    # ask (105) is a buy limit a full spread above the maker's bid, so in the
    # batch it, not the maker, buys the noise trader's sell, at 105. In
    # continuous trading the IOCs never meet and the noise sell hits the bid.
    sched, h = _tick()
    for a in (_act(1, 1, BUY, 95, 10, LIMIT, 0), _act(1, 2, SELL, 107, 10, LIMIT, 0),
              _act(200, 3, BUY, 105, 10, IOC, 0), _act(100, 4, SELL, 95, 10, IOC, 0)):
        sched._enqueue(a)
    sched._submit_due(0)
    sched._uncross(0)
    assert [(tr["buyer_id"], tr["seller_id"], tr["price"]) for tr in sched.trades] == [(200, 100, 105)]
    h.stop()


# --- Part B: how the engine's uncross prices a batch --------------------------

def _uncross_prices(orders):
    h = _harness(auction=True)
    for oid, (pid, side, price, qty) in enumerate(orders, 1):
        h.submit_order(1, oid, pid, side, price, qty, LIMIT, GTC)
    h.uncross(1)
    trades = h.drain_trades(1)
    h.stop()
    return trades


def test_engine_clears_a_stale_ask_against_two_bidders_at_the_stale_price():
    # THEORY's finding (PREREGISTRATION P9): one stale ask at A, two snipers
    # bidding V' > A. Every price in [A, V'] ties on volume and imbalance; the
    # engine takes the one nearest its reference (here A, the first order),
    # so the snipers keep V' - A. BCS's Bertrand outcome is V'; the
    # reference-free cascade (pressure: buy surplus clears high) gives it.
    A, VP = 1000, 1010
    trades = _uncross_prices([(1, SELL, A, 10), (2, BUY, VP, 10), (3, BUY, VP, 10)])
    assert {t.price for t in trades} == {A}
    assert discover_uncross_price([(VP, 20)], [(A, 10)], reference=None)[:2] == (VP, 10)


def test_engine_reference_is_the_sessions_first_order_not_the_last_print():
    # The reference is seeded by the first limit order and uncross() never
    # updates it, so after a print at 1100 the same tie still clears at 1000,
    # the price nearest the session's first order (900).
    h = _harness(auction=True)
    for oid, (pid, side, price, qty) in enumerate(
            [(9, BUY, 900, 1), (8, SELL, 1100, 5), (7, BUY, 1100, 5)], 1):
        h.submit_order(1, oid, pid, side, price, qty, LIMIT, GTC)
    h.uncross(1)
    assert {t.price for t in h.drain_trades(1)} == {1100}
    for oid, (pid, side, price) in enumerate([(1, SELL, 1000), (2, BUY, 1010), (3, BUY, 1010)], 10):
        h.submit_order(1, oid, pid, side, price, 10, LIMIT, GTC)
    h.uncross(1)
    assert {t.price for t in h.drain_trades(1)} == {1000}
    h.stop()


def test_engine_stamps_every_uncross_trade_buyer_aggressor():
    for orders in ([(1, BUY, 1000, 5), (2, SELL, 1000, 5)], [(1, SELL, 1000, 5), (2, BUY, 1000, 5)]):
        assert [t.aggressor_side for t in _uncross_prices(orders)] == [BUY]


# --- Part C: EventBatchScheduler ----------------------------------------------

TAU = 1_000_000          # 1 ms
V0 = 100 * be.PRICE_PRECISION


class _Script:
    """Sends a fixed plan {t_ns: [Order|Cancel]}; each arrives t_ns + order_latency_ns."""

    def __init__(self, pid, plan, order_latency_ns=0):
        self.participant_id = pid
        self.md_latency_ns = 0
        self.order_latency_ns = order_latency_ns
        self.plan = plan
        self.fills = []
        self.seen = []       # (t, best_bid, best_ask) at each wakeup

    def on_start(self, t_ns, view):
        return [Wakeup(t, "go") for t in sorted(self.plan)]

    def on_wakeup(self, t_ns, tag, view):
        self.seen.append((t_ns, view.best_bid(), view.best_ask()))
        return list(self.plan[t_ns])

    def on_signal(self, t_ns, update, view):
        return []

    def on_fill(self, t_ns, fill, view):
        self.fills.append(fill)
        return []


class _Path:
    def __init__(self, duration_ns, updates=()):
        self.duration_ns = duration_ns
        self._updates = list(updates)

    def value_at(self, t_ns):
        return V0

    def updates(self):
        return self._updates


def _batch(agents, duration=5 * TAU, seed=0, path=None, **kw):
    sched = EventBatchScheduler(_harness(), 1, TAU, seed=seed, **kw)
    return sched, sched.run(agents, path or _Path(duration))


def _continuous(agents, duration=5 * TAU, seed=0):
    return EventScheduler(_harness(), 1, seed=seed).run(agents, _Path(duration))


def _maker_ask_and_cancel(cancel_at):
    # Ask posted in batch 0, rests after the first boundary; cancelled at cancel_at.
    return _Script(1, {0: [Order(11, SELL, 1000, 5, LIMIT)], cancel_at: [Cancel(11)]})


@pytest.mark.parametrize("cancel_at", [TAU + 101, TAU + TAU // 2, 2 * TAU - 1])
def test_a_cancel_anywhere_before_the_boundary_beats_an_earlier_sniper(cancel_at):
    def agents():
        return [_maker_ask_and_cancel(cancel_at), _Script(2, {TAU + 100: [Order(21, BUY, 1000, 5, IOC)]})]
    _, batch = _batch(agents())
    assert batch.trades == []
    # The same arrivals on a continuous book: the earlier IOC wins the race.
    cont = _continuous(agents())
    assert [(t.buyer_id, t.seller_id) for t in cont.trades] == [(2, 1)]


def test_a_cancel_landing_on_the_boundary_belongs_to_the_next_batch():
    _, res = _batch([_maker_ask_and_cancel(2 * TAU),
                     _Script(2, {TAU + 100: [Order(21, BUY, 1000, 5, IOC)]})])
    assert [(t.t_ns, t.buyer_id, t.quantity) for t in res.trades] == [(2 * TAU, 2, 5)]


def test_ioc_fills_only_in_its_own_auction_and_never_rests():
    sniper = _Script(2, {TAU + 100: [Order(21, BUY, 1000, 9, IOC)]})
    seller = _Script(1, {0: [Order(11, SELL, 1000, 4, LIMIT)], 3 * TAU: [Order(12, SELL, 1000, 4, LIMIT)]})
    sched, res = _batch([seller, sniper])
    assert [(t.t_ns, t.quantity) for t in res.trades] == [(2 * TAU, 4)]   # 5 left, cancelled
    assert sched._h.best_ask(1) == 1000 and sched._h.best_bid(1) == 0      # the later ask rests alone
    assert sched._h.submit_cancel(1, 21)[1] == NOT_FOUND


def test_agents_and_snapshots_see_the_book_only_as_of_the_last_boundary():
    maker = _Script(1, {0: [Order(11, BUY, 990, 5, LIMIT)], TAU + 10: [Cancel(11)]})
    watcher = _Script(3, {TAU // 2: [], TAU + 20: [], 2 * TAU: []})
    _, res = _batch([maker, watcher])
    # Invisible before the boundary, visible after it although cancelled since,
    # gone once the next boundary publishes.
    assert [s[1] for s in watcher.seen] == [0, 990, 0]
    assert res.snapshots[0].best_bid == 0


def _two_snipers_one_ask(seed, rationing):
    snipers = [_Script(pid, {TAU + 10 * pid: [Order(pid * 10, BUY, 1000, 5, IOC)]}) for pid in (2, 3)]
    _, res = _batch([_Script(1, {0: [Order(11, SELL, 1000, 5, LIMIT)]})] + snipers,
                    seed=seed, rationing=rationing)
    return res.trades


def test_time_rationing_fills_the_earlier_arrival_every_time():
    assert {_two_snipers_one_ask(s, "time")[0].buyer_id for s in range(20)} == {2}


def test_random_rationing_is_seeded_and_changes_only_who_fills():
    draws = [_two_snipers_one_ask(s, "random") for s in range(20)]
    assert {d[0].buyer_id for d in draws} == {2, 3}
    assert {(d[0].price, d[0].quantity, d[0].seller_id) for d in draws} == {(1000, 5, 1)}
    assert _two_snipers_one_ask(7, "random") == draws[7]


@pytest.mark.parametrize("rule, price", [("engine", 1000), ("pressure", 1009)])
def test_price_rule_engine_keeps_the_stale_price_pressure_gives_bertrand(rule, price):
    # A stale ask at 1000 rests (and seeds the reference); two price-competitive
    # snipers bid V' - 1 = 1009. Same fills under both rules, different price.
    snipers = [_Script(pid, {TAU + pid: [Order(pid * 10, BUY, 1009, 5, IOC)]}) for pid in (2, 3)]
    sched, res = _batch([_Script(1, {0: [Order(11, SELL, 1000, 5, LIMIT)]})] + snipers, price_rule=rule)
    assert [(t.price, t.quantity, t.buyer_id, t.seller_id) for t in res.trades] == [(price, 5, 2, 1)]
    assert sched.repriced == (rule == "pressure") and sched.reprice_refused == 0
    assert snipers[0].fills[0].price == price


def test_the_later_arrival_is_the_taker():
    seller = _Script(1, {0: [Order(11, SELL, 1000, 5, LIMIT)], TAU + 50: [Order(12, SELL, 1000, 5, LIMIT)]})
    buyer = _Script(2, {TAU + 10: [Order(21, BUY, 1000, 10, LIMIT)]})
    _, res = _batch([seller, buyer])
    # vs the resting ask: the buyer is the taker; vs the ask that came after it: the seller is.
    assert [t.aggressor_buy for t in res.trades] == [True, False]


def _jump_run(seed, **kw):
    ups = [SignalUpdate(0, V0), SignalUpdate(2 * TAU + 300, V0 + 500, True)]
    agents = [EventMaker(1, 200, 5, md_latency_ns=50_000, order_latency_ns=50_000),
              EventSniper(2, 5, md_latency_ns=5_000, order_latency_ns=5_000),
              EventSniper(3, 5, md_latency_ns=5_000, order_latency_ns=5_000)]
    return _batch(agents, path=_Path(6 * TAU, ups), seed=seed, **kw)[1]


def test_race_log_carries_each_order_s_auction_outcome():
    race = _jump_run(0).races[0]
    kinds = {(a.participant_id, a.kind, a.outcome) for a in race.arrivals}
    # The maker's cancel and both snipers' IOCs land in the same interval:
    # the cancel wins and both IOCs miss, whatever their arrival order.
    assert (1, "cancel", "cancelled") in kinds
    assert {(2, "ioc", "missed"), (3, "ioc", "missed")} <= kinds
    assert all(a.outcome != "pending" for a in race.arrivals)


def test_same_seed_reproduces_a_batch_run_exactly():
    a, b = _jump_run(3, rationing="random"), _jump_run(3, rationing="random")
    assert a.trades == b.trades and a.races == b.races


def test_rejects_bad_configuration():
    for kw in ({"interval_ns": 0}, {"interval_ns": TAU, "rationing": "pro_rata"},
               {"interval_ns": TAU, "price_rule": "walras"}):
        with pytest.raises(ValueError):
            EventBatchScheduler(_harness(), 1, **kw)


# --- exp11's pieces -----------------------------------------------------------

class _Book:
    def __init__(self, bid, ask):
        self._bid, self._ask = bid, ask

    def best_bid(self):
        return self._bid

    def best_ask(self):
        return self._ask


def test_exp11_competitive_sniper_bids_its_value_less_one_tick():
    sys.path.insert(0, str(_R / "experiments"))
    import exp11_batch_semantics as E
    s = E.CompetitiveSniper(2, 1)
    [buy] = s.on_signal(0, SignalUpdate(0, 1010.4, True), _Book(990, 1000))
    [sell] = s.on_signal(0, SignalUpdate(0, 979.6, True), _Book(990, 1000))
    assert (buy.side, buy.price, sell.side, sell.price) == (BUY, 1010, SELL, 980)
    assert s.on_signal(0, SignalUpdate(0, 995.0, True), _Book(990, 1000)) == []


@pytest.mark.parametrize("arm", ["continuous", (1_000_000, "random", "pressure")])
def test_exp11_short_run_closes_the_books_under_both_fee_schedules(arm):
    sys.path.insert(0, str(_R / "experiments"))
    import exp11_batch_semantics as E
    row = E.run_once({**E.CFG, "duration_s": 2}, 1, arm, "competitive")
    assert row["n_trades"] > 0
    for fees in ("zero", "taker_heavy"):
        total = (row[f"maker_edge_{fees}"] + row[f"sniper_edge_{fees}"] - row[f"noise_cost_{fees}"]
                 + row[f"exchange_{fees}"])
        assert abs(total) < 1e-9 and abs(row[f"residual_{fees}"]) < 1e-9


def test_an_out_of_band_order_flushed_at_the_boundary_fails_the_run_loudly():
    # Arrange: batch 0 centres the book's fixed price band on V0; batch 1 then
    # flushes an ask far outside it, which the book refuses after admission.
    far = V0 + 10_000_000
    agent = _Script(1, {0: [Order(11, SELL, V0, 5, LIMIT)],
                        TAU + 10: [Order(12, SELL, far, 5, LIMIT)]})

    # Act / Assert
    with pytest.raises(RuntimeError, match="OutOfPriceRange"):
        _batch([agent])


def test_exp11_confirmatory_smoke_writes_p9_verdicts(tmp_path):
    import exp11_batch_semantics as E
    r = E.main(["--confirmatory", "--seeds", "2", "--horizon-s", "5", "--workers", "1",
                "--out-dir", str(tmp_path)])
    assert r["status"].startswith("smoke")
    assert {"H9a", "H9b", "H9c", "H9d"} <= set(r["p9"])
    assert len(r["arms"]) == len(E.confirm_arms())
    # stale-price snipers clear at the stale quote under either price rule
    assert r["p9"]["H9b"]["stale_identical_across_price_rules"]


def test_exp11_footnote34_bound_scales_as_one_over_tau():
    import exp11_batch_semantics as E
    assert E.footnote34_bound(1_000_000) == pytest.approx(10 * E.footnote34_bound(10_000_000))
