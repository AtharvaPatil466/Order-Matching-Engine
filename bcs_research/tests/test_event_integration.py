"""End to end: every wave-1 piece of the event model in one run on the real engine.

The lanes each tested their part in isolation (the race against a hand-made
path, the paths and accounting against synthetic data, the traders against a
fake book). This pins that they compose: a jump-diffusion path drives a slow
maker, two fast snipers, a reservation-value noise population and a slow
informed investor through EventScheduler, and the fee and speed accounting
closes on the trades that run produces.
"""
import sys
from pathlib import Path

_R = Path(__file__).resolve().parents[1]
for _d in ("build", "simulation", "agents", "metrics"):
    if str(_R / _d) not in sys.path:
        sys.path.insert(0, str(_R / _d))

import pytest  # noqa: E402

import bcs_engine as be  # noqa: E402
from accounting import TAKER_HEAVY, build_ledger, decompose  # noqa: E402
from event_maker import EventMaker  # noqa: E402
from event_noise import ReservationNoiseTrader, SlowInformedInvestor  # noqa: E402
from event_scheduler import EventScheduler  # noqa: E402
from event_sniper import EventSniper  # noqa: E402
from fundamental_paths import JumpDiffusionPath  # noqa: E402

P = be.PRICE_PRECISION
V0 = 100 * P
HALF_SPREAD = 200
MAKER, SNIPER_A, SNIPER_B, NOISE, INFORMED = 1, 2, 3, 4, 5
CLASSES = {MAKER: "maker", SNIPER_A: "sniper", SNIPER_B: "sniper",
           NOISE: "noise", INFORMED: "informed"}
SPEED_COSTS = {SNIPER_A: 3.0, SNIPER_B: 3.0}


def _run(seed):
    path = JumpDiffusionPath(v0=V0, sigma_step=20, update_interval_ns=10_000_000,
                             duration_ns=60_000_000_000, seed=seed,
                             jump_intensity_per_s=0.5, jump_scale=1_000)
    agents = [
        EventMaker(MAKER, HALF_SPREAD, qty=10, md_latency_ns=50_000, order_latency_ns=50_000),
        EventSniper(SNIPER_A, qty=10, threshold=HALF_SPREAD, md_latency_ns=5_000,
                    order_latency_ns=5_000),
        EventSniper(SNIPER_B, qty=10, threshold=HALF_SPREAD, md_latency_ns=5_000,
                    order_latency_ns=5_000),
        ReservationNoiseTrader(NOISE, arrival_rate_per_s=5.0, eps_scale=HALF_SPREAD,
                               v0=V0, qty=2, seed=seed),
        SlowInformedInvestor(INFORMED, path, info_lag_ns=2_000_000_000,
                             decision_interval_ns=5_000_000_000, parent_qty=6,
                             n_children=3, child_interval_ns=500_000_000,
                             min_edge=HALF_SPREAD, seed=seed),
    ]
    h = be.EngineHarness()
    h.add_symbol(1)
    h.start(disable_circuit_breaker=True)
    return EventScheduler(h, 1, seed=seed).run(agents, path)


@pytest.fixture(scope="module")
def result():
    return _run(seed=7)


def test_every_participant_class_trades_in_one_run(result):
    traded = {tr.buyer_id for tr in result.trades} | {tr.seller_id for tr in result.trades}
    assert {MAKER, SNIPER_A, SNIPER_B, NOISE} <= traded


def test_noise_participation_is_endogenous_not_all_or_nothing(result):
    noise = next(a for a in result.agents if a.participant_id == NOISE)
    assert 0 < noise.crossings < noise.arrivals


def test_faster_snipers_win_most_races_against_the_slower_maker(result):
    contested = [r for r in result.races if r.arrivals]
    sniped = [r for r in contested
              if r.arrivals[0].participant_id != MAKER
              and r.arrivals[0].outcome in ("filled", "partial")]
    assert len(contested) >= 10
    assert len(sniped) / len(contested) > 0.5


def test_fee_and_speed_accounting_closes_on_a_real_run(result):
    ledger = build_ledger(result.trades, TAKER_HEAVY, SPEED_COSTS)
    mark_v = result.path.value_at(result.duration_ns - 1)
    w = decompose(ledger, CLASSES, result.last_mid, mark_v)
    scale = sum(tr.price * tr.quantity for tr in result.trades) / P
    for key in ("gross_residual_mid", "gross_residual_v",
                "transfer_residual_mid", "transfer_residual_v"):
        assert abs(w[key]) <= 1e-12 * scale, key
    assert w["exchange"]["revenue"] > 0
    assert w["speed_cost_total"] == pytest.approx(6.0)


def test_same_seed_reproduces_the_run_exactly(result):
    again = _run(seed=7)
    assert again.trades == result.trades
    assert again.races == result.races
