"""Tests for the BCS (2015) closed forms the replication compares the simulator against.

The checks are the ones a wrong transcription of the paper would fail: the
Laplace closed form against the general integral solver, a second jump law
with its own closed form (uniform jumps), the limiting cases, the comparative
statics BCS state (Prop 5), the zero-sum and free-entry identities (equations
1-3, 7), and dimensional scaling.
"""
import math
import pathlib
import sys

_R = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(_R / "analysis"))

import numpy as np
import pytest
from scipy import stats

import bcs_theory as bt


@pytest.mark.parametrize("lam_invest,lam_jump,b", [
    (1.0, 1.0, 1000.0), (10.0, 2.5, 1000.0), (10.0, 40.0, 1000.0), (3.0, 0.01, 7.5),
])
def test_laplace_closed_form_matches_general_solver(lam_invest, lam_jump, b):
    closed = bt.laplace_half_spread(lam_invest, lam_jump, b)
    numeric = bt.half_spread(lam_invest, lam_jump, bt.laplace_jump(b))
    assert numeric == pytest.approx(closed, rel=1e-9)


def test_laplace_half_spread_solves_bcs_equation_3():
    lam_invest, lam_jump, b = 10.0, 10.0, 1000.0
    h = bt.laplace_half_spread(lam_invest, lam_jump, b)
    lhs = lam_invest * h
    rhs = lam_jump * math.exp(-h / b) * b   # Pr(J > h) * E[J - h | J > h]
    assert lhs == pytest.approx(rhs, rel=1e-12)
    assert h / b == pytest.approx(0.5671432904, rel=1e-9)   # W(1), the omega constant


def test_general_solver_matches_uniform_jump_closed_form():
    # J ~ U[0, a]: prize(h) = (a - h)^2 / (2a), so equation (3) is a quadratic.
    lam_invest, lam_jump, a = 4.0, 3.0, 50.0
    k = lam_invest * 2 * a / lam_jump
    expected = ((2 * a + k) - math.sqrt((2 * a + k) ** 2 - 4 * a * a)) / 2
    assert bt.half_spread(lam_invest, lam_jump, stats.uniform(0, a)) == pytest.approx(expected, rel=1e-8)


def test_sniping_prize_of_laplace_is_b_times_snipe_probability():
    b, h = 200.0, 130.0
    assert bt.sniping_prize(h, bt.laplace_jump(b)) == pytest.approx(b * math.exp(-h / b), rel=1e-9)


def test_no_jumps_means_zero_spread():
    assert bt.half_spread(5.0, 0.0, bt.laplace_jump(100.0)) == 0.0
    assert bt.laplace_half_spread(5.0, 0.0, 100.0) == 0.0


def test_lone_firm_never_loses_a_race_so_its_breakeven_spread_is_zero():
    assert bt.race_loss_probability(1) == 0.0
    eq = bt.laplace_equilibrium(1.0, 1.0, 1000.0, n_firms=1)
    assert eq["lp_breakeven_half_spread"] == 0.0
    assert eq["lp_loss_per_jump"] == 0.0


def test_lp_breakeven_spread_rises_to_bcs_spread_as_n_grows():
    spreads = [bt.laplace_equilibrium(1.0, 1.0, 1000.0, n)["lp_breakeven_half_spread"]
               for n in (2, 3, 5, 10, 1000)]
    h_star = bt.laplace_half_spread(1.0, 1.0, 1000.0)
    assert all(a < b for a, b in zip(spreads, spreads[1:]))
    assert spreads[-1] == pytest.approx(h_star, rel=1e-3)
    assert spreads[-1] < h_star


def test_equilibrium_spread_does_not_depend_on_n():
    halves = {bt.laplace_equilibrium(10.0, 10.0, 1000.0, n)["half_spread"] for n in (2, 3, 5, 10)}
    assert len(halves) == 1


@pytest.mark.parametrize("n", [2, 3, 5, 10])
def test_race_loss_probability_is_n_minus_one_over_n(n):
    assert bt.race_loss_probability(n) == pytest.approx((n - 1) / n)


def test_race_win_probability_reduces_to_one_over_n_with_no_lag():
    assert bt.race_win_probability(0.0, 1000.0, n_snipers=4) == pytest.approx(1 / 5)
    assert bt.race_win_probability(0.0, 0.0, n_snipers=4) == pytest.approx(1 / 5)
    assert bt.race_win_probability(1.0, 0.0, n_snipers=4) == 0.0
    assert bt.race_win_probability(1000.0, 1000.0, n_snipers=1) == 0.0


def test_race_win_probability_matches_monte_carlo_jitter_race():
    rng = np.random.default_rng(0)
    lag, eps, k, draws = 300.0, 1000.0, 3, 400_000
    lp = lag + rng.uniform(0, eps, draws)
    snipers = rng.uniform(0, eps, (draws, k)).min(axis=1)
    empirical = float(np.mean(lp < snipers))
    se = math.sqrt(empirical * (1 - empirical) / draws)
    assert abs(empirical - bt.race_win_probability(lag, eps, k)) < 4 * se


def test_spread_increases_in_jump_intensity_and_scale_and_falls_in_investor_rate():
    jump = bt.laplace_jump(100.0)
    by_jump_rate = [bt.half_spread(1.0, lj, jump) for lj in (0.1, 0.5, 1.0, 5.0)]
    by_scale = [bt.laplace_half_spread(1.0, 1.0, b) for b in (10.0, 100.0, 1000.0)]
    by_invest = [bt.laplace_half_spread(li, 1.0, 100.0) for li in (0.5, 1.0, 5.0)]
    assert all(a < b for a, b in zip(by_jump_rate, by_jump_rate[1:]))
    assert all(a < b for a, b in zip(by_scale, by_scale[1:]))
    assert all(a > b for a, b in zip(by_invest, by_invest[1:]))


def test_half_spread_scales_linearly_with_jump_scale_and_is_invariant_to_time_unit():
    base = bt.half_spread(2.0, 3.0, bt.laplace_jump(50.0))
    assert bt.half_spread(2.0, 3.0, bt.laplace_jump(500.0)) == pytest.approx(10 * base, rel=1e-8)
    # Same rates per millisecond instead of per second: h is a price, so unchanged.
    assert bt.half_spread(2e-3, 3e-3, bt.laplace_jump(50.0)) == pytest.approx(base, rel=1e-8)


def test_trade_among_firms_is_zero_sum_and_roles_are_indifferent():
    n = 4
    eq = bt.laplace_equilibrium(10.0, 5.0, 1000.0, n)
    assert eq["lp_loss_per_jump"] == pytest.approx((n - 1) * eq["sniper_profit_per_jump_each"])
    assert eq["lp_profit_rate"] == pytest.approx(eq["sniper_profit_rate_each"], rel=1e-9)
    assert eq["rent_rate"] == pytest.approx(10.0 * eq["half_spread"], rel=1e-12)


def test_free_entry_spends_the_whole_rent_on_speed():
    eq = bt.laplace_equilibrium(10.0, 10.0, 1000.0, 5, c_speed=1000.0)
    assert eq["waste_rate"] == pytest.approx(eq["rent_rate"])        # equation (7)
    assert eq["n_star"] == pytest.approx(eq["rent_rate"] / 1000.0)
    assert eq["n_star_integer"] == math.floor(eq["n_star"])
    assert eq["waste_rate_integer"] <= eq["waste_rate"]


def test_free_entry_refuses_a_speed_cost_that_leaves_fewer_than_two_firms():
    with pytest.raises(ValueError, match="footnote 26"):
        bt.free_entry_n(rent_rate=100.0, c_speed=60.0)


def test_depth_spreads_widen_with_level_and_first_level_is_eq3_with_all_investors():
    jump = bt.laplace_jump(100.0)
    spreads = bt.depth_half_spreads(1.0, 1.0, jump, [0.5, 0.3, 0.2])
    assert all(a < b for a, b in zip(spreads, spreads[1:]))           # Prop 2(ii)
    assert spreads[0] == pytest.approx(bt.half_spread(1.0, 1.0, jump), rel=1e-9)
    assert spreads[2] == pytest.approx(bt.laplace_half_spread(0.2, 1.0, 100.0), rel=1e-8)


def test_mean_preserving_spread_of_jumps_raises_the_prize():
    # Prop 5(ii). J uniform on [m - w, m + w]: a wider w is a mean-preserving spread.
    narrow = bt.sniping_prize(40.0, stats.uniform(40.0, 20.0))
    wide = bt.sniping_prize(40.0, stats.uniform(10.0, 80.0))
    assert narrow == pytest.approx(10.0)                    # all mass above h: E[J] - h
    assert wide == pytest.approx(50.0 ** 2 / 160.0)         # (hi - h)^2 / (2 * width)
    assert wide > narrow


@pytest.mark.parametrize("scale", [1e-3, 1.0, 1e5, 1e9])
def test_sniping_prize_is_accurate_at_any_price_scale(scale):
    # Engine ticks put prices near 1e6; the quadrature must not care.
    assert bt.sniping_prize(0.3 * scale, bt.laplace_jump(scale)) == pytest.approx(
        scale * math.exp(-0.3), rel=1e-9)
    far = stats.uniform(scale * 100.0, scale)               # bounded support far from 0
    assert bt.sniping_prize(0.0, far) == pytest.approx(far.mean(), rel=1e-9)


def test_fba_with_no_exposure_has_zero_spread_and_full_exposure_recovers_the_clob():
    jump = bt.laplace_jump(100.0)
    assert bt.fba_half_spread(1.0, 1.0, jump, 0.0) == 0.0                # Prop 7(i)
    assert bt.fba_half_spread(1.0, 1.0, jump, 1.0) == pytest.approx(bt.half_spread(1.0, 1.0, jump))


def test_fba_jitter_exposure_tends_to_footnote_34_as_snipers_multiply():
    eps, tau = 10_000, 1_000_000
    assert bt.fba_jitter_exposure(eps, tau) == pytest.approx(eps / (2 * tau))
    assert bt.fba_jitter_exposure(eps, tau, n_firms=1) == 0.0
    assert bt.fba_jitter_exposure(eps, tau, n_firms=2) == pytest.approx(eps / tau / 6)
    assert bt.fba_jitter_exposure(eps, tau, n_firms=10_000) == pytest.approx(eps / (2 * tau), rel=1e-3)


def test_fba_slow_lp_exposure_is_delta_over_tau_capped_at_one():
    assert bt.fba_slow_lp_exposure(100.0, 100_000.0) == pytest.approx(1e-3)   # BCS's 1/1000 example
    assert bt.fba_slow_lp_exposure(5.0, 1.0) == 1.0


def test_fba_max_slow_depth_grows_linearly_in_batch_interval():
    q1 = bt.fba_max_slow_depth(c_speed=10.0, tau=1.0, delta=0.01, lam_jump=2.0, mean_jump=5.0)
    q2 = bt.fba_max_slow_depth(c_speed=10.0, tau=2.0, delta=0.01, lam_jump=2.0, mean_jump=5.0)
    assert q1 == pytest.approx(10.0 * 1.0 / (0.01 * 2.0 * 5.0))       # equation (8) at equality
    assert q2 == pytest.approx(2 * q1)


@pytest.mark.parametrize("bad", [
    lambda: bt.half_spread(0.0, 1.0, bt.laplace_jump(1.0)),
    lambda: bt.half_spread(1.0, -1.0, bt.laplace_jump(1.0)),
    lambda: bt.sniping_prize(-1.0, bt.laplace_jump(1.0)),
    lambda: bt.race_loss_probability(0),
    lambda: bt.depth_half_spreads(1.0, 1.0, bt.laplace_jump(1.0), [0.5, 0.4]),
    lambda: bt.fba_jitter_exposure(2.0, 1.0),
    lambda: bt.fba_half_spread(1.0, 1.0, bt.laplace_jump(1.0), 1.5),
])
def test_invalid_inputs_are_rejected(bad):
    with pytest.raises(ValueError):
        bad()
