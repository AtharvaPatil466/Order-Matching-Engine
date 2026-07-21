"""Realized/unrealized P&L split (inventory-controlled rent metric).

The point of this module is Experiment 4: over long runs, `cash + inventory *
mark` is dominated by directional exposure on the fundamental random walk, not
by latency arbitrage. `realized` isolates the round-trip component, which is
where sniping profit actually lands.
"""
from __future__ import annotations

import sys
from pathlib import Path

_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_ROOT / "metrics"))

from position import EMPTY, apply_fill, total_pnl, unrealized  # noqa: E402


def _legacy_pnl(pos, mark: float) -> float:
    """The formula the agents used before the split; the identity must hold."""
    return pos.cash + pos.inventory * mark


def test_closed_long_round_trip_realizes_the_spread():
    # Arrange
    pos = apply_fill(EMPTY, 10, 100.0)

    # Act
    pos = apply_fill(pos, -10, 101.0)

    # Assert
    assert pos.inventory == 0
    assert pos.realized == 10.0
    assert unrealized(pos, 999.0) == 0.0        # flat: mark cannot matter
    assert total_pnl(pos, 999.0) == 10.0


def test_closed_short_round_trip_realizes_the_spread():
    # Arrange
    pos = apply_fill(EMPTY, -10, 100.0)

    # Act
    pos = apply_fill(pos, 10, 99.0)

    # Assert
    assert pos.inventory == 0
    assert pos.realized == 10.0
    assert total_pnl(pos, 999.0) == 10.0


def test_open_position_carries_no_realized_pnl():
    # Arrange / Act
    pos = apply_fill(EMPTY, 10, 100.0)

    # Assert — this is the term that swamps rent over long runs
    assert pos.realized == 0.0
    assert unrealized(pos, 105.0) == 50.0
    assert total_pnl(pos, 105.0) == 50.0


def test_partial_reduction_realizes_only_the_closed_quantity():
    # Arrange
    pos = apply_fill(EMPTY, 10, 100.0)

    # Act
    pos = apply_fill(pos, -4, 110.0)

    # Assert
    assert pos.inventory == 6
    assert pos.realized == 40.0
    assert pos.avg_cost == 100.0                # basis of the remaining lot is unchanged


def test_averaging_up_blends_the_cost_basis():
    # Arrange
    pos = apply_fill(EMPTY, 10, 100.0)

    # Act
    pos = apply_fill(pos, 10, 110.0)

    # Assert
    assert pos.inventory == 20
    assert pos.avg_cost == 105.0
    assert pos.realized == 0.0


def test_sign_flip_closes_the_old_lot_and_opens_at_the_fill_price():
    # Arrange
    pos = apply_fill(EMPTY, 10, 100.0)

    # Act — sell through zero into a short
    pos = apply_fill(pos, -30, 110.0)

    # Assert
    assert pos.inventory == -20
    assert pos.realized == 100.0                # only the 10 long units closed
    assert pos.avg_cost == 110.0                # the new short opens at the fill


def test_total_pnl_matches_the_legacy_cash_plus_inventory_formula():
    # Arrange — a path with adds, partial reduces and a sign flip
    fills = [(10, 100.0), (5, 102.0), (-7, 101.0), (-20, 99.0),
             (8, 98.5), (4, 103.0), (-3, 97.0)]
    pos = EMPTY

    # Act
    for qty, price in fills:
        pos = apply_fill(pos, qty, price)

    # Assert — the decomposition must not change the bottom line
    for mark in (90.0, 100.0, 115.0):
        assert abs(total_pnl(pos, mark) - _legacy_pnl(pos, mark)) < 1e-9


def test_apply_fill_does_not_mutate_its_input():
    # Arrange
    pos = apply_fill(EMPTY, 10, 100.0)

    # Act
    apply_fill(pos, -10, 120.0)

    # Assert
    assert pos.inventory == 10
    assert pos.realized == 0.0


def test_zero_quantity_fill_is_a_no_op():
    # Arrange
    pos = apply_fill(EMPTY, 10, 100.0)

    # Act / Assert
    assert apply_fill(pos, 0, 500.0) == pos
