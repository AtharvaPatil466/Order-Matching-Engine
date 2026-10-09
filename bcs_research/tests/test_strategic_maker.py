"""StrategicMaker on the real engine: nesting, one test per policy dimension,
live-order bookkeeping, and a full-population smoke run.

The nesting test matters most. Wave 3 compares best responses against the
wave-1 race results; that comparison only means something if the default
policy IS EventMaker, so it checks trades, race logs, snapshots and the event
count are identical on the same seeds.

Run as a script for the per-event cost against EventMaker:
    .venv/bin/python tests/test_strategic_maker.py [hours]
"""
import itertools
import math
import random
import sys
import time
from collections import Counter
from pathlib import Path

_R = Path(__file__).resolve().parents[1]
for _d in ("build", "simulation", "agents", "metrics", "tests"):
    if str(_R / _d) not in sys.path:
        sys.path.insert(0, str(_R / _d))

import pytest  # noqa: E402

import bcs_engine as be  # noqa: E402
from accounting import TAKER_HEAVY, build_ledger, decompose  # noqa: E402
from contracts import Order, SignalUpdate, TradeRecord, Wakeup  # noqa: E402
from event_maker import EventMaker  # noqa: E402
from event_noise import ReservationNoiseTrader, SlowInformedInvestor  # noqa: E402
from event_scheduler import EventScheduler  # noqa: E402
from event_sniper import EventSniper  # noqa: E402
from fundamental_paths import JumpDiffusionPath  # noqa: E402
from strategic_maker import MakerPolicy, StrategicMaker  # noqa: E402
from test_event_scheduler import SYM, V0, ListPath, Scripted, run  # noqa: E402
from test_race import PoissonTaker, market_path  # noqa: E402

BUY, SELL, IOC = be.Side.Buy, be.Side.Sell, be.OrderType.IOC
MAKER, SNIPER_A, SNIPER_B, NOISE, INFORMED = 1, 2, 3, 4, 5
HALF = 200
QTY = 10
JUMP = 1_000
JUMP_T = 1_000_000
POLICY = MakerPolicy(half_spread=HALF, qty=QTY)


def run_on_engine(agents, path, seed=0, **kwargs):
    """run() that also hands back the harness, to read the final book."""
    h = be.EngineHarness()
    h.add_symbol(SYM)
    h.start(disable_circuit_breaker=True)
    return EventScheduler(h, SYM, seed=seed, **kwargs).run(agents, path), h


def engine_book(h):
    snap = h.snapshot(SYM, 100)
    return ({(BUY, lv.price): lv.total_quantity for lv in snap.bids}
            | {(SELL, lv.price): lv.total_quantity for lv in snap.asks})


def believed_book(maker):
    book = Counter()
    for q in maker.live_orders.values():
        book[(q.side, q.price)] += q.qty
    return dict(book)


def one_jump_path(jump=JUMP):
    return ListPath([SignalUpdate(0, V0), SignalUpdate(JUMP_T, V0 + jump, is_jump=True)],
                    2 * JUMP_T)


def summary(race):
    return [(a.participant_id, a.kind, a.outcome) for a in race.arrivals]


# --- nesting ---------------------------------------------------------------

def race_market(maker, seed):
    """test_race's throughput scenario: 60 s, snipers 10x faster, a Poisson taker."""
    agents = [maker,
              EventSniper(SNIPER_A, QTY, md_latency_ns=20_000, order_latency_ns=20_000),
              EventSniper(SNIPER_B, QTY, md_latency_ns=20_000, order_latency_ns=20_000),
              PoissonTaker(NOISE, 16.0, 1, seed, md_latency_ns=1_000_000,
                           order_latency_ns=1_000_000)]
    return run(agents, market_path(60 * 10**9, seed), seed=seed,
               race_jitter_ns={SNIPER_A: 5_000, SNIPER_B: 5_000})


def jump_diffusion(seed):
    return JumpDiffusionPath(v0=V0, sigma_step=20, update_interval_ns=10_000_000,
                             duration_ns=60_000_000_000, seed=seed,
                             jump_intensity_per_s=0.5, jump_scale=1_000)


def full_population(maker, path, seed):
    """test_event_integration's population: two fast snipers, a reservation-value
    noise population and a slow informed investor around one slow maker."""
    agents = [maker,
              EventSniper(SNIPER_A, qty=QTY, threshold=HALF, md_latency_ns=5_000,
                          order_latency_ns=5_000),
              EventSniper(SNIPER_B, qty=QTY, threshold=HALF, md_latency_ns=5_000,
                          order_latency_ns=5_000),
              ReservationNoiseTrader(NOISE, arrival_rate_per_s=5.0, eps_scale=HALF,
                                     v0=V0, qty=2, seed=seed),
              SlowInformedInvestor(INFORMED, path, info_lag_ns=2_000_000_000,
                                   decision_interval_ns=5_000_000_000, parent_qty=6,
                                   n_children=3, child_interval_ns=500_000_000,
                                   min_edge=HALF, seed=seed)]
    return run(agents, path, seed=seed)


def _scenario(name, seed, make_maker):
    if name == "race_market":
        return race_market(make_maker(200_000, 200_000), seed)
    return full_population(make_maker(50_000, 50_000), jump_diffusion(seed), seed)


@pytest.mark.parametrize("name,seed", [("race_market", 5), ("race_market", 11),
                                       ("full_population", 7), ("full_population", 8)])
def test_default_policy_reproduces_event_maker_exactly(name, seed):
    # Arrange
    def event_maker(md, order):
        return EventMaker(MAKER, HALF, QTY, md_latency_ns=md, order_latency_ns=order)

    def strategic(md, order):
        return StrategicMaker(MAKER, POLICY, md_latency_ns=md, order_latency_ns=order)

    # Act
    base = _scenario(name, seed, event_maker)
    nested = _scenario(name, seed, strategic)

    # Assert: the scenario is not trivial, and nothing differs.
    assert len(base.trades) > 50 and any(r.arrivals for r in base.races)
    assert nested.trades == base.trades
    assert nested.races == base.races
    assert nested.snapshots == base.snapshots
    assert nested.n_events == base.n_events


# --- one test per dimension ------------------------------------------------

def test_no_quote_policy_sends_nothing_and_never_trades():
    # Arrange: a sniper that would lift any stale quote.
    maker = StrategicMaker(MAKER, MakerPolicy(quote=False), md_latency_ns=1_000,
                           order_latency_ns=1_000)
    sniper = EventSniper(SNIPER_A, QTY, md_latency_ns=1_500, order_latency_ns=1_000)

    # Act
    res = run([maker, sniper], one_jump_path(), snapshot_every_ns=100_000)

    # Assert
    assert res.trades == []
    assert all(r.arrivals == () for r in res.races)
    assert {(s.best_bid, s.best_ask) for s in res.snapshots} == {(0, 0)}


def test_multi_level_quotes_rest_at_their_spacing_and_sizes():
    # Arrange
    policy = MakerPolicy(half_spread=HALF, qty=QTY, n_levels=3, level_spacing=50, level_qty=4)
    maker = StrategicMaker(MAKER, policy)

    # Act
    _, h = run_on_engine([maker], ListPath([SignalUpdate(0, V0)], 1_000))

    # Assert
    assert engine_book(h) == {(BUY, V0 - 200): 10, (BUY, V0 - 250): 4, (BUY, V0 - 300): 4,
                              (SELL, V0 + 200): 10, (SELL, V0 + 250): 4, (SELL, V0 + 300): 4}


def _sell_into_bid_then_requote(policy, path=None):
    """The maker quotes at t=0; a taker sells QTY into its bid at 100 us, making it
    long QTY; an unchanged update at 200 us makes it requote. Zero latency."""
    updates = [SignalUpdate(0, V0), SignalUpdate(200_000, V0)]
    path = path or ListPath(updates, 300_000)
    maker = StrategicMaker(MAKER, policy, path=path)
    taker = Scripted(NOISE, start=lambda t, _, v: [Wakeup(100_000)],
                     wakeup=lambda t, tag, v: [Order(41, SELL, v.best_bid(), QTY, IOC)])
    _, h = run_on_engine([maker, taker], path)
    return maker, h


GAMMA, SIGMA2, TAU_NS = 2.5e-4, 40_000.0, 1_000_000_000


@pytest.mark.parametrize("policy,shift,half", [
    # Avellaneda-Stoikov: r = s - q gamma sigma^2 tau; half-spread
    # ln(1 + gamma/kappa)/gamma + gamma sigma^2 tau / 2 with kappa = 1/HALF.
    (MakerPolicy(half_spread=HALF, qty=QTY, as_gamma=GAMMA, as_sigma2_per_s=SIGMA2,
                 as_horizon_ns=TAU_NS),
     QTY * GAMMA * SIGMA2 * 1.0, math.log1p(GAMMA * HALF) / GAMMA + GAMMA * SIGMA2 * 1.0 / 2),
    # Linear skew: centre moves by -skew per share; the spread is untouched.
    (MakerPolicy(half_spread=HALF, qty=QTY, inventory_skew=5.0), QTY * 5.0, HALF),
], ids=["avellaneda_stoikov", "linear_skew"])
def test_long_inventory_shifts_both_quotes_down(policy, shift, half):
    # Act
    maker, h = _sell_into_bid_then_requote(policy)

    # Assert: long QTY, both sides re-centred below V by exactly the shift.
    assert maker.inventory == QTY
    assert shift > 0
    assert engine_book(h) == {(BUY, round(V0 - shift - half)): QTY,
                              (SELL, round(V0 - shift + half)): QTY}


def test_inventory_limit_stops_quoting_the_side_that_adds_to_it():
    # Act
    maker, h = _sell_into_bid_then_requote(MakerPolicy(half_spread=HALF, qty=QTY,
                                                       inventory_limit=QTY))

    # Assert: at the limit long, only the ask (which reduces it) is quoted.
    assert maker.inventory == QTY
    assert engine_book(h) == {(SELL, V0 + HALF): QTY}


def test_a_fill_on_the_right_side_of_true_value_does_not_widen():
    # Arrange: buying at V0 - HALF while V = V0 is noise flow, not a pickoff.
    policy = MakerPolicy(half_spread=HALF, qty=QTY, adverse_sensitivity=0.5)

    # Act
    maker, h = _sell_into_bid_then_requote(policy)

    # Assert
    assert maker.adverse_fills == 0 and maker.spread_mult == 1.0
    assert engine_book(h) == {(BUY, V0 - HALF): QTY, (SELL, V0 + HALF): QTY}


def test_adverse_fill_widens_and_thins_then_decays_toward_base():
    # Arrange: the slow maker of test_race is sniped on the jump (it sold at
    # V0 + HALF while V = V0 + JUMP). Two calm updates follow.
    v1 = V0 + JUMP
    path = ListPath([SignalUpdate(0, V0), SignalUpdate(JUMP_T, v1, is_jump=True),
                     SignalUpdate(JUMP_T + 100_000, v1), SignalUpdate(JUMP_T + 200_000, v1)],
                    JUMP_T + 300_000)
    policy = MakerPolicy(half_spread=HALF, qty=QTY, adverse_sensitivity=0.5,
                         spread_decay=0.2, thin_liquidity=True)
    maker = StrategicMaker(MAKER, policy, path=path, md_latency_ns=5_000,
                           order_latency_ns=1_000)
    sniper = EventSniper(SNIPER_A, QTY, md_latency_ns=1_500, order_latency_ns=1_000)

    # Act
    res, h = run_on_engine([maker, sniper], path)

    # Assert: x1.5 on the pickoff, then x(1 - 0.2) of the excess per update:
    # 1.4, then 1.32. Half-spread 200 * 1.32 = 264; size round(10 / 1.32) = 8.
    assert res.trades == [TradeRecord(JUMP_T + 2_500, V0 + HALF, QTY, SNIPER_A, MAKER, True)]
    assert maker.adverse_fills == 1
    assert maker.spread_mult == pytest.approx(1.32)
    assert engine_book(h) == {(BUY, v1 - 264): 8, (SELL, v1 + 264): 8}


@pytest.mark.parametrize("fade_widen,during", [
    (None, (0, 0)),                                   # pull every quote
    (300.0, (V0 + JUMP - 500, V0 + JUMP + 500)),      # widen 200 -> 500
], ids=["pull", "widen"])
def test_fading_after_a_jump_lasts_exactly_fade_ns(fade_widen, during):
    # Arrange: no sniper, so no adverse fill extends the fade.
    fade_ns = 50_000
    policy = MakerPolicy(half_spread=HALF, qty=QTY, fade_ns=fade_ns, fade_widen=fade_widen)
    path = one_jump_path()
    maker = StrategicMaker(MAKER, policy, path=path, md_latency_ns=1_000,
                           order_latency_ns=1_000)

    # Act
    res = run([maker], path, snapshot_every_ns=1_000)

    # Assert: the jump is seen at +1 us, the fade's burst lands at +2 us, the
    # unfade wakeup fires at +51 us and its quotes land at +52 us.
    book = {s.t_ns: (s.best_bid, s.best_ask) for s in res.snapshots}
    faded = sorted(t for t, b in book.items() if t >= JUMP_T and b == during)
    assert book[JUMP_T + 1_000] == (V0 - HALF, V0 + HALF)
    assert faded == list(range(JUMP_T + 2_000, JUMP_T + 2_000 + fade_ns, 1_000))
    assert book[JUMP_T + 2_000 + fade_ns] == (V0 + JUMP - HALF, V0 + JUMP + HALF)
    race, = res.races
    expected_kinds = ["cancel", "cancel"] + (["limit", "limit"] if fade_widen else [])
    assert [a.kind for a in race.arrivals] == expected_kinds


def test_a_second_jump_while_fading_extends_the_fade_and_the_first_wakeup_does_nothing():
    # Arrange: two jumps 20 us apart inside one 50 us widened fade.
    fade_ns, j2 = 50_000, JUMP_T + 20_000
    path = ListPath([SignalUpdate(0, V0), SignalUpdate(JUMP_T, V0 + JUMP, is_jump=True),
                     SignalUpdate(j2, V0 + 2 * JUMP, is_jump=True)], 2 * JUMP_T)
    policy = MakerPolicy(half_spread=HALF, qty=QTY, fade_ns=fade_ns, fade_widen=300.0)
    maker = StrategicMaker(MAKER, policy, path=path, md_latency_ns=1_000,
                           order_latency_ns=1_000)

    # Act
    res = run([maker], path, snapshot_every_ns=1_000)

    # Assert: wide from the first fade's burst until the second fade ends.
    book = {s.t_ns: (s.best_bid, s.best_ask) for s in res.snapshots}
    v2 = V0 + 2 * JUMP
    assert book[j2 + 2_000 + fade_ns - 1_000] == (v2 - 500, v2 + 500)
    assert book[j2 + 2_000 + fade_ns] == (v2 - HALF, v2 + HALF)
    # Quote pairs: start, jump 1, jump 2, unfade. The superseded wakeup at
    # JUMP_T + 51 us would have been a fifth, identical, wide pair.
    first = MAKER * 1_000_000_000
    assert sorted(maker.live_orders) == [first + 7, first + 8]


def test_requote_threshold_above_the_jump_sends_no_cancel_so_the_maker_is_sniped():
    # Arrange: the FASTER maker of test_race, which wins this race when it
    # cancels on every update; a threshold above the jump stops it cancelling.
    policy = MakerPolicy(half_spread=HALF, qty=QTY, requote_threshold=2 * JUMP)
    maker = StrategicMaker(MAKER, policy, md_latency_ns=1_000, order_latency_ns=1_000)
    sniper = EventSniper(SNIPER_A, QTY, md_latency_ns=1_500, order_latency_ns=1_000)

    # Act
    res = run([maker, sniper], one_jump_path())

    # Assert
    race, = res.races
    assert summary(race) == [(SNIPER_A, "ioc", "filled")]
    assert res.trades == [TradeRecord(JUMP_T + 2_500, V0 + HALF, QTY, SNIPER_A, MAKER, True)]


@pytest.mark.parametrize("requote_after_fill,bid_after", [(True, V0 - HALF), (False, 0)])
def test_requote_after_fill_replaces_the_emptied_side(requote_after_fill, bid_after):
    # Arrange: one update only, so nothing but the fill can trigger a requote.
    path = ListPath([SignalUpdate(0, V0)], 300_000)
    policy = MakerPolicy(half_spread=HALF, qty=QTY, requote_after_fill=requote_after_fill)

    # Act
    maker, h = _sell_into_bid_then_requote(policy, path)

    # Assert
    assert maker.inventory == QTY
    assert h.best_bid(SYM) == bid_after
    assert h.best_ask(SYM) == V0 + HALF


def test_policy_validation_and_true_value_requirement():
    with pytest.raises(ValueError, match="qty"):
        MakerPolicy(qty=0)
    with pytest.raises(ValueError, match="kappa"):
        MakerPolicy(half_spread=0, as_gamma=1e-4)
    with pytest.raises(ValueError, match="path"):
        StrategicMaker(MAKER, MakerPolicy(adverse_sensitivity=0.1))


# --- live-order bookkeeping ------------------------------------------------

def mixed_path(n_jumps, seed, gap_ns=1_000_000):
    """Alternating updates gap_ns apart: a +-JUMP jump, then a calm update at
    the same value; a quiet gap at the end so every message lands in the run."""
    rng, v, updates = random.Random(seed), V0, []
    for k in range(n_jumps):
        v += JUMP if rng.random() < 0.5 else -JUMP
        updates.append(SignalUpdate((2 * k + 1) * gap_ns, v, is_jump=True))
        updates.append(SignalUpdate((2 * k + 2) * gap_ns, v))
    return ListPath(updates, (2 * n_jumps + 2) * gap_ns)


def test_live_order_bookkeeping_matches_the_engine_through_partials_and_too_late_cancels():
    # Arrange: a two-level maker that requotes after fills, sniped on jumps by
    # two jittered snipers of 4 and 7 lots (so a 10-lot is partly taken, or
    # emptied before the cancel lands), and a slow taker that takes 3 from the
    # fresh quotes after every calm update. The maker is jittered wider than
    # the gap between its own bursts, which only stays consistent because the
    # scheduler keeps each session FIFO (an overtaking cancel would orphan
    # the order it targets).
    ids = itertools.count(NOISE * 1_000_000_000 + 1)

    def calm_taker(t, u, view):
        if u.is_jump:
            return []
        oid = next(ids)
        buy = oid % 2 == 0
        price = view.best_ask() if buy else view.best_bid()
        return [Order(oid, BUY if buy else SELL, price, 3, IOC)] if price else []

    policy = MakerPolicy(half_spread=HALF, qty=QTY, n_levels=2, level_spacing=50,
                         level_qty=5, requote_after_fill=True)
    maker = StrategicMaker(MAKER, policy, md_latency_ns=5_000, order_latency_ns=1_000)
    agents = [maker,
              EventSniper(SNIPER_A, 4, md_latency_ns=1_500, order_latency_ns=1_000),
              EventSniper(SNIPER_B, 7, md_latency_ns=1_500, order_latency_ns=1_000),
              Scripted(NOISE, md=20_000, order=1_000, signal=calm_taker)]

    # Act
    res, h = run_on_engine(agents, mixed_path(150, seed=3), seed=3,
                           race_jitter_ns={MAKER: 6_000, SNIPER_A: 6_000, SNIPER_B: 6_000})

    # Assert: the run exercised every path...
    maker_outcomes = Counter(a.outcome for r in res.races for a in r.arrivals
                             if a.participant_id == MAKER and a.kind == "cancel")
    sniper_outcomes = Counter(a.outcome for r in res.races for a in r.arrivals
                              if a.participant_id != MAKER)
    assert maker_outcomes["too_late"] > 0 and maker_outcomes["cancelled"] > 0
    assert sniper_outcomes["partial"] > 0 and sniper_outcomes["filled"] > 0
    # ...and what the maker believes rests is exactly what rests, including the
    # 7-lot the last calm taker left behind.
    assert believed_book(maker) == engine_book(h)
    assert 7 in {q.qty for q in maker.live_orders.values()}
    net = sum(tr.quantity if tr.buyer_id == MAKER else -tr.quantity
              for tr in res.trades if MAKER in (tr.buyer_id, tr.seller_id))
    assert maker.inventory == net


# --- smoke: a rich policy in the full population ---------------------------

RICH = MakerPolicy(half_spread=HALF, qty=QTY, n_levels=2, level_spacing=100, level_qty=5,
                   as_gamma=GAMMA, as_sigma2_per_s=SIGMA2, inventory_skew=2.0,
                   inventory_limit=40, adverse_sensitivity=0.3, spread_decay=0.05,
                   fade_ns=2_000_000, requote_threshold=10.0, requote_after_fill=True)


def test_rich_policy_against_faster_snipers_and_noise_closes_the_accounting():
    # Arrange
    seed = 7
    path = jump_diffusion(seed)
    maker = StrategicMaker(MAKER, RICH, path=path, md_latency_ns=50_000,
                           order_latency_ns=50_000)

    # Act
    res = full_population(maker, path, seed)

    # Assert: everyone trades, the maker was picked off and reacted, and the
    # fee and speed identities still close on the trades.
    traded = {tr.buyer_id for tr in res.trades} | {tr.seller_id for tr in res.trades}
    assert {MAKER, SNIPER_A, SNIPER_B, NOISE} <= traded
    assert maker.adverse_fills > 0
    classes = {MAKER: "maker", SNIPER_A: "sniper", SNIPER_B: "sniper",
               NOISE: "noise", INFORMED: "informed"}
    ledger = build_ledger(res.trades, TAKER_HEAVY, {SNIPER_A: 3.0, SNIPER_B: 3.0})
    w = decompose(ledger, classes, res.last_mid, path.value_at(res.duration_ns - 1))
    scale = sum(tr.price * tr.quantity for tr in res.trades) / be.PRICE_PRECISION
    for key in ("gross_residual_mid", "gross_residual_v",
                "transfer_residual_mid", "transfer_residual_v"):
        assert abs(w[key]) <= 1e-12 * scale, key
    assert ledger.accounts[MAKER].inventory == maker.inventory


# --- per-event cost --------------------------------------------------------

class _Timed:
    """Forwards an agent's callbacks and accumulates the time spent in them."""

    def __init__(self, inner):
        self.inner, self.ns, self.calls = inner, 0, 0
        self.participant_id = inner.participant_id
        self.md_latency_ns = inner.md_latency_ns
        self.order_latency_ns = inner.order_latency_ns

    def _timed(self, fn, *args):
        t0 = time.perf_counter_ns()
        out = fn(*args)
        self.ns += time.perf_counter_ns() - t0
        self.calls += 1
        return out

    def on_start(self, *a):
        return self._timed(self.inner.on_start, *a)

    def on_signal(self, *a):
        return self._timed(self.inner.on_signal, *a)

    def on_wakeup(self, *a):
        return self._timed(self.inner.on_wakeup, *a)

    def on_fill(self, *a):
        return self._timed(self.inner.on_fill, *a)


def benchmark(hours=0.25, seed=0):
    duration = int(hours * 3600 * 10**9)
    path = JumpDiffusionPath(v0=V0, sigma_step=20, update_interval_ns=10_000_000,
                             duration_ns=duration, seed=seed,
                             jump_intensity_per_s=0.5, jump_scale=1_000)
    lat = dict(md_latency_ns=50_000, order_latency_ns=50_000)
    makers = {"EventMaker": lambda: EventMaker(MAKER, HALF, QTY, **lat),
              "StrategicMaker default": lambda: StrategicMaker(MAKER, POLICY, **lat),
              "StrategicMaker rich": lambda: StrategicMaker(MAKER, RICH, path=path, **lat)}
    for name, make in makers.items():
        maker = _Timed(make())
        t0 = time.perf_counter()
        res = full_population(maker, path, seed)
        wall = time.perf_counter() - t0
        print(f"{name:24s} {res.n_events:>9,} events {wall:6.2f} s "
              f"{wall / res.n_events * 1e6:5.2f} us/event | maker {maker.calls:,} calls "
              f"{maker.ns / maker.calls / 1e3:5.2f} us/call, {len(res.trades):,} trades")


if __name__ == "__main__":
    benchmark(float(sys.argv[1]) if len(sys.argv) > 1 else 0.25)
