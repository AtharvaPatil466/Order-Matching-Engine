"""Contracts for the event-driven model (the Tier-1 rebuild).

Every lane of the rebuild codes against this file; none edits it. A change goes
through the integration branch so all lanes see it at once. The tick model
(scheduler.py, batch_auction.py) is left untouched so every published result
keeps reproducing while the new model is built beside it.

Units: time is integer NANOSECONDS everywhere in the event model (`*_ns`).
Order prices are engine fixed-point ints; values (V, the public signal) are
floats in the same fixed-point space, as in fundamental_value.py.

Latency semantics the scheduler implements:
  - An agent OBSERVES a SignalUpdate, and its own fills, `md_latency_ns` after
    they happen. Its callback runs at that time.
  - Every Order/Cancel an agent returns from a callback at time t ARRIVES at
    the engine at t + order_latency_ns (+ any race jitter the scheduler adds).
  - Arrival order at the engine is the race. Exact arrival ties are broken by
    a seeded random permutation, never by registration order.
  - A Wakeup is a self-scheduled callback (requotes, Poisson arrivals); it
    involves no latency.
  - BookView reads the engine's CURRENT book. ponytail: book-data latency is
    not modelled; the race is driven by the signal latency. Add it if a
    strategy turns out to depend on seeing a stale book.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Protocol, Sequence, Union


@dataclass(frozen=True)
class SignalUpdate:
    """A public price update, e.g. a move in a correlated instrument."""
    t_ns: int
    value: float
    is_jump: bool = False


class FundamentalPath(Protocol):
    """A pre-generated, seed-deterministic path over [0, duration_ns)."""

    duration_ns: int

    def value_at(self, t_ns: int) -> float:
        """True V in effect at t. For accounting and adverse-fill tests only;
        agents trade on the updates they observe, never on this."""

    def updates(self) -> Sequence[SignalUpdate]:
        """Every public update, ascending in t_ns."""


@dataclass(frozen=True)
class Order:
    order_id: int
    side: object          # be.Side
    price: int
    quantity: int
    order_type: object    # be.OrderType
    tif: object = None    # be.TimeInForce; None means GTC


@dataclass(frozen=True)
class Cancel:
    order_id: int


@dataclass(frozen=True)
class Wakeup:
    at_ns: int
    tag: str = ""


Action = Union[Order, Cancel, Wakeup]


@dataclass(frozen=True)
class TradeRecord:
    """One execution as the engine reported it. The maker is the non-aggressor."""
    t_ns: int
    price: int
    quantity: int
    buyer_id: int
    seller_id: int
    aggressor_buy: bool


@dataclass(frozen=True)
class Fill:
    """One agent's side of a TradeRecord, delivered to that agent."""
    t_ns: int             # engine execution time, not delivery time
    order_id: int
    price: int
    quantity: int
    is_buyer: bool
    is_maker: bool        # True when this agent's order was resting


class BookView(Protocol):
    def best_bid(self) -> int: ...
    def best_ask(self) -> int: ...


class EventAgent(Protocol):
    participant_id: int
    md_latency_ns: int
    order_latency_ns: int

    def on_start(self, t_ns: int, view: BookView) -> Sequence[Action]: ...
    def on_signal(self, t_ns: int, update: SignalUpdate, view: BookView) -> Sequence[Action]: ...
    def on_wakeup(self, t_ns: int, tag: str, view: BookView) -> Sequence[Action]: ...
    def on_fill(self, t_ns: int, fill: Fill, view: BookView) -> Sequence[Action]: ...
