"""Exp 17 (P3): H3a's closed form, the clustered slope, the verdicts, the two paths.

H3a's prediction is a number P3 prints for three cadences, so the closed form
must reproduce all three; the walk must have the variance rate it claims; and
each verdict rule is checked on hand-made inputs for HIT, MISS, INCONCLUSIVE.
"""
import math
import sys
from pathlib import Path

_R = Path(__file__).resolve().parents[1]
if str(_R / "experiments") not in sys.path:
    sys.path.insert(0, str(_R / "experiments"))

import numpy as np  # noqa: E402
import pytest  # noqa: E402

import _confirm_common as cc  # noqa: E402
import exp17_jump_vs_walk as x17  # noqa: E402


@pytest.mark.parametrize("rate,want,tol", [(10, 0.87, 0.005), (100, 1.30, 0.005),
                                           (1000, 2900, 50)])
def test_h3a_theory_reproduces_p3s_table(rate, want, tol):
    assert x17.h3a_theory(lam_rw=rate) == pytest.approx(want, abs=tol)


def test_h3a_runs_at_the_middle_of_p1s_grid():
    assert x17.H3A_H in x17.H_GRID and x17.H_GRID[3] == 567


def test_rw_profit_per_snipe_falls_with_h():
    s = cc.rw_sigma(10, 1000, 100)
    vals = [x17.rw_profit_per_snipe(h, s) for h in x17.H_GRID]
    assert all(a > b for a, b in zip(vals, vals[1:]))


def test_walk_matches_the_jump_variance_rate():
    p = x17.RW_OP_PATH
    jd = x17._P
    assert p["jump_intensity_per_s"] == 0
    assert p["sigma_bp_per_sqrt_s"] ** 2 == pytest.approx(
        jd["sigma_bp_per_sqrt_s"] ** 2 + jd["jump_intensity_per_s"] * 2 * jd["jump_scale_bp"] ** 2)


def test_slope_ci_recovers_a_known_slope():
    hs = [1.0, 2.0, 3.0]
    fills = np.full((3, 50), 10.0)
    edge = np.array([[10.0 * (5 - 0.5 * h)] * 50 for h in hs])
    s = x17.slope_ci(hs, edge, fills)
    assert s["slope"] == pytest.approx(-0.5) and s["upper"] < 0


def _ci(lo, hi):
    return {"mean": (lo + hi) / 2, "lower": lo, "upper": hi}


def test_h3b_hit_miss_inconclusive():
    inside = {h: _ci(980, 1020) for h in x17.H_GRID}
    flat, down = _ci(-0.1, 0.1), _ci(-0.3, -0.1)
    assert x17.h3b(inside, {"laplace": flat, "rw": down})["verdict"] == "HIT"
    assert x17.h3b(inside, {"laplace": _ci(0.05, 0.2), "rw": down})["verdict"] == "MISS"
    assert x17.h3b(inside, {"laplace": flat, "rw": _ci(0.0, 0.2)})["verdict"] == "MISS"
    wide = {**inside, x17.H_GRID[0]: _ci(900, 1100)}
    assert x17.h3b(wide, {"laplace": flat, "rw": down})["verdict"] == "INCONCLUSIVE"
    assert x17.h3b(inside, {"laplace": flat, "rw": _ci(-0.2, 0.1)})["verdict"] == "INCONCLUSIVE"


def test_h3a_and_h3c_verdicts():
    th = x17.h3a_theory()
    rw = np.linspace(100, 200, 200)
    assert x17.h3a(th * rw, rw)["verdict"] == "HIT"
    assert x17.h3a(1.5 * th * rw, rw)["verdict"] == "MISS"
    noisy = th * rw * np.random.default_rng(0).lognormal(0, 1.5, rw.size)
    assert x17.h3a(noisy, rw)["verdict"] in ("INCONCLUSIVE", "MISS")
    assert x17.h3c_verdict(3, 7) == "HIT"
    assert x17.h3c_verdict(7, 7) == "MISS" and x17.h3c_verdict(0, 0) == "MISS"


def test_bcs_runs_are_deterministic_and_laplace_snipes_pay_about_b():
    a = x17.bcs_task("laplace", 567, 3, duration_s=60)
    assert a == x17.bcs_task("laplace", 567, 3, duration_s=60)
    assert a["sniper_fills"] > 50
    assert a["sniper_edge"] / a["sniper_fills"] == pytest.approx(1000, rel=0.3)
    w = x17.bcs_task("rw", 567, 3, duration_s=20)
    assert w["n_jumps"] == 2000 and w["sniper_fills"] > 0      # every 10-ms step is news


def test_h3c_walk_arm_swaps_only_the_path():
    a = x17.h3c_run("laplace", 3, 0.0049, 2, horizon_s=30)
    b = x17.h3c_run("rw", 3, 0.0049, 2, horizon_s=30)
    assert a == x17.h3c_run("laplace", 3, 0.0049, 2, horizon_s=30)
    assert a["n_jumps"] > 0 or b["n_jumps"] == 0
    assert b["n_jumps"] == 0                                    # the walk has no jumps


def test_end_to_end_summary_on_tiny_runs(tmp_path):
    seeds = [1, 2]
    rows = cc.run_pending(x17.tasks(seeds, horizon_s=1, bcs_duration_s=5), cc.call,
                          tmp_path / "ck.jsonl", 1, "t")
    s = x17.summarize(rows, seeds)
    assert all(s[h]["verdict"] in ("HIT", "MISS", "INCONCLUSIVE") for h in ("H3a", "H3b", "H3c"))
    assert math.isfinite(s["H3a"]["mean"])
