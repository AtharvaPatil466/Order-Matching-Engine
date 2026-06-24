"""BatchAuctionScheduler — frequent batch auction above the pybind11 bridge.

The continuous arm (LatencyScheduler) routes every order through the verified C++
matcher, which matches immediately. A batch auction instead ACCUMULATES orders
for `batch_interval_ticks` and then clears them all at ONE uniform price — the
discrete "uncross" the engine's Auction.tla spec models (SingleClearingPrice).
The pybind bridge does not expose uncross, so the clearing is done here in Python,
replicating OrderBook::discoverUncrossPrice (src/OrderBook.cpp) faithfully so it
is the same rule the engine proves SingleClearingPrice over.

Why a batch neutralizes the HFT race (BCS): submission TIME within an interval is
irrelevant — only price-time priority among accumulated orders at the uncross
matters. The market maker posts resting Limit quotes; its (latency-delayed)
cancel of a stale quote lands in the same batch before the uncross, so the stale
quote is gone by clearing time and the HFT's IOC finds nothing to pick off at the
stale price. Noise and HFT orders are IOC (aggressive): they accumulate, take
part in the uncross at P*, and any unfilled remainder is cancelled (true to IOC),
which also keeps the resting Limit book two-sided between uncrosses (no spurious
empty-book "gap" artifact).

Produces the same SimResult shape as LatencyScheduler, so all existing metrics
(welfare decomposition, liquidity gaps, Kyle's lambda) apply unchanged.
"""
from __future__ import annotations

import heapq

import bcs_engine as be
from scheduler import Action, SimResult  # noqa: F401  (Action re-exported for agents)


def discover_uncross_price(buys, sells, reference=None):
    """Uniform clearing price replicating OrderBook::discoverUncrossPrice.

    buys / sells: iterables of (price, qty). Candidate prices are the populated
    limit levels on either side. Returns (price, volume, buy_surplus); price is
    None when nothing crosses. Tie-break cascade: maximize executable volume,
    minimize imbalance, minimize distance to `reference` (if > 0), then market
    pressure (buy surplus clears higher, sell surplus lower).
    """
    buys = [(int(p), int(q)) for p, q in buys if q > 0]
    sells = [(int(p), int(q)) for p, q in sells if q > 0]
    if not buys and not sells:
        return (None, 0, True)

    candidates = sorted({p for p, _ in buys} | {p for p, _ in sells})
    have_best = False
    best_price = None
    best_vol = 0
    best_imb = 0
    best_buy_surplus = True

    for p in candidates:
        cum_buy = sum(q for bp, q in buys if bp >= p)
        cum_sell = sum(q for sp, q in sells if sp <= p)
        vol = min(cum_buy, cum_sell)
        imb = abs(cum_buy - cum_sell)
        buy_surplus = cum_buy >= cum_sell

        if not have_best:
            better = True
        elif vol != best_vol:
            better = vol > best_vol
        elif imb != best_imb:
            better = imb < best_imb
        elif reference is not None and reference > 0:
            d_cur = abs(p - reference)
            d_best = abs(best_price - reference)
            if d_cur != d_best:
                better = d_cur < d_best
            else:
                better = (p > best_price) if best_buy_surplus else (p < best_price)
        else:
            better = (p > best_price) if best_buy_surplus else (p < best_price)

        if better:
            have_best = True
            best_price, best_vol, best_imb, best_buy_surplus = p, vol, imb, buy_surplus

    if best_vol == 0:
        return (None, 0, best_buy_surplus)
    return (best_price, best_vol, best_buy_surplus)


class _Trade:
    """Minimal trade record passed to agent.on_fill (price/quantity are read)."""

    __slots__ = ("price", "quantity", "buyer_id", "seller_id", "aggressor_buy")

    def __init__(self, price, quantity, buyer_id, seller_id, aggressor_buy):
        self.price = price
        self.quantity = quantity
        self.buyer_id = buyer_id
        self.seller_id = seller_id
        self.aggressor_buy = aggressor_buy


class _BatchMarketView:
    """Read-only best bid/ask/mid over the resting Limit book."""

    def __init__(self, sched):
        self._s = sched

    def best_bid(self):
        return self._s._best_bid()

    def best_ask(self):
        return self._s._best_ask()

    def mid(self):
        bb, ba = self._s._best_bid(), self._s._best_ask()
        return (bb + ba) // 2 if bb > 0 and ba > 0 else 0


class BatchAuctionScheduler:
    def __init__(self, symbol, dt_us, batch_interval_ticks):
        self._sym = symbol
        self.dt_us = int(dt_us)
        self.batch_interval = max(1, int(batch_interval_ticks))
        self.market = _BatchMarketView(self)
        self._pending = []          # heap of (submit_at, enqueue_seq, Action)
        self._enq_seq = 0
        self._order_seq = 0         # arrival order for price-time priority
        self._limits = {}           # order_id -> resting Limit order dict
        self._aggressive = []       # IOC/Market orders accumulated this window
        self._by_id = {}
        self._reference = None      # last clearing price (tie-break anchor)
        self.snapshots = []
        self.trades = []
        self._tick_volume = 0
        self._tick_signed = 0

    # --- book queries --------------------------------------------------------

    def _best_bid(self):
        bids = [o["price"] for o in self._limits.values()
                if o["side"] == be.Side.Buy and o["qty"] > 0]
        return max(bids) if bids else 0

    def _best_ask(self):
        asks = [o["price"] for o in self._limits.values()
                if o["side"] == be.Side.Sell and o["qty"] > 0]
        return min(asks) if asks else 0

    # --- scheduling ----------------------------------------------------------

    def _enqueue(self, action):
        heapq.heappush(self._pending, (action.submit_at, self._enq_seq, action))
        self._enq_seq += 1

    def _submit_due(self, t):
        while self._pending and self._pending[0][0] <= t:
            _, _, a = heapq.heappop(self._pending)
            if a.kind == "cancel":
                self._limits.pop(a.cancel_order_id, None)
                continue
            order = {
                "order_id": a.order_id, "participant_id": a.participant_id,
                "side": a.side, "price": int(a.price), "qty": int(a.quantity),
                "seq": self._order_seq,
            }
            self._order_seq += 1
            if a.order_type == be.OrderType.Limit:
                self._limits[a.order_id] = order   # rests across windows
            else:
                self._aggressive.append(order)     # IOC/Market: this window only

    # --- clearing ------------------------------------------------------------

    def _uncross(self, t):
        participating = list(self._limits.values()) + self._aggressive
        buys = [(o["price"], o["qty"]) for o in participating
                if o["side"] == be.Side.Buy and o["qty"] > 0]
        sells = [(o["price"], o["qty"]) for o in participating
                 if o["side"] == be.Side.Sell and o["qty"] > 0]
        price, vol, buy_surplus = discover_uncross_price(buys, sells, self._reference)
        if price is None or vol == 0:
            self._aggressive = []   # unfilled aggressive orders cancelled
            return

        buy_orders = sorted(
            (o for o in participating
             if o["side"] == be.Side.Buy and o["qty"] > 0 and o["price"] >= price),
            key=lambda o: (-o["price"], o["seq"]))
        sell_orders = sorted(
            (o for o in participating
             if o["side"] == be.Side.Sell and o["qty"] > 0 and o["price"] <= price),
            key=lambda o: (o["price"], o["seq"]))

        remaining = vol
        bi = si = 0
        filled_total = 0
        while remaining > 0 and bi < len(buy_orders) and si < len(sell_orders):
            b, s = buy_orders[bi], sell_orders[si]
            fill = min(b["qty"], s["qty"], remaining)
            if fill <= 0:
                break
            b["qty"] -= fill
            s["qty"] -= fill
            remaining -= fill
            filled_total += fill
            self._route_fill(t, price, fill, b["participant_id"],
                             s["participant_id"], buy_surplus)
            if b["qty"] == 0:
                bi += 1
            if s["qty"] == 0:
                si += 1

        # Fully-filled limits drop out; partials rest on. Aggressive buffer clears.
        self._limits = {oid: o for oid, o in self._limits.items() if o["qty"] > 0}
        self._aggressive = []
        self._reference = price
        self._tick_volume += filled_total
        self._tick_signed += filled_total if buy_surplus else -filled_total

    def _route_fill(self, t, price, qty, buyer_id, seller_id, buy_surplus):
        self.trades.append({
            "t": t, "price": price, "quantity": qty,
            "buyer_id": buyer_id, "seller_id": seller_id,
            "aggressor_buy": buy_surplus,
        })
        tr = _Trade(price, qty, buyer_id, seller_id, buy_surplus)
        buyer = self._by_id.get(buyer_id)
        seller = self._by_id.get(seller_id)
        if buyer is not None:
            buyer.on_fill(tr, True)
        if seller is not None:
            seller.on_fill(tr, False)

    def _record(self, t, v):
        self.snapshots.append({
            "t": t,
            "best_bid": self._best_bid(),
            "best_ask": self._best_ask(),
            "mid": self.market.mid(),
            "v": v,
            "tick_volume": self._tick_volume,
            "signed_volume": self._tick_signed,
        })

    # --- driver --------------------------------------------------------------

    def run(self, agents, fundamental, duration_us):
        for ag in agents:
            self._by_id[ag.participant_id] = ag
        t = 0
        duration_us = int(duration_us)
        tick = 0
        while t < duration_us:
            v = fundamental.step(t)
            self._tick_volume = 0
            self._tick_signed = 0
            for ag in agents:
                for action in ag.act(t, self.market, fundamental):
                    self._enqueue(action)
            self._submit_due(t)
            tick += 1
            if tick % self.batch_interval == 0:
                self._uncross(t)
            self._record(t, v)
            t += self.dt_us
        return SimResult(self.snapshots, self.trades, list(agents),
                         duration_us, self.dt_us)
