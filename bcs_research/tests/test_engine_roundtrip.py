"""Proves the pybind11 bridge drives the real C++ MatchingEngine end to end.

Submits orders from Python, lets the verified engine match them, and reads
fills and book state back. This is the foundation the whole BCS extension
stands on: if these pass, Python agents can trade against the verified engine.
"""
import bcs_engine as be

SYM = 1
P = be.PRICE_PRECISION  # fixed-point scale (10000): price 100.00 == 100 * P


def make_engine():
    h = be.EngineHarness()
    h.add_symbol(SYM)        # must precede start()
    h.start()
    return h


def test_module_constants():
    assert be.PRICE_PRECISION == 10000


def test_resting_then_cross_produces_trade():
    h = make_engine()

    # Resting sell (maker) at 100.00, qty 50.
    ok, reason, _seq = h.submit_order(
        SYM, 1, 10, be.Side.Sell, 100 * P, 50, be.OrderType.Limit, be.TimeInForce.GTC)
    assert ok, reason
    assert h.drain_trades(SYM) == []          # nothing crosses yet
    assert h.best_ask(SYM) == 100 * P
    assert h.best_bid(SYM) == 0               # empty side normalized to 0

    # Aggressive buy crossing at 100.00, qty 30 -> one trade for 30.
    ok, reason, _seq = h.submit_order(
        SYM, 2, 20, be.Side.Buy, 100 * P, 30, be.OrderType.Limit, be.TimeInForce.GTC)
    assert ok, reason

    trades = h.drain_trades(SYM)
    assert len(trades) == 1
    t = trades[0]
    assert t.price == 100 * P
    assert t.quantity == 30
    assert t.buyer_id == 20
    assert t.seller_id == 10

    # 20 of the original sell remains resting.
    assert h.best_ask(SYM) == 100 * P
    snap = h.snapshot(SYM)
    assert snap.asks and snap.asks[0].total_quantity == 20
    assert h.drain_trades(SYM) == []          # drain is destructive


def test_ioc_fills_available_and_drops_remainder():
    h = make_engine()
    h.submit_order(SYM, 1, 10, be.Side.Sell, 101 * P, 20,
                   be.OrderType.Limit, be.TimeInForce.GTC)

    # IOC buy for 50 at 101: fills the 20 available, remainder does NOT rest.
    ok, reason, _seq = h.submit_order(
        SYM, 2, 20, be.Side.Buy, 101 * P, 50, be.OrderType.IOC, be.TimeInForce.GTC)
    assert ok, reason

    trades = h.drain_trades(SYM)
    assert sum(t.quantity for t in trades) == 20
    assert h.best_ask(SYM) == 0               # resting sell fully consumed
    assert h.best_bid(SYM) == 0               # IOC remainder not booked


def test_cancel_removes_quote():
    h = make_engine()
    ok, _reason, _seq = h.submit_order(
        SYM, 1, 10, be.Side.Buy, 99 * P, 40, be.OrderType.Limit, be.TimeInForce.GTC)
    assert ok
    assert h.best_bid(SYM) == 99 * P

    ok, reason, _seq = h.submit_cancel(SYM, 1)
    assert ok, reason
    assert h.best_bid(SYM) == 0


def test_add_symbol_after_start_raises():
    h = be.EngineHarness()
    h.add_symbol(SYM)
    h.start()
    raised = False
    try:
        h.add_symbol(2)
    except Exception:
        raised = True
    assert raised, "add_symbol after start() must raise"
