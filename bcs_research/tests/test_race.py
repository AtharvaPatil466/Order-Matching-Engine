"""The BCS race on the real engine: a maker's cancel against snipers' IOCs.

In Budish-Cramton-Shim the core event is a race. Public news makes the maker's
quote stale; the maker sends a cancel, every sniper an IOC at the stale price,
and the first message to reach the matching engine decides whether the maker
is picked off. With N equally fast participants each is first with
probability 1/N, so the maker escapes a jump with probability 1/N. These tests
check that EventScheduler + EventMaker + EventSniper produce exactly that.

Run as a script for the one-hour throughput benchmark (events per second, to
size later sweeps):
    .venv/bin/python tests/test_race.py
"""
import math
import random
import sys
import time
from pathlib import Path

_R = Path(__file__).resolve().parents[1]
for _d in ("build", "simulation", "agents", "tests"):
    if str(_R / _d) not in sys.path:
        sys.path.insert(0, str(_R / _d))

import pytest  # noqa: E402

import bcs_engine as be  # noqa: E402
from contracts import Order, SignalUpdate, TradeRecord, Wakeup  # noqa: E402
from event_maker import EventMaker  # noqa: E402
from event_sniper import EventSniper  # noqa: E402
from test_event_scheduler import V0, ListPath, run  # noqa: E402

MAKER = 1
HALF_SPREAD = 200          # 0.02 in price units
JUMP = 1_000               # 0.10: five half-spreads, so every jump is worth sniping
QTY = 10
JUMP_T = 1_000_000


def one_jump_path(jump):
    return ListPath([SignalUpdate(0, V0), SignalUpdate(JUMP_T, V0 + jump, is_jump=True)],
                    2 * JUMP_T)


def jump_path(n_jumps, seed, gap_ns=1_000_000):
    """V0 at t=0, then n_jumps jumps of +-JUMP with seeded signs, gap_ns apart:
    far enough apart that every race is over before the next jump."""
    rng, v, updates = random.Random(seed), V0, [SignalUpdate(0, V0)]
    for k in range(1, n_jumps + 1):
        v += JUMP if rng.random() < 0.5 else -JUMP
        updates.append(SignalUpdate(k * gap_ns, v, is_jump=True))
    return ListPath(updates, (n_jumps + 1) * gap_ns)


def maker_won(race):
    """True when no sniper's IOC took anything from the stale quote."""
    return not any(a.outcome in ("filled", "partial") for a in race.arrivals
                   if a.participant_id != MAKER)


def summary(race):
    return [(a.participant_id, a.kind, a.outcome) for a in race.arrivals]


def test_faster_maker_cancels_first_and_the_snipe_misses():
    # Arrange: the sniper sees the stale quote first (+1.5 us vs the maker's
    # +1.0 us observation but before its cancel lands at +2.0 us), yet its IOC
    # lands at +2.5 us — after the cancel.
    maker = EventMaker(MAKER, HALF_SPREAD, QTY, md_latency_ns=1_000, order_latency_ns=1_000)
    sniper = EventSniper(2, QTY, md_latency_ns=1_500, order_latency_ns=1_000)

    # Act
    res = run([maker, sniper], one_jump_path(+JUMP))

    # Assert
    race, = res.races
    assert summary(race) == [(MAKER, "cancel", "cancelled"), (MAKER, "cancel", "cancelled"),
                             (MAKER, "limit", "rested"), (MAKER, "limit", "rested"),
                             (2, "ioc", "missed")]
    assert race.arrivals[0].arrival_ns == JUMP_T + 2_000
    assert race.arrivals[-1].arrival_ns == JUMP_T + 2_500
    assert res.trades == []


def test_slower_maker_is_sniped_at_its_stale_price():
    # Arrange: the maker's cancel lands at +6 us, the sniper's IOC at +2.5 us.
    maker = EventMaker(MAKER, HALF_SPREAD, QTY, md_latency_ns=5_000, order_latency_ns=1_000)
    sniper = EventSniper(2, QTY, md_latency_ns=1_500, order_latency_ns=1_000)

    # Act
    res = run([maker, sniper], one_jump_path(+JUMP))

    # Assert: the IOC lifts the ask quoted around the OLD value; the maker's
    # cancel of that ask arrives too late.
    race, = res.races
    assert summary(race) == [(2, "ioc", "filled"), (MAKER, "cancel", "cancelled"),
                             (MAKER, "cancel", "too_late"), (MAKER, "limit", "rested"),
                             (MAKER, "limit", "rested")]
    stale_ask = V0 + HALF_SPREAD
    assert res.trades == [TradeRecord(JUMP_T + 2_500, stale_ask, QTY, 2, MAKER, True)]


@pytest.mark.parametrize("n_agents", [2, 4])
@pytest.mark.parametrize("jitter_ns", [0, 1_000])
def test_equal_latency_maker_wins_the_race_with_probability_one_over_n(n_agents, jitter_ns):
    # Arrange: one maker, n-1 snipers, identical latencies. jitter 0 makes
    # every race an exact tie (decided by the seeded permutation); jitter 1 us
    # decides it by i.i.d. arrival noise. Either way the maker should be first
    # in 1/N of races. Tolerance: 4 binomial standard errors over 2,000 races.
    n_jumps = 2_000
    agents = [EventMaker(MAKER, HALF_SPREAD, QTY, md_latency_ns=1_000, order_latency_ns=1_000)]
    agents += [EventSniper(MAKER + i, QTY, md_latency_ns=1_000, order_latency_ns=1_000)
               for i in range(1, n_agents)]
    jitter = {a.participant_id: jitter_ns for a in agents} if jitter_ns else None

    # Act
    res = run(agents, jump_path(n_jumps, seed=1), seed=2, race_jitter_ns=jitter)

    # Assert: every race was contested by everyone and decided by who was first.
    assert len(res.races) == n_jumps
    assert all(len({a.participant_id for a in r.arrivals}) == n_agents for r in res.races)
    assert all(maker_won(r) == (r.arrivals[0].participant_id == MAKER) for r in res.races)
    p = 1 / n_agents
    win_rate = sum(maker_won(r) for r in res.races) / n_jumps
    assert abs(win_rate - p) < 4 * math.sqrt(p * (1 - p) / n_jumps)


def test_same_seeds_reproduce_identical_trades_and_race_log_and_a_new_seed_does_not():
    def once(seed):
        agents = [EventMaker(MAKER, HALF_SPREAD, QTY, md_latency_ns=1_000, order_latency_ns=1_000)]
        agents += [EventSniper(MAKER + i, QTY, md_latency_ns=1_000, order_latency_ns=1_000)
                   for i in (1, 2, 3)]
        res = run(agents, jump_path(200, seed=3), seed=seed,
                  race_jitter_ns={a.participant_id: 300 for a in agents})
        return res.trades, res.races

    assert once(7) == once(7)
    assert once(7)[1] != once(8)[1]


class PoissonTaker:
    """Uninformed taker: IOCs at the touch on a random side, Poisson arrivals."""

    def __init__(self, participant_id, rate_per_s, qty, seed, md_latency_ns=0, order_latency_ns=0):
        self.participant_id = participant_id
        self.md_latency_ns = md_latency_ns
        self.order_latency_ns = order_latency_ns
        self.rate_per_s = rate_per_s
        self.qty = qty
        self._rng = random.Random(seed)
        self._oid = participant_id * 1_000_000_000

    def _next_wakeup(self, t_ns):
        return Wakeup(t_ns + int(self._rng.expovariate(self.rate_per_s) * 1e9))

    def on_start(self, t_ns, view):
        return [self._next_wakeup(t_ns)]

    def on_wakeup(self, t_ns, tag, view):
        actions = [self._next_wakeup(t_ns)]
        buy = self._rng.random() < 0.5
        price = view.best_ask() if buy else view.best_bid()
        if price:
            self._oid += 1
            side = be.Side.Buy if buy else be.Side.Sell
            actions.append(Order(self._oid, side, price, self.qty, be.OrderType.IOC))
        return actions

    def on_signal(self, t_ns, update, view):
        return []

    def on_fill(self, t_ns, fill, view):
        return []


def market_path(duration_ns, seed, every_ns=10_000_000, jump_prob=0.02):
    """Updates every 10 ms: small Gaussian moves, and with probability
    jump_prob a jump of three half-spreads instead (is_jump)."""
    rng, v, updates = random.Random(seed), float(V0), []
    for t in range(0, duration_ns, every_ns):
        is_jump = rng.random() < jump_prob
        v += (rng.choice((-3, 3)) * HALF_SPREAD) if is_jump else rng.gauss(0, HALF_SPREAD / 10)
        updates.append(SignalUpdate(t, v, is_jump))
    return ListPath(updates, duration_ns)


def market_agents(seed, maker_latency_ns=200_000, sniper_latency_ns=20_000):
    """One maker, two snipers and a 16/s Poisson taker (the throughput scenario)."""
    return [
        EventMaker(MAKER, HALF_SPREAD, QTY, md_latency_ns=maker_latency_ns,
                   order_latency_ns=maker_latency_ns),
        EventSniper(2, QTY, md_latency_ns=sniper_latency_ns, order_latency_ns=sniper_latency_ns),
        EventSniper(3, QTY, md_latency_ns=sniper_latency_ns, order_latency_ns=sniper_latency_ns),
        PoissonTaker(4, 16.0, 1, seed, md_latency_ns=1_000_000, order_latency_ns=1_000_000),
    ]


def test_without_jumps_at_zero_latency_nothing_is_sniped():
    # Arrange: a zero-latency market whose updates are all small diffusive
    # moves (sd a tenth of the half-spread, never a jump). The requoted maker
    # is never stale by more than the snipers' zero threshold.
    path = market_path(30 * 10**9, seed=4, jump_prob=0.0)
    agents = market_agents(seed=4, maker_latency_ns=0, sniper_latency_ns=0)

    # Act
    res = run(agents, path)

    # Assert: the taker still trades with the maker; no sniper ever does.
    assert res.races == ()
    assert len(res.trades) > 100
    assert not any({tr.buyer_id, tr.seller_id} & {2, 3} for tr in res.trades)


def test_market_scenario_snipers_pick_off_the_slower_maker_on_jumps():
    # Arrange: the throughput scenario, 60 s: snipers 10x faster than the maker.
    res = run(market_agents(seed=5), market_path(60 * 10**9, seed=5), seed=5,
              race_jitter_ns={2: 5_000, 3: 5_000})

    # Assert: snipers win the races they contest, the taker trades throughout.
    contested = [r for r in res.races if any(a.kind == "ioc" for a in r.arrivals)]
    assert contested and not any(maker_won(r) for r in contested)
    traders = {pid for tr in res.trades for pid in (tr.buyer_id, tr.seller_id)}
    assert traders == {MAKER, 2, 3, 4}


def benchmark(hours=1.0, seed=0):
    duration = int(hours * 3600 * 10**9)
    path, agents = market_path(duration, seed), market_agents(seed)
    t0 = time.perf_counter()
    res = run(agents, path, seed=seed, race_jitter_ns={2: 5_000, 3: 5_000})
    wall = time.perf_counter() - t0
    print(f"{hours:g} h simulated: {res.n_events:,} events, {len(res.trades):,} trades, "
          f"{len(res.races):,} jump races, {len(res.snapshots):,} snapshots in {wall:.1f} s "
          f"-> {res.n_events / wall:,.0f} events/s, {hours * 3600 / wall:.0f}x real time")


if __name__ == "__main__":
    benchmark(float(sys.argv[1]) if len(sys.argv) > 1 else 1.0)
