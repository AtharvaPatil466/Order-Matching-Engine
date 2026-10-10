"""StrategicMaker — one maker class that executes any MakerPolicy.

Why: the next wave searches for a maker's best response (and a Nash point for
several makers) over widening, decay, size and staying out. A search needs
the strategy to be a small, frozen, hashable value it can perturb, compare
and cache; so a maker's whole behaviour is a MakerPolicy, and this one class
runs any of them. The default policy is EventMaker exactly — same orders, ids
and bursts, so the same trades and race log on the same seeds — and every
dimension below is off until a field turns it on.

Dimensions (MakerPolicy fields):
  quote                False stays out of the market: the no-quote option
                       that gives an entry/exit margin.
  half_spread, qty     the base quote, as EventMaker.
  n_levels, level_spacing, level_qty
                       a ladder: level k rests k*level_spacing behind the touch,
                       level 0 with qty, deeper levels with level_qty.
  as_gamma, as_sigma2_per_s, as_horizon_ns
                       Avellaneda-Stoikov quoting (below).
  inventory_skew       linear skew, as the tick maker: the centre moves by
                       -inventory_skew per unit held, separately from A-S.
  inventory_limit      at |inventory| >= limit the side that would add to it
                       is not quoted.
  adverse_sensitivity, spread_decay, spread_cap_mult, adverse_threshold,
  thin_liquidity, min_quote_qty
                       bcs_market_maker.py's mechanism (paper §3.2): widen the
                       half-spread x(1+sensitivity) only on a fill that was
                       adverse against the TRUE V, decay back toward base on
                       every observed update, and optionally thin resting size
                       by the same factor.
  fade_ns, fade_widen  after observing a jump-flagged update or an adverse fill,
                       pull every quote (fade_widen None) or widen the
                       half-spread by fade_widen, for fade_ns, then requote.
  requote_threshold    cancel and requote on an update only when the signal is
                       at least this far from where the live quotes are centred;
                       0 is EventMaker's cancel-on-every-update. Above the move
                       size the maker sends no cancel, so a jump snipes it.
  requote_after_fill   when a fill empties a live order, requote at once rather
                       than leaving that side empty until the next update.
  post_only            send quotes as post-only, which the engine refuses when
                       they would cross. Two equally fast plain-limit makers
                       cross each other's stale quotes after every move; real
                       makers use post-only so a quote never takes. A refused
                       quote is learned through on_reject, dropped, and treated
                       like a side a fill emptied (requote_after_fill decides).

Avellaneda-Stoikov, and why this version. Avellaneda & Stoikov (2008, "High-
frequency trading in a limit order book", Quant. Finance 8(3)) give, for CARA
risk aversion gamma, mid variance sigma^2 and time to horizon tau = T - t,
    reservation price  r = s - q gamma sigma^2 tau
    half-spread        (1/gamma) ln(1 + gamma/kappa) + gamma sigma^2 tau / 2
with order arrivals lambda(delta) = A exp(-kappa delta). We hold tau constant
(a rolling horizon, `as_horizon_ns`) instead of letting T - t run down. A
shrinking horizon would make a maker in a 60 s run and in a one-hour run quote
differently near the end for no economic reason, and the best-response search
compares policies across run lengths. The stationary Gueant-Lehalle-Fernandez-
Tapia (2013, "Dealing with the inventory risk", Math. Finan. Econ. 7(4))
approximation is the principled alternative, but its inventory term needs the
arrival scale A, which in this model is an equilibrium outcome of the noise
population and rival makers rather than a primitive anyone could set.
kappa is not a separate field: at gamma = 0 the A-S half-spread is 1/kappa,
so kappa = 1/half_spread. That makes gamma = 0 symmetric fixed-spread quoting
at exactly `half_spread`, and leaves the search no redundant dimension.

True V. Like the tick maker, the adverse test compares the fill price with the
TRUE V at execution, `path.value_at(fill.t_ns)` (the value the maker
eventually learns). That adverse-fill test is the only use of the path, one of
the exceptions contracts.py sanctions; quotes use observed updates only. A
policy that needs it (widening or fading on) must be given the path.

Live orders. `_live` is what the maker BELIEVES rests: an order leaves it when
the maker sends its cancel, or when a fill it observes empties it. Plain limits
need no cancel acks: a cancelled order is gone once the cancel lands, and a
cancel that arrives too late (the order already traded) changes nothing the
maker would track, because the fill still arrives and still moves
`inventory`. Inventory therefore counts every fill, including fills on orders
it has already cancelled; the requote/limit logic only ever reads observed
quantities. A post-only quote can also be refused outright, so a post-only
maker (only) defines on_reject and drops it; a plain-limit maker leaves the
callback undefined, so it gets no reject events and the default policy's runs
stay identical to EventMaker's.
"""
from __future__ import annotations

import math
from dataclasses import dataclass
from typing import NamedTuple, Optional

import bcs_engine as be
from contracts import Cancel, Order, Wakeup

NS_PER_S = 1_000_000_000
_BUY, _SELL = be.Side.Buy, be.Side.Sell
_LIMIT, _POST_ONLY = be.OrderType.Limit, be.OrderType.PostOnly
_UNFADE = "unfade"


@dataclass(frozen=True)
class MakerPolicy:
    """A maker's whole strategy. Prices in engine fixed-point units."""
    quote: bool = True
    half_spread: float = 200.0
    qty: int = 10
    n_levels: int = 1
    level_spacing: int = 100
    level_qty: Optional[int] = None          # None: deeper levels also rest qty
    as_gamma: float = 0.0                    # per fixed-point unit per share
    as_sigma2_per_s: float = 0.0             # var of V per second, fixed-point units^2
    as_horizon_ns: int = NS_PER_S
    inventory_skew: float = 0.0              # price units per share held
    inventory_limit: Optional[int] = None
    adverse_sensitivity: float = 0.0
    spread_decay: float = 0.05
    spread_cap_mult: float = 10.0
    adverse_threshold: float = 0.0
    thin_liquidity: bool = True
    min_quote_qty: int = 1
    fade_ns: int = 0
    fade_widen: Optional[float] = None       # None pulls quotes while fading
    requote_threshold: float = 0.0
    requote_after_fill: bool = False
    post_only: bool = False

    def __post_init__(self):
        checks = {
            "half_spread must be >= 0 (> 0 when as_gamma > 0, kappa = 1/half_spread)":
                self.half_spread > 0 or (self.half_spread == 0 and self.as_gamma == 0),
            "qty, min_quote_qty and n_levels must be >= 1":
                min(self.qty, self.min_quote_qty, self.n_levels) >= 1,
            "level_spacing must be >= 1": self.level_spacing >= 1,
            "level_qty must be None or >= 1": self.level_qty is None or self.level_qty >= 1,
            "as_gamma and as_sigma2_per_s must be >= 0":
                self.as_gamma >= 0 and self.as_sigma2_per_s >= 0,
            "as_horizon_ns must be positive": self.as_horizon_ns > 0,
            "inventory_limit must be None or >= 1":
                self.inventory_limit is None or self.inventory_limit >= 1,
            "adverse_sensitivity and adverse_threshold must be >= 0":
                self.adverse_sensitivity >= 0 and self.adverse_threshold >= 0,
            "spread_decay must be in [0, 1]": 0 <= self.spread_decay <= 1,
            "spread_cap_mult must be >= 1": self.spread_cap_mult >= 1,
            "fade_ns must be >= 0": self.fade_ns >= 0,
            "fade_widen must be None or >= 0": self.fade_widen is None or self.fade_widen >= 0,
            "requote_threshold must be >= 0": self.requote_threshold >= 0,
        }
        bad = [msg for msg, ok in checks.items() if not ok]
        if bad:
            raise ValueError("; ".join(bad))

    @property
    def needs_true_value(self) -> bool:
        """Widening or fading reads the adverse-fill test, which needs the path."""
        return self.adverse_sensitivity > 0 or self.fade_ns > 0


class Quote(NamedTuple):
    side: object      # be.Side
    price: int
    qty: int          # what the maker believes still rests


class StrategicMaker:
    def __init__(self, participant_id, policy=MakerPolicy(), path=None,
                 md_latency_ns=0, order_latency_ns=0):
        if policy.needs_true_value and path is None:
            raise ValueError("this policy widens or fades on adverse fills; pass the path")
        self.participant_id = int(participant_id)
        self.md_latency_ns = int(md_latency_ns)
        self.order_latency_ns = int(order_latency_ns)
        self.policy = policy
        self._path = path
        self._oid = self.participant_id * 1_000_000_000
        self._live = {}            # order id -> Quote
        self._value = None         # latest observed signal
        self._anchor = None        # signal the live quotes are centred on
        self._fade_until = -1
        self.inventory = 0
        self.spread_mult = 1.0     # adverse widening state, 1 = base
        self.adverse_fills = 0
        self.rejects = 0           # post-only quotes the engine refused
        if policy.post_only:       # see "Live orders" above
            self.on_reject = self._on_reject
        g, tau_s = policy.as_gamma, policy.as_horizon_ns / NS_PER_S
        self._as_shift = g * policy.as_sigma2_per_s * tau_s   # r = s - q * this
        self._base_half = (policy.half_spread if g == 0
                           else math.log1p(g * policy.half_spread) / g + self._as_shift / 2)

    @property
    def live_orders(self):
        return dict(self._live)

    def on_start(self, t_ns, view):
        return []

    def on_signal(self, t_ns, update, view):
        p = self.policy
        if not p.quote:
            return []
        self._value = update.value
        # ponytail: decays once per observed update, as the tick maker did once
        # per requote tick; make it exp(-dt/tau) if update cadence varies by run.
        self.spread_mult = 1.0 + (self.spread_mult - 1.0) * (1.0 - p.spread_decay)
        if update.is_jump and p.fade_ns:
            return self._fade(t_ns)
        if self._anchor is not None and abs(update.value - self._anchor) < p.requote_threshold:
            return []
        return self._requote(t_ns)

    def on_wakeup(self, t_ns, tag, view):
        # A later trigger pushes _fade_until out; the earlier wakeup is then stale.
        if tag != _UNFADE or t_ns < self._fade_until:
            return []
        return self._requote(t_ns)

    def on_fill(self, t_ns, fill, view):
        p = self.policy
        self.inventory += fill.quantity if fill.is_buyer else -fill.quantity
        quote = self._live.get(fill.order_id)
        emptied = False
        if quote is not None:       # None: already cancelled, filled before the cancel landed
            if quote.qty > fill.quantity:
                self._live[fill.order_id] = quote._replace(qty=quote.qty - fill.quantity)
            else:
                del self._live[fill.order_id]
                emptied = True
        if self._path is not None and self._is_adverse(fill):
            self.adverse_fills += 1
            self.spread_mult = min(self.spread_mult * (1.0 + p.adverse_sensitivity),
                                   p.spread_cap_mult)
            if p.fade_ns:
                return self._fade(t_ns)
        if emptied and p.requote_after_fill:
            return self._requote(t_ns)
        return []

    def _on_reject(self, t_ns, order_id, reason_name, view):
        # A too-late cancel names an order already dropped here: nothing to do.
        if self._live.pop(order_id, None) is None:
            return []
        self.rejects += 1
        # ponytail: no post-only "slide" (repricing a refused quote behind the
        # opposite touch). With requote_after_fill a cross that persists is
        # re-refused every md + order latency until the rival requotes; slide
        # against view.best_bid/ask if a strategy needs that side filled.
        return self._requote(t_ns) if self.policy.requote_after_fill else []

    def _is_adverse(self, fill):
        v = self._path.value_at(fill.t_ns)
        edge = fill.price - v if fill.is_buyer else v - fill.price
        return edge > self.policy.adverse_threshold

    def _fade(self, t_ns):
        self._fade_until = t_ns + self.policy.fade_ns   # callbacks run in time order
        return self._requote(t_ns) + [Wakeup(self._fade_until, _UNFADE)]

    def _requote(self, t_ns):
        """Cancel everything believed live and post the policy's quote set, in one
        burst, so the cancels reach the engine ahead of the new quotes."""
        actions = [Cancel(oid) for oid in self._live]
        self._live = {}
        otype = _POST_ONLY if self.policy.post_only else _LIMIT
        for quote in self._quotes(fading=t_ns < self._fade_until):
            self._oid += 1
            self._live[self._oid] = quote
            actions.append(Order(self._oid, quote.side, quote.price, quote.qty, otype))
        self._anchor = self._value
        return actions

    def _quotes(self, fading):
        p = self.policy
        if fading and p.fade_widen is None:
            return []
        q = self.inventory
        centre = self._value - (p.inventory_skew + self._as_shift) * q
        hs = self._base_half * self.spread_mult + (p.fade_widen if fading else 0.0)
        bid = max(1, int(round(centre - hs)))
        ask = max(bid + 1, int(round(centre + hs)))
        # ponytail: the limit gates whole sides on current inventory, so one full
        # fill can overshoot it by a level's size; cap sizes if the bound must be hard.
        quote_bids = p.inventory_limit is None or q < p.inventory_limit
        quote_asks = p.inventory_limit is None or q > -p.inventory_limit
        out = []
        for k in range(p.n_levels):
            size = p.qty if k == 0 or p.level_qty is None else p.level_qty
            if p.thin_liquidity:
                size = max(p.min_quote_qty, round(size / self.spread_mult))
            off = k * p.level_spacing
            if quote_bids and bid - off >= 1:
                out.append(Quote(_BUY, bid - off, size))
            if quote_asks:
                out.append(Quote(_SELL, ask + off, size))
        return out
