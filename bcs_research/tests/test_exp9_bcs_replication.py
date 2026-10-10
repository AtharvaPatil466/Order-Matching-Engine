"""Exp 9 (BCS replication, P1): the driver's pieces and one tiny end-to-end cell.

The pilot and the confirmatory run are only as good as three things checked
here: a short run on the real engine already lands on BCS's closed forms; the
same seed reproduces a run exactly, and different h or N meet the same jumps
and investors (common random numbers); and the zero-profit search recovers a
known root from a monotone curve, exactly when noise-free and within its
interval when noisy.
"""
import json
import math
import sys
from pathlib import Path

_R = Path(__file__).resolve().parents[1]
for _d in ("build", "simulation", "agents", "metrics", "analysis", "experiments"):
    if str(_R / _d) not in sys.path:
        sys.path.insert(0, str(_R / _d))

import numpy as np  # noqa: E402
import pytest  # noqa: E402

import bcs_engine as be  # noqa: E402
from bcs_theory import laplace_half_spread  # noqa: E402
from contracts import Fill, Order  # noqa: E402
from exp9_bcs_replication import (V0, BCSLiquidityProvider, JumpsOnlyPath, cell_grid,  # noqa: E402
                                  classify, grid_root, main, paired_slope, root_estimate,
                                  run_one, tasks_for)
from fundamental_paths import JumpDiffusionPath  # noqa: E402

B, LAM_I = 1000.0, 10.0


def _theory_d(h, lam_jump):
    """Spec §3: D(h) = λ_i·h − λ_j·b·e^(−h/b), root h*."""
    return LAM_I * h - lam_jump * B * math.exp(-h / B)


# --- the zero-profit search ----------------------------------------------------

def test_grid_root_is_exact_on_a_linear_curve():
    hs = [85, 90, 95, 100, 105, 110, 115]
    root, extrapolated, changes = grid_root(hs, [h - 101.5 for h in hs])
    assert root == pytest.approx(101.5)
    assert not extrapolated and changes == 1


def test_grid_root_extrapolates_the_nearest_end_segment_without_a_sign_change():
    hs = [85, 90, 95, 100]
    assert grid_root(hs, [h - 70 for h in hs])[:2] == (pytest.approx(70), True)
    assert grid_root(hs, [h - 130 for h in hs])[:2] == (pytest.approx(130), True)


def test_search_converges_on_the_noise_free_bcs_curve():
    lam_jump = 10.0
    h_star = laplace_half_spread(LAM_I, lam_jump, B)
    hs = cell_grid(1.0, 2)["grid_star"]
    root, extrapolated, _ = grid_root(hs, [_theory_d(h, lam_jump) for h in hs])
    assert not extrapolated
    assert root == pytest.approx(h_star, rel=1e-3)   # chord error only


def test_search_interval_covers_the_root_of_a_noisy_monotone_curve():
    lam_jump = 40.0
    h_star = laplace_half_spread(LAM_I, lam_jump, B)
    hs = cell_grid(4.0, 2)["grid_star"]
    rng = np.random.default_rng(3)
    curves = [[_theory_d(h, lam_jump) + rng.normal(0, 0.03 * LAM_I * h_star) for h in hs]
              for _ in range(40)]
    est = root_estimate(hs, curves)
    assert est["lower"] < h_star < est["upper"]
    assert est["upper"] - est["lower"] < 0.03 * h_star
    assert len(est["per_seed"]) == 40


def test_classify_follows_the_spec_band_rule():
    assert classify(0.49, 0.51, 0.48, 0.52) == "PASS"
    assert classify(0.40, 0.45, 0.48, 0.52) == "MISS"
    assert classify(0.47, 0.50, 0.48, 0.52) == "INCONCLUSIVE"


def test_paired_slope_is_zero_when_roots_do_not_move_with_n():
    rng = np.random.default_rng(0)
    seed_effect = rng.normal(500, 20, 40)          # shared by every N: CRN
    roots = [seed_effect + rng.normal(0, 1, 40) for _ in range(4)]
    out = paired_slope([0.5, 2 / 3, 0.8, 0.9], roots, half_width=15.0)
    assert out["lower"] < 0 < out["upper"]
    assert out["verdict"] == "PASS"


# --- the agents and the path ---------------------------------------------------

def test_lp_refills_a_live_quote_at_its_price_and_ignores_a_replaced_one():
    lp = BCSLiquidityProvider(1, half_spread=100, v0=V0, latency_ns=0)
    start = lp.on_start(0, None)
    ask = next(a for a in start if a.side == be.Side.Sell)
    refill = lp.on_fill(5, Fill(5, ask.order_id, ask.price, 1, False, True), None)
    assert len(refill) == 1 and refill[0].price == ask.price and refill[0].side == be.Side.Sell
    stale_bid = next(a for a in start if a.side == be.Side.Buy)
    lp.on_signal(6, type("U", (), {"value": V0 + 5000})(), None)
    assert lp.on_fill(7, Fill(7, stale_bid.order_id, stale_bid.price, 1, True, True), None) == []


def test_lp_requote_cancels_before_it_quotes():
    lp = BCSLiquidityProvider(1, half_spread=100, v0=V0, latency_ns=0)
    lp.on_start(0, None)
    burst = lp.on_signal(1, type("U", (), {"value": V0 + 300.4})(), None)
    kinds = [type(a).__name__ for a in burst]
    assert kinds == ["Cancel", "Cancel", "Order", "Order"]
    assert [a.price for a in burst if isinstance(a, Order)] == [V0 + 200, V0 + 400]


def test_jumps_only_path_reflects_at_the_cap_and_keeps_every_jump_size():
    src = JumpDiffusionPath(V0, 0.0, 10**9, 10**9, seed=1, jump_intensity_per_s=2000,
                            jump_scale=B)
    path = JumpsOnlyPath(src, reflect_at=20_000)   # > every jump; walk s.d. ~63,000
    ups = path.updates()
    src_jumps = [u for u in src.updates() if u.is_jump]
    assert len(ups) == len(src_jumps) > 1000 and all(u.is_jump for u in ups)
    sizes = np.abs(np.diff([V0] + [u.value for u in ups]))
    src_sizes = np.abs(np.diff([V0] + [u.value for u in src_jumps]))
    np.testing.assert_allclose(sizes, src_sizes, atol=1e-6)
    assert max(abs(u.value - V0) for u in ups) <= 20_000
    assert path.value_at(ups[10].t_ns) == ups[10].value
    assert path.value_at(ups[0].t_ns - 1) == V0


# --- end to end on the engine --------------------------------------------------

@pytest.fixture(scope="module")
def tiny_cell():
    """λ_j = 40/s, N = 3, h = h*: 3 seeds × 60 s (~2,000 snipe-able jumps)."""
    h = round(laplace_half_spread(LAM_I, 40.0, B))
    return h, [run_one(40.0, 3, h, s, 60.0) for s in range(3)]


def test_tiny_cell_reproduces_bcs_race_loss_and_snipe_profit(tiny_cell):
    h, rows = tiny_cell
    snipeable = sum(r["n_snipeable"] for r in rows)
    assert snipeable > 1500
    assert sum(r["n_sniped"] for r in rows) / snipeable == pytest.approx(2 / 3, abs=0.04)
    per_snipe = sum(r["sniper_edge"] for r in rows) / sum(r["sniper_fills"] for r in rows)
    assert per_snipe == pytest.approx(B, rel=0.10)
    jumps = sum(r["n_jumps"] for r in rows)
    assert snipeable / jumps == pytest.approx(math.exp(-h / B), abs=0.03)


def test_tiny_cell_books_close_and_every_investor_trades(tiny_cell):
    _, rows = tiny_cell
    for r in rows:
        assert r["investor_fills"] == r["investor_arrivals"] > 0
        assert abs(r["zero_sum_residual"]) < 1e-6 * r["n_trades"] * B
        assert r["n_quote_gaps"] <= 0.005 * r["n_jumps"]


def test_same_seed_reproduces_a_run_exactly(tiny_cell):
    h, rows = tiny_cell
    assert run_one(40.0, 3, h, 0, 60.0) == rows[0]


def test_common_random_numbers_hold_across_h_and_n():
    a = run_one(10.0, 2, 500, 4, 20.0)
    b = run_one(10.0, 5, 600, 4, 20.0)
    assert a["seeds"] == b["seeds"]
    assert (a["n_jumps"], a["investor_arrivals"]) == (b["n_jumps"], b["investor_arrivals"])
    assert run_one(10.0, 2, 500, 5, 20.0)["seeds"]["path"] != a["seeds"]["path"]


def test_main_writes_a_pilot_report_with_seeds_and_verdicts(tmp_path):
    out = tmp_path / "exp9.json"
    main(["--seeds", "2", "--duration-s", "5", "--ratios", "1", "--n-firms", "2", "3",
          "--workers", "1", "--out", str(out)])
    report = json.loads(out.read_text())
    assert report["status"].startswith("pilot")
    assert [s["seed"] for s in report["seeds"]] == [0, 1]
    assert len(report["cells"]) == 2
    for cell in report["cells"]:
        for key in ("race_loss", "h_hat", "h0_hat", "snipe_profit", "jitter_race_loss"):
            assert cell[key]["verdict"] in ("PASS", "MISS", "INCONCLUSIVE")
    assert report["p1_primary"] in ("PASS", "MISS", "INCONCLUSIVE")
    n_runs = len(tasks_for([cell_grid(1.0, 2), cell_grid(1.0, 3)], [0, 1], 5.0))
    assert out.with_suffix(".runs.jsonl").read_text().count("\n") == n_runs
