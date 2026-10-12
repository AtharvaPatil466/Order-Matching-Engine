"""Experiment 10: free entry of snipers who pay for speed, and the welfare at N*.

The free-entry and tier routines are checked on synthetic profit tables whose
equilibria are known in closed form; the market pieces on short real runs:
per-sniper profit falls as snipers enter, the accounting identities close at
the equilibrium with the speed leg booked, and a run is reproducible, in or
out of the process pool.
"""
import sys
from pathlib import Path

_R = Path(__file__).resolve().parents[1]
for _d in ("build", "simulation", "agents", "metrics", "experiments"):
    if str(_R / _d) not in sys.path:
        sys.path.insert(0, str(_R / _d))

import numpy as np  # noqa: E402
import pytest  # noqa: E402

import exp10_speed_entry as x  # noqa: E402
from accounting import build_ledger, decompose  # noqa: E402

SHORT_S = 60.0
SEEDS = (0, 1, 2)


def _synthetic(rent, n_max, noise_sd, n_seeds=40, seed=0):
    """BCS with a fast maker: each of N snipers wins 1/(N+1) of the races."""
    rng = np.random.default_rng(seed)
    return {n: list(rent / (n + 1) + noise_sd * rng.standard_normal(n_seeds))
            for n in range(1, n_max + 1)}


def test_free_entry_finds_the_known_n_star_on_a_synthetic_profit_curve():
    # 1/(N+1) >= 0.15  <=>  N <= 5.67, so N* = 5.
    fe = x.free_entry(_synthetic(1.0, 12, noise_sd=1e-4), cost=0.15)
    assert (fe["n_star"], fe["n_star_lo"], fe["n_star_hi"]) == (5, 5, 5)
    assert fe["straddles"] == [] and not fe["censored"]


def test_free_entry_reports_a_range_when_the_profit_ci_straddles_the_cost():
    # Cost exactly at N = 4's mean profit (0.2): its CI must straddle.
    fe = x.free_entry(_synthetic(1.0, 12, noise_sd=0.05), cost=0.2)
    assert 4 in fe["straddles"]
    assert fe["n_star_lo"] <= fe["n_star"] <= fe["n_star_hi"]
    assert fe["n_star_lo"] < fe["n_star_hi"]


def test_free_entry_edges_no_entrant_and_a_censored_grid():
    table = _synthetic(1.0, 6, noise_sd=1e-4)
    assert x.free_entry(table, cost=0.9)["n_star"] == 0
    top = x.free_entry(table, cost=0.01)
    assert top["n_star"] == 6 and top["censored"]


def test_tier_fixed_point_on_a_winner_take_all_race():
    # Fast always beats slow: slow earns only when no one is fast, so the fast
    # tier fills up exactly as single-tier entry would: n_fast = floor(R / c).
    pool, rent, cost = 8, 1.0, 0.3
    fast = [0.0] + [rent / k for k in range(1, pool + 1)]
    slow = [rent / (pool + 1)] + [0.0] * pool
    for start in (0, pool):
        fp = x.tier_fixed_point(fast, slow, cost, start)
        assert fp["n_fast"] == 3, fp
        assert fp["path"][0] == start and fp["path"][-1] == 3


@pytest.fixture(scope="module")
def short_rows():
    tasks = [(x.CONFIG, 2.0, ("fast",) * n, "zero", s, SHORT_S) for n in (1, 3) for s in SEEDS]
    flat = x.run_tasks(tasks, workers=1)
    return {1: flat[:len(SEEDS)], 3: flat[len(SEEDS):]}


def test_profit_per_sniper_falls_as_snipers_enter(short_rows):
    one = np.mean([r["fast_net"] for r in short_rows[1]])
    three = np.mean([r["fast_net"] for r in short_rows[3]])
    assert one > three > 0
    # Common random numbers: the same path for every N.
    assert [r["n_jumps"] for r in short_rows[1]] == [r["n_jumps"] for r in short_rows[3]]


def test_welfare_identities_hold_at_the_free_entry_equilibrium(short_rows):
    cost = 0.12
    fe = x.free_entry({n: [r["fast_net"] for r in rows] for n, rows in short_rows.items()}, cost)
    n = fe["n_star"]
    assert n >= 1
    tiers = ("fast",) * n
    res = x.simulate(x.CONFIG, 2.0, tiers, "taker_heavy", seed=0, duration_s=SHORT_S)
    # The maker is on the fast line too (BCS), so it pays c as well.
    speed = {pid: cost * SHORT_S for pid in [x.MAKER] + [x.FIRST_SNIPER + i for i in range(n)]}
    ledger = build_ledger(res.trades, x.FEES["taker_heavy"], speed)
    classes = {x.MAKER: "maker", x.NOISE: "noise"} | {x.FIRST_SNIPER + i: "sniper" for i in range(n)}
    w = decompose(ledger, classes, res.last_mid, res.path.value_at(res.duration_ns - 1))
    scale = sum(tr.price * tr.quantity for tr in res.trades) / x.P
    # accounting.py's identities: gross P&L sums to zero; net P&L + exchange
    # revenue sums to zero. Speed is outside both, so all-in welfare = -speed.
    assert abs(w["gross_residual_v"]) <= 1e-12 * scale
    assert abs(w["transfer_residual_v"]) <= 1e-12 * scale
    assert w["speed_cost_total"] == pytest.approx((n + 1) * cost * SHORT_S)
    all_in = sum(r["net_v"] - r["speed_cost"] for r in w["by_class"].values())
    assert all_in + w["exchange"]["revenue"] == pytest.approx(-w["speed_cost_total"], abs=1e-9 * scale)
    # The per-fill V mark is zero-sum too, and the summary agrees with it.
    assert abs(sum(x.edge_pnl(res.trades, res.path).values())) <= 1e-12 * scale
    row = x.summarize(res, tiers, "taker_heavy")
    wel = x.welfare_at([row], n, cost, maker_pays_speed=True)
    assert wel["speed_spend_total"] == pytest.approx((n + 1) * cost)
    assert wel["exchange_revenue"]["mean"] == pytest.approx(w["exchange"]["revenue"] / SHORT_S)


def test_same_seed_reproduces_exactly_in_and_out_of_the_pool():
    tasks = [(x.CONFIG, 2.0, ("fast", "fast"), "taker_heavy", s, 20.0) for s in (5, 6)]
    serial = x.run_tasks(tasks, workers=1)
    assert x.run_tasks(tasks, workers=1) == serial
    assert x.run_tasks(tasks, workers=2) == serial


def test_theory_half_spread_nests_bcs_lambert_w_for_inelastic_investors():
    # Huge reservation shocks: every arrival crosses (2 Phi(-h/eps) -> 1), so
    # eq. (3) holds with lam_i = lam_n q_n / q_m investors per quote unit.
    import bcs_theory
    cfg = x.CONFIG | {"noise_eps_bp": 1e9}
    p = cfg["path"]
    b = x._fixed(cfg, p["jump_scale_bp"])
    h = bcs_theory.laplace_half_spread(cfg["noise_rate_per_s"] * cfg["noise_qty"] / cfg["maker_qty"],
                                       p["jump_intensity_per_s"], b)
    assert x.theory_half_spread_bp(cfg) == pytest.approx(h / (1e-4 * p["v0"]), rel=1e-6)


def test_theory_free_entry_counts_the_racing_maker():
    th = x.theory(x.CONFIG, 2.0, "zero", "fast")
    rent = th["prize_rate"]
    fast = x.theory(x.CONFIG, 2.0, "zero", "fast", cost=rent / 5.5)
    slow = x.theory(x.CONFIG, 2.0, "zero", "slow", cost=rent / 5.5)
    # floor(5.5) = 5 racing firms: the fast maker is one of them.
    assert (fast["n_star"], slow["n_star"]) == (4, 5)
    assert fast["dissipated_share"] == pytest.approx(5 / 5.5)
    assert slow["dissipated_share"] == pytest.approx(5 / 5.5)
    # Outside BCS's domain (c > R/2, footnote 26) there is no prediction.
    assert x.theory(x.CONFIG, 2.0, "zero", "fast", cost=rent * 0.6)["n_star"] is None


# --- confirmatory mode (P4, spec geometry) -------------------------------------

def test_entry_stops_at_the_first_entrant_whose_interval_is_not_above_cost():
    import exp10_speed_entry as x10
    profit = {2: [10.0, 10.2], 3: [6.0, 6.1], 4: [4.9, 5.2], 5: [3.0, 3.1]}
    e = x10.entry_stop(profit, 5.0, 2)
    assert e["n_star"] == 3 and not e["censored"]       # entrant 4's interval straddles 5
    assert x10.entry_stop(profit, 100.0, 2)["n_star"] == 1
    assert x10.entry_stop(profit, 0.0, 2)["censored"]


def test_confirmatory_mode_writes_p4_verdicts(tmp_path):
    import exp10_speed_entry as x10
    r = x10.confirmatory(["--confirmatory", "--seeds", "2", "--duration-s", "3", "--workers", "1",
                          "--out-dir", str(tmp_path)])
    assert r["status"].startswith("smoke")
    assert {"H4a", "H4b", "H4c"} <= set(r["p4"])
    assert r["max_zero_sum_residual"] < 1e-6
    assert (tmp_path / "exp10_speed_entry.json").exists()
