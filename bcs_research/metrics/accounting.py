"""Fill-based accounting for the event model: fees, rebates, speed spend, two marks.

The tick model's agents each kept their own cash and welfare_decomposition.py
summed them. The event model derives every participant's book from the
engine's trade list instead, so the accounting is one fold over
list[TradeRecord] and cannot drift from what actually executed.

Three legs, and why each is booked where it is:

  fees   every fill pays maker_bps or taker_bps of its notional; the maker is
         the non-aggressor, and a negative maker fee is a rebate. Fees are
         TRANSFERS to the exchange: gross trading P&L sums to zero across
         participants, and net P&L plus exchange revenue sums to zero.
  speed  latency is bought ({participant_id: cost} for the run). That money
         leaves the market for fibre and towers; it is the real resource cost
         BCS calls waste, so it is reported beside the identities, never in them.
  marks  open inventory is valued at the last two-sided mid AND at the path's
         true V at the end. The mid is what participants could see; V is what
         the inventory was worth. Read rents at V.

Units: cash, P&L and fees are in position.py's units (engine price divided by
PRICE_PRECISION). Marks are passed as engine fixed-point prices, like V. The
gross leg IS position.apply_fill, reused rather than re-derived, so under the
ZERO schedule every figure equals a plain apply_fill fold.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from types import MappingProxyType
from typing import Iterable, Mapping

import bcs_engine as be
from position import EMPTY, Position, apply_fill, total_pnl

CLASSES = ("maker", "sniper", "noise", "informed")


@dataclass(frozen=True)
class FeeSchedule:
    maker_bps: float
    taker_bps: float


ZERO = FeeSchedule(0.0, 0.0)
# Binance USDⓈ-M futures, Regular (VIP 0) user: maker 0.0200%, taker 0.0500%.
# Source: Binance FAQ "Binance Futures Fee Structure & Fee Calculations",
# https://www.binance.com/en/support/faq/360033544231 (page updated 2026-05-01,
# checked 2026-10-10): "a Regular User's maker fee is 0.02% and a Regular
# User's taker fee is 0.05%". The FAQ defers to the live fee table; paying in
# BNB takes 10% off, and the API docs' sample response still shows a 4 bp taker.
TAKER_HEAVY = FeeSchedule(2.0, 5.0)
# Stylised maker-taker venue: the taker fee funds a maker rebate.
MAKER_REBATE = FeeSchedule(-1.0, 3.0)


@dataclass(frozen=True)
class Account:
    position: Position = EMPTY   # gross trading book (position.py)
    fees: float = 0.0            # paid to the exchange; negative = net rebate
    speed_cost: float = 0.0      # bought latency, outside the trading identities

    @property
    def inventory(self) -> int:
        return self.position.inventory

    @property
    def cash(self) -> float:
        """Account balance: trading cash less fees."""
        return self.position.cash - self.fees

    @property
    def realized(self) -> float:
        return self.position.realized

    def gross_pnl(self, mark: float) -> float:
        return total_pnl(self.position, mark / be.PRICE_PRECISION)

    def net_pnl(self, mark: float) -> float:
        return self.gross_pnl(mark) - self.fees


@dataclass(frozen=True)
class Ledger:
    accounts: Mapping[int, Account] = field(default_factory=dict)
    taker_fees: float = 0.0      # collected by the exchange
    maker_fees: float = 0.0      # collected; negative when it pays rebates

    @property
    def exchange_revenue(self) -> float:
        return self.taker_fees + self.maker_fees


def _booked(acc: Account, qty: int, price: float, fee: float) -> Account:
    return Account(apply_fill(acc.position, qty, price), acc.fees + fee, acc.speed_cost)


def build_ledger(trades: Iterable, fees: FeeSchedule = ZERO,
                 speed_costs: Mapping[int, float] | None = None) -> Ledger:
    """Fold a run's TradeRecords (contracts.py) into per-participant accounts.

    A participant that bought speed but never traded still gets an account,
    so the speed leg is complete.
    """
    books: dict[int, Account] = {}
    taker_total = maker_total = 0.0
    for tr in trades:
        price = tr.price / be.PRICE_PRECISION
        notional = price * tr.quantity
        taker_fee = fees.taker_bps * 1e-4 * notional
        maker_fee = fees.maker_bps * 1e-4 * notional
        buyer_fee, seller_fee = ((taker_fee, maker_fee) if tr.aggressor_buy
                                 else (maker_fee, taker_fee))
        books[tr.buyer_id] = _booked(books.get(tr.buyer_id, Account()),
                                     tr.quantity, price, buyer_fee)
        books[tr.seller_id] = _booked(books.get(tr.seller_id, Account()),
                                      -tr.quantity, price, seller_fee)
        taker_total += taker_fee
        maker_total += maker_fee
    for pid, cost in (speed_costs or {}).items():
        acc = books.get(pid, Account())
        books[pid] = Account(acc.position, acc.fees, float(cost))
    return Ledger(MappingProxyType(books), taker_total, maker_total)


def decompose(ledger: Ledger, classes: Mapping[int, str], mark_mid: float,
              mark_v: float) -> dict:
    """Welfare by participant class, at both marks, with the identity residuals.

    `classes` maps every participant id in the ledger to one of CLASSES; an
    unclassified participant would silently break the identities, so it raises.
    """
    unclassified = sorted(set(ledger.accounts) - set(classes))
    if unclassified:
        raise ValueError(f"participants without a class: {unclassified}")
    unknown = sorted({c for c in classes.values()} - set(CLASSES))
    if unknown:
        raise ValueError(f"unknown classes {unknown}; expected {CLASSES}")

    rows = {c: dict.fromkeys(("realized", "fees", "gross_mid", "gross_v",
                              "net_mid", "net_v", "speed_cost"), 0.0) | {"n": 0}
            for c in CLASSES}
    for pid, acc in ledger.accounts.items():
        r = rows[classes[pid]]
        r["n"] += 1
        r["realized"] += acc.realized
        r["fees"] += acc.fees
        r["gross_mid"] += acc.gross_pnl(mark_mid)
        r["gross_v"] += acc.gross_pnl(mark_v)
        r["net_mid"] += acc.net_pnl(mark_mid)
        r["net_v"] += acc.net_pnl(mark_v)
        r["speed_cost"] += acc.speed_cost

    def total(key):
        return sum(r[key] for r in rows.values())

    return {
        "mark_mid": mark_mid,
        "mark_v": mark_v,
        "by_class": rows,
        "exchange": {"revenue": ledger.exchange_revenue,
                     "taker_fees": ledger.taker_fees,
                     "maker_fees": ledger.maker_fees},
        "speed_cost_total": total("speed_cost"),   # the resource BCS calls waste
        # Identities, ~0 to rounding: gross P&L is zero-sum at any common mark;
        # fees only move money to the exchange.
        "gross_residual_mid": total("gross_mid"),
        "gross_residual_v": total("gross_v"),
        "transfer_residual_mid": total("net_mid") + ledger.exchange_revenue,
        "transfer_residual_v": total("net_v") + ledger.exchange_revenue,
    }
