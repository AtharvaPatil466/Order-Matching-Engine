"""Unit tests for BCSMarketMaker — adverse-selection feedback quoting.

These tests exercise the MM in isolation with lightweight stubs (no engine
matching). The real bcs_engine module is still imported by the MM for its
Side / OrderType enums and PRICE_PRECISION.
"""
import sys
import pathlib

_R = pathlib.Path(__file__).resolve().parents[1]
for _d in ("build", "agents", "simulation"):
    sys.path.insert(0, str(_R / _d))

import pytest

import bcs_engine as be
from bcs_market_maker import BCSMarketMaker


# --------------------------------------------------------------------------- #
# Lightweight stubs (no engine matching involved)
# --------------------------------------------------------------------------- #
class StubMarket:
    def __init__(self, best_bid, best_ask):
        self._best_bid = best_bid
        self._best_ask = best_ask

    def best_bid(self):
        return self._best_bid

    def best_ask(self):
        return self._best_ask

    def mid(self):
        return (self._best_bid + self._best_ask) / 2.0


class StubFundamental:
    def __init__(self, v, current_v=None):
        self._v = v
        # current_v is the TRUE value the MM reads ex-post in on_fill; defaults
        # to the observed value when not overridden.
        self.current_v = v if current_v is None else current_v

    def observe(self, t, latency_us=0):
        return self._v


class FakeTrade:
    def __init__(self, price, quantity):
        self.price = price
        self.quantity = quantity


def _order_actions(actions):
    return [a for a in actions if a.kind == "order"]


# --------------------------------------------------------------------------- #
# 1. init
# --------------------------------------------------------------------------- #
def test_init_starts_at_base_half_spread():
    # Arrange / Act
    mm = BCSMarketMaker(participant_id=1, base_half_spread=50)

    # Assert
    assert mm.current_half_spread == 50.0
    assert mm.current_spread_ticks == 2 * 50.0


# --------------------------------------------------------------------------- #
# 2. first-tick quotes
# --------------------------------------------------------------------------- #
def test_first_tick_emits_two_quotes_around_v():
    # Arrange
    mm = BCSMarketMaker(participant_id=1, base_half_spread=50, inventory_skew=0.0)
    market = StubMarket(best_bid=999_900, best_ask=1_000_100)
    fund = StubFundamental(1_000_000)

    # Act
    actions = mm.act(0, market, fund)
    orders = _order_actions(actions)

    # Assert: exactly two orders, one Buy one Sell (no prior quotes -> no cancels)
    assert len(orders) == 2
    buys = [a for a in orders if a.side == be.Side.Buy]
    sells = [a for a in orders if a.side == be.Side.Sell]
    assert len(buys) == 1
    assert len(sells) == 1
    # Decay toward base while already at base leaves it at base -> hs == 50.
    assert buys[0].price == 1_000_000 - 50
    assert sells[0].price == 1_000_000 + 50


# --------------------------------------------------------------------------- #
# 3. widen on fill
# --------------------------------------------------------------------------- #
def test_widen_on_single_adverse_fill():
    # Arrange: true V (999_000) is BELOW the MM's buy price (1_000_000), so the
    # MM bought above true value -> adverse (a stale-bid pickoff).
    mm = BCSMarketMaker(participant_id=1, base_half_spread=50,
                        adverse_sensitivity=0.2)
    mm._fundamental = StubFundamental(999_000)

    # Act
    mm.on_fill(FakeTrade(1_000_000, 50), is_buyer=True)

    # Assert
    assert mm.current_half_spread == pytest.approx(50 * 1.2)  # 60
    assert mm.pickoffs == 1
    assert mm.fills == 1
    assert mm.inventory == 50


def test_non_adverse_noise_fill_does_not_widen():
    # Arrange: true V (1_000_000) sits between the MM's quotes; a fill at the
    # ask (1_000_050) is a noise trade, NOT adverse. The spread must not widen.
    mm = BCSMarketMaker(participant_id=1, base_half_spread=50,
                        adverse_sensitivity=0.2)
    mm._fundamental = StubFundamental(1_000_000)

    # Act: MM sells at its ask to a noise buyer.
    mm.on_fill(FakeTrade(1_000_050, 50), is_buyer=False)

    # Assert: inventory/fills update but the spread stays at base, no pickoff.
    assert mm.current_half_spread == pytest.approx(50)
    assert mm.pickoffs == 0
    assert mm.fills == 1
    assert mm.inventory == -50


# --------------------------------------------------------------------------- #
# 4. cap
# --------------------------------------------------------------------------- #
def test_widening_is_capped():
    # Arrange: cap at base * 2 == 100, aggressive sensitivity
    mm = BCSMarketMaker(participant_id=1, base_half_spread=50,
                        adverse_sensitivity=1.0, spread_cap_mult=2)
    mm._fundamental = StubFundamental(999_000)  # adverse buys at 1_000_000

    # Act: fill many times
    for _ in range(10):
        mm.on_fill(FakeTrade(1_000_000, 50), is_buyer=True)

    # Assert: never exceeds the cap
    assert mm.current_half_spread == pytest.approx(100)
    assert mm.max_half_spread_seen == 100


# --------------------------------------------------------------------------- #
# 5. decay toward base
# --------------------------------------------------------------------------- #
def test_decay_pulls_half_spread_toward_base():
    # Arrange
    mm = BCSMarketMaker(participant_id=1, base_half_spread=50, spread_decay=0.1)
    market = StubMarket(best_bid=999_900, best_ask=1_000_100)
    fund = StubFundamental(1_000_000)
    mm.current_half_spread = 150.0

    # Act / Assert: each act decays once, strictly decreasing, no fills.
    prev = mm.current_half_spread
    first_post = None
    for i in range(10):
        mm.act(i, market, fund)
        if i == 0:
            first_post = mm.current_half_spread
        assert mm.current_half_spread < prev
        prev = mm.current_half_spread

    # First post-decay value: 150 * 0.9 + 50 * 0.1 == 140
    assert first_post == pytest.approx(150 * 0.9 + 50 * 0.1)
    assert mm.current_half_spread < 150
    assert mm.current_half_spread > 50


# --------------------------------------------------------------------------- #
# 6. feedback shows in quotes
# --------------------------------------------------------------------------- #
def test_feedback_widens_subsequent_quotes():
    # Arrange: no decay so the effect is clean.
    mm = BCSMarketMaker(participant_id=1, base_half_spread=50,
                        adverse_sensitivity=0.5, spread_decay=0.0)
    # observe() returns 1_000_000 (quotes around it) but TRUE V is 999_000, so
    # the buy at 1_000_000 is a stale-quote pickoff -> adverse.
    market = StubMarket(best_bid=999_900, best_ask=1_000_100)
    fund = StubFundamental(1_000_000, current_v=999_000)

    # Act: first quote
    first = _order_actions(mm.act(0, market, fund))
    first_bid = next(a.price for a in first if a.side == be.Side.Buy)
    first_ask = next(a.price for a in first if a.side == be.Side.Sell)
    first_spread = first_ask - first_bid

    # Picked off (adverse) -> half-spread 50 * 1.5 == 75
    mm.on_fill(FakeTrade(1_000_000, 50), is_buyer=True)

    second = _order_actions(mm.act(1, market, fund))
    second_bid = next(a.price for a in second if a.side == be.Side.Buy)
    second_ask = next(a.price for a in second if a.side == be.Side.Sell)
    second_spread = second_ask - second_bid

    # Assert
    assert first_spread == pytest.approx(100)
    assert mm.current_half_spread == pytest.approx(75)
    assert second_spread == pytest.approx(150)
    assert second_spread > first_spread


# --------------------------------------------------------------------------- #
# 7. PnL
# --------------------------------------------------------------------------- #
def test_pnl_after_sell_fill():
    # Arrange
    mm = BCSMarketMaker(participant_id=1, base_half_spread=50)
    v = 1_000_000

    # Act: a sell fill (MM is the seller) qty 50 at price v + 50
    mm.on_fill(FakeTrade(v + 50, 50), is_buyer=False)

    # Assert
    assert mm.inventory == -50
    assert mm.cash > 0
    expected = mm.cash + (-50) * v / be.PRICE_PRECISION
    assert mm.pnl(v) == pytest.approx(expected)


# --------------------------------------------------------------------------- #
# 8. Fix 2 — liquidity thins as the half-spread widens
# --------------------------------------------------------------------------- #
def test_thin_liquidity_shrinks_quote_size():
    # Arrange: no decay so current_half_spread stays where we set it.
    mm = BCSMarketMaker(participant_id=1, base_half_spread=50, quote_qty=50,
                        spread_decay=0.0, thin_liquidity=True, min_quote_qty=1)
    market = StubMarket(best_bid=999_900, best_ask=1_000_100)
    fund = StubFundamental(1_000_000)

    # At base spread the size is full.
    base_orders = _order_actions(mm.act(0, market, fund))
    assert all(a.quantity == 50 for a in base_orders)

    # Widen 5x (half-spread 250) -> size scales by 50/250 == 0.2 -> 10.
    mm.current_half_spread = 250.0
    wide_orders = _order_actions(mm.act(1, market, fund))
    assert all(a.quantity == 10 for a in wide_orders)

    # Extreme widening floors at min_quote_qty.
    mm.current_half_spread = 100_000.0
    floored = _order_actions(mm.act(2, market, fund))
    assert all(a.quantity == mm.min_quote_qty for a in floored)


def test_thin_liquidity_disabled_keeps_constant_size():
    mm = BCSMarketMaker(participant_id=1, base_half_spread=50, quote_qty=50,
                        spread_decay=0.0, thin_liquidity=False)
    market = StubMarket(best_bid=999_900, best_ask=1_000_100)
    fund = StubFundamental(1_000_000)
    mm.current_half_spread = 500.0  # widened, but thinning is off
    orders = _order_actions(mm.act(0, market, fund))
    assert all(a.quantity == 50 for a in orders)
