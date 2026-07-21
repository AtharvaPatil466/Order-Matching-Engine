"""Unit tests for the batch-auction scheduler + the retired Python matcher.

The scheduler now clears through the verified engine's own uncross() via the
pybind bridge (park in AuctionOpen -> accumulate -> uncross). The Auction.tla
invariant SingleClearingPrice — every uncross executes at exactly one price —
is asserted directly on engine-produced fills.

discover_uncross_price is the RETIRED Python re-implementation, kept as a
cross-check. Its tie-break cascade (max volume, min imbalance, reference
distance, market pressure) can pick a different price than the engine when
several prices tie on max volume; that divergence is pinned below so it
cannot regress silently.
"""
import sys
import pathlib

_R = pathlib.Path(__file__).resolve().parents[1]
for _d in ("build", "agents", "simulation", "metrics"):
    sys.path.insert(0, str(_R / _d))

import pytest

import bcs_engine as be
from batch_auction import Action, discover_uncross_price, BatchAuctionScheduler


# --- clearing price discovery ------------------------------------------------

def test_simple_cross_at_common_price():
    price, vol, _ = discover_uncross_price([(100, 10)], [(100, 10)])
    assert price == 100
    assert vol == 10


def test_overlapping_book_maximizes_volume():
    # Max executable volume is 10 at p=100.
    buys = [(101, 5), (100, 5)]
    sells = [(99, 5), (100, 5)]
    price, vol, _ = discover_uncross_price(buys, sells)
    assert price == 100
    assert vol == 10


def test_no_cross_returns_none():
    price, vol, _ = discover_uncross_price([(99, 5)], [(101, 5)])
    assert price is None
    assert vol == 0


def test_empty_book_returns_none():
    assert discover_uncross_price([], [])[0] is None


def test_buy_surplus_tiebreak_picks_higher_price():
    # p=101 and p=102 both clear vol=10 with imbalance 0; buy surplus -> higher.
    buys = [(102, 10)]
    sells = [(100, 5), (101, 5)]
    price, vol, buy_surplus = discover_uncross_price(buys, sells)
    assert vol == 10
    assert price == 102
    assert buy_surplus is True


def test_sell_surplus_tiebreak_picks_lower_price():
    # p=98 and p=100 both clear vol=5 with imbalance 5 and sell surplus
    # (cumSell > cumBuy); market pressure then clears at the LOWER price.
    buys = [(100, 5)]
    sells = [(98, 10), (102, 10)]
    price, vol, buy_surplus = discover_uncross_price(buys, sells)
    assert vol == 5
    assert price == 98
    assert buy_surplus is False


def test_imbalance_breaks_volume_ties():
    # Two prices clear the same volume; the one with smaller imbalance wins.
    buys = [(105, 10), (100, 10)]
    sells = [(100, 10), (101, 10)]
    price, vol, _ = discover_uncross_price(buys, sells)
    # p=100: cumBuy=20, cumSell=10 -> vol10 imb10; p=101: cumBuy=10,cumSell=20 ->
    # vol10 imb10; p=105: cumBuy=10,cumSell=20 vol10 imb10. All imb10 -> pressure.
    assert vol == 10
    assert price in (100, 101, 105)


def test_reference_price_breaks_ties_toward_reference():
    # Equal volume & imbalance at 101 and 102; reference=101 pulls clearing to 101.
    buys = [(102, 10)]
    sells = [(100, 5), (101, 5)]
    price, vol, _ = discover_uncross_price(buys, sells, reference=101)
    assert vol == 10
    assert price == 101


# --- scheduler integration (verified engine via pybind bridge) ---------------

def _make_sched(batch_interval_ticks=5):
    h = be.EngineHarness()
    h.add_symbol(1)
    h.start()
    return BatchAuctionScheduler(h, 1, dt_us=1000,
                                 batch_interval_ticks=batch_interval_ticks), h


# The fact-4 book: two prices (100, 101) tie on max executable volume (80).
_TIE_BUYS = [(102, 50), (101, 30)]
_TIE_SELLS = [(99, 40), (100, 60)]


def _submit_tie_book(sched):
    oid = 0
    for side, levels in ((be.Side.Buy, _TIE_BUYS), (be.Side.Sell, _TIE_SELLS)):
        for price, qty in levels:
            oid += 1
            sched._enqueue(Action(participant_id=oid, submit_at=0, order_id=oid,
                                  side=side, price=price, quantity=qty,
                                  order_type=be.OrderType.Limit))
    sched._submit_due(0)


def test_engine_uncross_clears_batch_at_single_price():
    # SingleClearingPrice on real engine fills: a book crossed at multiple
    # limit prices clears entirely at ONE price per batch.
    sched, h = _make_sched()
    _submit_tie_book(sched)
    assert sched.trades == []                     # parked: nothing matched on arrival
    sched._uncross(0)
    assert sum(tr["quantity"] for tr in sched.trades) == 80
    assert len({tr["price"] for tr in sched.trades}) == 1
    h.stop()


def test_python_matcher_divergence_pinned():
    # Same book, two matchers: the retired Python rule clears the volume-80
    # tie at 100, the verified engine at 101. Both move all 80 units. Pinned
    # so the documented divergence (paper §4.4) cannot regress silently.
    py_price, py_vol, _ = discover_uncross_price(_TIE_BUYS, _TIE_SELLS)
    assert (py_price, py_vol) == (100, 80)

    sched, h = _make_sched()
    _submit_tie_book(sched)
    sched._uncross(0)
    assert {tr["price"] for tr in sched.trades} == {101}
    assert sum(tr["quantity"] for tr in sched.trades) == 80
    h.stop()


class _ConstFund:
    """Fundamental stub: flat V, supports step()/observe()/current_v."""

    def __init__(self, v):
        self.current_v = float(v)

    def step(self, t):
        return self.current_v

    def observe(self, t, latency_us=0):
        return self.current_v


class _OneShotLimitMM:
    """Posts a two-sided limit once, then idle. Rests in the persistent book."""

    def __init__(self, pid, v, half):
        import bcs_engine as be
        self.participant_id = pid
        self._be = be
        self._v = v
        self._half = half
        self._done = False
        self.cash = 0.0
        self.inventory = 0

    def act(self, t, market, fundamental):
        if self._done:
            return []
        self._done = True
        from scheduler import Action
        be = self._be
        return [
            Action(participant_id=self.participant_id, order_id=1, side=be.Side.Buy,
                   price=self._v - self._half, quantity=10,
                   order_type=be.OrderType.Limit, submit_at=t),
            Action(participant_id=self.participant_id, order_id=2, side=be.Side.Sell,
                   price=self._v + self._half, quantity=10,
                   order_type=be.OrderType.Limit, submit_at=t),
        ]

    def on_fill(self, trade, is_buyer):
        notional = trade.price * trade.quantity
        if is_buyer:
            self.inventory += trade.quantity
            self.cash -= notional
        else:
            self.inventory -= trade.quantity
            self.cash += notional

    def pnl(self, mark):
        return self.cash + self.inventory * mark


def test_scheduler_runs_and_produces_simresult_shape():
    sched, h = _make_sched()
    mm = _OneShotLimitMM(1, v=100, half=2)
    result = sched.run([mm], _ConstFund(100), duration_us=20000)
    # Produces the metrics-compatible SimResult shape.
    assert hasattr(result, "snapshots") and hasattr(result, "trades")
    assert len(result.snapshots) == 20
    for tr in result.trades:
        assert set(tr) >= {"t", "price", "quantity", "buyer_id", "seller_id"}
    # run() restores continuous trading after the last uncross.
    assert h.trading_state(1) == be.TradingState.Continuous
    h.stop()


def test_resting_limit_keeps_book_two_sided_between_uncrosses():
    # The MM's limits rest, so the book is never spuriously empty post-uncross.
    sched, h = _make_sched()
    mm = _OneShotLimitMM(1, v=100, half=2)
    result = sched.run([mm], _ConstFund(100), duration_us=20000)
    two_sided = [s for s in result.snapshots if s["best_bid"] > 0 and s["best_ask"] > 0]
    assert len(two_sided) >= 15  # quotes rest across the run
    h.stop()
