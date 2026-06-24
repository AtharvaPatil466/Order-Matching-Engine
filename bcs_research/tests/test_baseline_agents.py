"""Unit tests for the Phase 1 baseline agents.

These exercise BaselineMarketMaker and NoiseTrader logic directly with
lightweight stubs — no compiled engine roundtrip needed. We still import
bcs_engine for the enum/precision constants the agents reference.
"""
import sys
import pathlib

_R = pathlib.Path(__file__).resolve().parents[1]
for _d in ("build", "agents", "simulation"):
    sys.path.insert(0, str(_R / _d))

import bcs_engine as be  # noqa: E402
from market_maker import BaselineMarketMaker  # noqa: E402
from noise_trader import NoiseTrader  # noqa: E402

V = 1_000_000
HALF_SPREAD = 100


class StubMarket:
    """Read-only book view stub with configurable touch prices."""

    def __init__(self, best_bid=0, best_ask=0, mid=0):
        self._best_bid = best_bid
        self._best_ask = best_ask
        self._mid = mid

    def best_bid(self):
        return self._best_bid

    def best_ask(self):
        return self._best_ask

    def mid(self):
        return self._mid


class StubFundamental:
    """Fundamental-value stub returning a fixed v regardless of latency."""

    def __init__(self, v):
        self._v = v

    def observe(self, t, latency_us=0):
        return self._v


class FakeTrade:
    """Minimal trade object exposing the fields the agents read."""

    def __init__(self, price, quantity):
        self.price = price
        self.quantity = quantity


# --- 1. MM first tick: two-sided quote at +/- half_spread -------------------

def test_mm_first_tick_quotes_both_sides():
    # Arrange
    mm = BaselineMarketMaker(participant_id=1, half_spread=HALF_SPREAD,
                             quote_qty=50)
    market = StubMarket()
    fund = StubFundamental(V)

    # Act
    actions = mm.act(0, market, fund)

    # Assert
    assert len(actions) == 2
    assert all(a.kind == "order" for a in actions)
    sides = {a.side for a in actions}
    assert sides == {be.Side.Buy, be.Side.Sell}

    bid = next(a for a in actions if a.side == be.Side.Buy)
    ask = next(a for a in actions if a.side == be.Side.Sell)
    assert bid.price == V - HALF_SPREAD
    assert ask.price == V + HALF_SPREAD
    assert bid.price < ask.price


# --- 2. MM requote includes cancels of the prior quotes ---------------------

def test_mm_requote_cancels_previous_quotes():
    # Arrange
    mm = BaselineMarketMaker(participant_id=1, half_spread=HALF_SPREAD)
    market = StubMarket()
    fund = StubFundamental(V)

    first = mm.act(0, market, fund)
    first_bid_oid = next(a.order_id for a in first if a.side == be.Side.Buy)
    first_ask_oid = next(a.order_id for a in first if a.side == be.Side.Sell)

    # Act
    second = mm.act(1, market, fund)

    # Assert
    assert len(second) == 4
    cancels = [a for a in second if a.kind == "cancel"]
    orders = [a for a in second if a.kind == "order"]
    assert len(cancels) == 2
    assert len(orders) == 2
    cancelled_ids = {a.cancel_order_id for a in cancels}
    assert cancelled_ids == {first_bid_oid, first_ask_oid}


# --- 3. MM inventory skew shifts both quotes down when long -----------------

def test_mm_inventory_skew_shifts_quotes_down():
    # Arrange: zero-inventory baseline
    flat = BaselineMarketMaker(participant_id=1, half_spread=HALF_SPREAD,
                               inventory_skew=1.0)
    market = StubMarket()
    fund = StubFundamental(V)
    flat_actions = flat.act(0, market, fund)
    flat_bid = next(a.price for a in flat_actions if a.side == be.Side.Buy)
    flat_ask = next(a.price for a in flat_actions if a.side == be.Side.Sell)

    # long inventory
    skewed = BaselineMarketMaker(participant_id=2, half_spread=HALF_SPREAD,
                                 inventory_skew=1.0)
    skewed.inventory = 100
    skewed_actions = skewed.act(0, market, fund)
    skewed_bid = next(a.price for a in skewed_actions if a.side == be.Side.Buy)
    skewed_ask = next(a.price for a in skewed_actions if a.side == be.Side.Sell)

    # Assert: a long position pushes both quotes down to shed inventory
    assert skewed_bid < flat_bid
    assert skewed_ask < flat_ask


# --- 4. MM PnL accounting on a sell fill ------------------------------------

def test_mm_pnl_accounting_on_sell_fill():
    # Arrange
    mm = BaselineMarketMaker(participant_id=1, half_spread=HALF_SPREAD)
    trade = FakeTrade(price=V + HALF_SPREAD, quantity=50)

    # Act: MM sells (is_buyer=False) — it is the seller of this trade
    mm.on_fill(trade, is_buyer=False)

    # Assert
    assert mm.inventory == -50
    assert mm.cash > 0
    expected = mm.cash + (-50) * V / be.PRICE_PRECISION
    assert mm.pnl(V) == expected


# --- 5. NoiseTrader crosses on a two-sided book, abstains when empty --------

def test_noise_trader_crosses_two_sided_book():
    # Arrange
    nt = NoiseTrader(participant_id=7, lambda_per_tick=1.0, qty=10, seed=42)
    market = StubMarket(best_bid=999_900, best_ask=1_000_100)
    fund = StubFundamental(V)

    # Act
    actions = nt.act(0, market, fund)

    # Assert
    assert len(actions) == 1
    a = actions[0]
    assert a.order_type == be.OrderType.IOC
    assert a.quantity == 10
    if a.side == be.Side.Buy:
        assert a.price == 1_000_100  # crosses at the ask
    else:
        assert a.side == be.Side.Sell
        assert a.price == 999_900  # crosses at the bid


def test_noise_trader_abstains_on_empty_book():
    # Arrange
    nt = NoiseTrader(participant_id=7, lambda_per_tick=1.0, qty=10, seed=42)
    empty = StubMarket(best_bid=0, best_ask=0)
    fund = StubFundamental(V)

    # Act
    actions = nt.act(0, empty, fund)

    # Assert
    assert actions == []


# --- 6. NoiseTrader PnL points negative after buying at the ask -------------

def test_noise_trader_pnl_negative_after_buy_at_ask():
    # Arrange
    nt = NoiseTrader(participant_id=7, lambda_per_tick=1.0, qty=10, seed=1)
    ask = V + HALF_SPREAD
    mid = V
    trade = FakeTrade(price=ask, quantity=10)

    # Act: buy at the ask, then mark at mid (strictly below the ask)
    nt.on_fill(trade, is_buyer=True)

    # Assert: paying the spread leaves an immediate negative mark-to-mid
    assert nt.inventory == 10
    assert nt.pnl(mid) < 0
