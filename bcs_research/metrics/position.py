"""Cash/inventory book with a weighted-average cost basis.

Splits an agent's P&L into two terms that behave very differently as run length
grows:

  realized    round-trip profit from matched buy/sell pairs. This is where
              latency arbitrage lands: a sniper's edge is earned by closing a
              position it opened against a stale quote.
  unrealized  inventory marked against the current price. Pure directional
              exposure. Its variance scales with the fundamental's displacement,
              so over a long simulation it swamps the rent signal entirely
              (Experiment 4: per-seed rent dispersion grows ~linearly in run
              length rather than as its square root).

`total_pnl` reproduces the `cash + inventory * mark` figure the agents reported
before this split, so the decomposition is additive and changes no headline.

Prices and cash are in the same units the agents already use: engine ticks
divided by PRICE_PRECISION. This module has no engine dependency.
"""
from __future__ import annotations

from typing import NamedTuple


class Position(NamedTuple):
    inventory: int = 0
    cash: float = 0.0
    avg_cost: float = 0.0     # weighted-average entry price of the open lot
    realized: float = 0.0     # round-trip P&L, inventory-neutral


EMPTY = Position()


def apply_fill(pos: Position, qty: int, price: float) -> Position:
    """Return a new Position after a fill. qty > 0 buys, qty < 0 sells.

    Adding to a position blends the cost basis; reducing one realizes P&L on the
    closed quantity only; a fill that crosses through zero closes the old lot and
    opens the remainder at the fill price.
    """
    if qty == 0:
        return pos

    cash = pos.cash - qty * price
    inventory = pos.inventory + qty

    is_opening = pos.inventory == 0 or (pos.inventory > 0) == (qty > 0)
    if is_opening:
        blended = abs(pos.inventory) * pos.avg_cost + abs(qty) * price
        return pos._replace(inventory=inventory, cash=cash,
                            avg_cost=blended / abs(inventory))

    # Reducing or flipping: profit accrues on the quantity that closes.
    closed = min(abs(qty), abs(pos.inventory))
    direction = 1.0 if pos.inventory > 0 else -1.0
    realized = pos.realized + direction * (price - pos.avg_cost) * closed

    if inventory == 0:
        return Position(0, cash, 0.0, realized)
    if (inventory > 0) == (pos.inventory > 0):
        return Position(inventory, cash, pos.avg_cost, realized)   # partial reduce
    return Position(inventory, cash, price, realized)              # flipped through zero


def unrealized(pos: Position, mark: float) -> float:
    """Mark-to-market on the open lot. Zero when flat, whatever the mark."""
    return pos.inventory * (mark - pos.avg_cost)


def total_pnl(pos: Position, mark: float) -> float:
    """Equals the legacy `cash + inventory * mark`."""
    return pos.realized + unrealized(pos, mark)
