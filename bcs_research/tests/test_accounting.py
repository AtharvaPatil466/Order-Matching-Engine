"""Fill-based accounting: fee legs, speed leg, two marks, and the transfer identities.

The identities are checked on random synthetic trade lists; "machine
precision" means a residual within 1e-12 of total traded notional, i.e. a few
ulps per fill summed over the run.
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

_ROOT = Path(__file__).resolve().parents[1]
for _d in ("build", "metrics", "simulation"):
    sys.path.insert(0, str(_ROOT / _d))

import bcs_engine as be  # noqa: E402
from accounting import (MAKER_REBATE, TAKER_HEAVY, ZERO, FeeSchedule,  # noqa: E402
                        build_ledger, decompose)
from contracts import TradeRecord  # noqa: E402
from position import EMPTY, apply_fill, total_pnl  # noqa: E402

PP = be.PRICE_PRECISION
P100 = 100 * PP
CLASSES = {1: "maker", 2: "maker", 3: "sniper", 4: "noise", 5: "informed"}


def _random_trades(seed, n=400):
    rng = np.random.default_rng(seed)
    trades = []
    for k in range(n):
        buyer, seller = rng.choice(list(CLASSES), size=2, replace=False)
        trades.append(TradeRecord(t_ns=k * 1000, price=int(P100 + rng.integers(-500, 500)),
                                  quantity=int(rng.integers(1, 50)),
                                  buyer_id=int(buyer), seller_id=int(seller),
                                  aggressor_buy=bool(rng.random() < 0.5)))
    return trades


def _notional(trades):
    return sum(t.price / PP * t.quantity for t in trades)


def _one_trade(aggressor_buy=True):
    return [TradeRecord(t_ns=0, price=P100, quantity=10, buyer_id=3, seller_id=1,
                        aggressor_buy=aggressor_buy)]


@pytest.mark.parametrize("seed", [0, 1, 2])
def test_zero_fees_reproduce_position_py_exactly(seed):
    # Arrange: the fold every tick-model agent ran, one apply_fill per side
    trades = _random_trades(seed)
    expected = {pid: EMPTY for pid in CLASSES}
    for t in trades:
        expected[t.buyer_id] = apply_fill(expected[t.buyer_id], t.quantity, t.price / PP)
        expected[t.seller_id] = apply_fill(expected[t.seller_id], -t.quantity, t.price / PP)
    mark = P100 + 123

    # Act
    ledger = build_ledger(trades, ZERO)

    # Assert: exact float equality, no tolerance
    for pid, pos in expected.items():
        acc = ledger.accounts[pid]
        assert acc.position == pos
        assert acc.cash == pos.cash
        assert acc.net_pnl(mark) == total_pnl(pos, mark / PP)
    assert ledger.exchange_revenue == 0.0


@pytest.mark.parametrize("fees", [ZERO, TAKER_HEAVY, MAKER_REBATE])
@pytest.mark.parametrize("seed", [0, 1, 2])
def test_gross_pnl_is_zero_sum_and_fees_are_pure_transfers(fees, seed):
    # Arrange
    trades = _random_trades(seed)
    tol = 1e-12 * _notional(trades)

    # Act
    w = decompose(build_ledger(trades, fees), CLASSES, mark_mid=P100 + 40, mark_v=P100 - 70)

    # Assert
    assert abs(w["gross_residual_mid"]) < tol
    assert abs(w["gross_residual_v"]) < tol
    assert abs(w["transfer_residual_mid"]) < tol
    assert abs(w["transfer_residual_v"]) < tol


def test_aggressor_pays_taker_and_resting_side_pays_maker():
    # Arrange: 10 units at 100.00 -> notional 1000
    trades = _one_trade(aggressor_buy=True)

    # Act
    ledger = build_ledger(trades, TAKER_HEAVY)

    # Assert
    assert ledger.accounts[3].fees == pytest.approx(0.5)     # buyer took: 5 bp
    assert ledger.accounts[1].fees == pytest.approx(0.2)     # seller rested: 2 bp
    assert ledger.exchange_revenue == pytest.approx(0.7)


def test_negative_maker_fee_is_a_rebate_funded_by_the_taker():
    # Arrange: seller aggresses, buyer rests
    trades = _one_trade(aggressor_buy=False)

    # Act
    ledger = build_ledger(trades, MAKER_REBATE)

    # Assert
    maker, taker = ledger.accounts[3], ledger.accounts[1]
    assert maker.fees == pytest.approx(-0.1)                 # received 1 bp
    assert maker.cash == pytest.approx(-1000.0 + 0.1)
    assert taker.fees == pytest.approx(0.3)
    assert ledger.maker_fees == pytest.approx(-0.1)
    assert ledger.exchange_revenue == pytest.approx(0.2)


def test_inventory_is_marked_at_both_the_mid_and_true_v():
    # Arrange: buyer 3 ends long 10, seller 1 short 10
    ledger = build_ledger(_one_trade(), ZERO)

    # Act
    w = decompose(ledger, CLASSES, mark_mid=P100 + 2 * PP, mark_v=P100 - PP)

    # Assert: +10 * 2.00 at the mid, -10 * 1.00 at V
    assert w["by_class"]["sniper"]["gross_mid"] == pytest.approx(20.0)
    assert w["by_class"]["sniper"]["gross_v"] == pytest.approx(-10.0)
    assert w["by_class"]["maker"]["net_v"] == pytest.approx(10.0)


def test_speed_spend_is_reported_beside_the_identities_not_inside_them():
    # Arrange: participant 5 buys speed and never trades
    speed = {3: 250.0, 5: 40.0}

    # Act
    ledger = build_ledger(_one_trade(), TAKER_HEAVY, speed_costs=speed)
    w = decompose(ledger, CLASSES, mark_mid=P100, mark_v=P100)

    # Assert
    assert ledger.accounts[5].speed_cost == 40.0
    assert ledger.accounts[5].position == EMPTY
    assert w["by_class"]["sniper"]["speed_cost"] == 250.0
    assert w["by_class"]["informed"]["speed_cost"] == 40.0
    assert w["speed_cost_total"] == 290.0
    assert w["transfer_residual_v"] == pytest.approx(0.0, abs=1e-12)
    assert w["by_class"]["noise"]["n"] == 0


def test_decompose_rejects_unclassified_participants_and_unknown_classes():
    # Arrange
    ledger = build_ledger(_one_trade(), ZERO)

    # Act / Assert
    with pytest.raises(ValueError, match="without a class"):
        decompose(ledger, {3: "sniper"}, P100, P100)
    with pytest.raises(ValueError, match="unknown classes"):
        decompose(ledger, {3: "snipers", 1: "maker"}, P100, P100)


def test_fee_schedule_presets():
    assert ZERO == FeeSchedule(0.0, 0.0)
    assert (TAKER_HEAVY.maker_bps, TAKER_HEAVY.taker_bps) == (2.0, 5.0)
    assert MAKER_REBATE.maker_bps < 0 < MAKER_REBATE.taker_bps
