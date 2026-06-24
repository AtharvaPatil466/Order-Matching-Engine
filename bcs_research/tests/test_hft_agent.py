"""Tests for HFTAgent — latency-advantaged stale-quote sniper (BCS Step 5).

Uses lightweight stubs (no engine) for the market view and fundamental value;
the agent itself imports bcs_engine for the Side/OrderType enums and
PRICE_PRECISION, so the bootstrap below puts the compiled module on the path.
"""
import sys
import pathlib

_R = pathlib.Path(__file__).resolve().parents[1]
for _d in ("build", "agents", "simulation"):
    sys.path.insert(0, str(_R / _d))

import bcs_engine as be  # noqa: E402
from hft_agent import HFTAgent  # noqa: E402


class StubMarket:
    """Minimal read-only book view: fixed best bid/ask."""

    def __init__(self, best_bid, best_ask):
        self._bid = best_bid
        self._ask = best_ask

    def best_bid(self):
        return self._bid

    def best_ask(self):
        return self._ask


class StubFundamental:
    """Fixed-value fundamental: observe() always returns v regardless of t."""

    def __init__(self, v):
        self._v = v

    def observe(self, t, latency_us=0):
        return self._v


class FakeTrade:
    """Minimal trade with the fields on_fill reads."""

    def __init__(self, price, quantity):
        self.price = price
        self.quantity = quantity


def test_buy_snipe_picks_off_stale_low_ask():
    # Arrange: ask is stale-low — true value sits above it.
    market = StubMarket(best_bid=999_000, best_ask=1_000_000)
    fund = StubFundamental(v=1_000_500)
    hft = HFTAgent(participant_id=1, latency_us=0, race_noise_us=0)

    # Act
    actions = hft.act(0, market, fund)

    # Assert
    assert len(actions) == 1
    a = actions[0]
    assert a.side == be.Side.Buy
    assert a.order_type == be.OrderType.IOC
    assert a.price == 1_000_000          # lifts the stale ask
    assert a.quantity == hft.qty
    assert a.submit_at == 0              # no latency, no race noise
    assert hft.snipes == 1


def test_sell_snipe_picks_off_stale_high_bid():
    # Arrange: bid is stale-high — true value sits below it.
    market = StubMarket(best_bid=999_000, best_ask=1_000_000)
    fund = StubFundamental(v=998_000)
    hft = HFTAgent(participant_id=2, latency_us=0, race_noise_us=0)

    # Act
    actions = hft.act(0, market, fund)

    # Assert
    assert len(actions) == 1
    a = actions[0]
    assert a.side == be.Side.Sell
    assert a.order_type == be.OrderType.IOC
    assert a.price == 999_000            # hits the stale bid
    assert a.quantity == hft.qty
    assert hft.snipes == 1


def test_no_snipe_when_value_inside_spread():
    # Arrange: true value strictly between bid and ask — no edge.
    market = StubMarket(best_bid=999_000, best_ask=1_000_000)
    fund = StubFundamental(v=999_500)
    hft = HFTAgent(participant_id=3, latency_us=0, race_noise_us=0)

    # Act
    actions = hft.act(0, market, fund)

    # Assert
    assert actions == []
    assert hft.snipes == 0


def test_transaction_cost_gate_blocks_thin_edge():
    # Arrange: tiny 50-tick edge above the ask.
    market = StubMarket(best_bid=999_000, best_ask=1_000_000)
    fund = StubFundamental(v=1_000_050)

    # Zero cost -> the thin edge is enough to snipe.
    hft_free = HFTAgent(participant_id=4, transaction_cost_bps=0,
                        latency_us=0, race_noise_us=0)
    assert len(hft_free.act(0, market, fund)) == 1

    # 10 bps cost => cost*ask = 1000 ticks; edge of 50 < 1000 -> no snipe.
    hft_costly = HFTAgent(participant_id=5, transaction_cost_bps=10,
                          latency_us=0, race_noise_us=0)
    assert hft_costly.act(0, market, fund) == []


def test_race_jitter_within_bound():
    # Arrange: persistent buy-snipe opportunity, fixed seed, race noise 1000us.
    market = StubMarket(best_bid=999_000, best_ask=1_000_000)
    fund = StubFundamental(v=1_000_500)
    hft = HFTAgent(participant_id=6, latency_us=0, race_noise_us=1000, seed=42)

    # Act / Assert: every submit_at must land in [0, 1000).
    for _ in range(50):
        actions = hft.act(0, market, fund)
        assert len(actions) == 1
        assert 0 <= actions[0].submit_at < 1000


def test_pnl_buy_below_value_is_profit():
    # Arrange: bought 50 @ 1_000_000, true value (mark) is 1_000_500.
    hft = HFTAgent(participant_id=7)
    trade = FakeTrade(price=1_000_000, quantity=50)

    # Act
    hft.on_fill(trade, is_buyer=True)

    # Assert
    assert hft.inventory == 50
    assert hft.cash < 0                      # paid out notional
    assert hft.pnl(mark_price=1_000_500) > 0  # bought below value -> profit


def test_pnl_sell_above_value_is_profit():
    # Arrange: sold 50 @ bid 999_000, true value (mark) is 998_000.
    hft = HFTAgent(participant_id=8)
    trade = FakeTrade(price=999_000, quantity=50)

    # Act
    hft.on_fill(trade, is_buyer=False)

    # Assert
    assert hft.inventory == -50
    assert hft.cash > 0                       # received notional
    assert hft.pnl(mark_price=998_000) > 0    # sold above value -> profit
