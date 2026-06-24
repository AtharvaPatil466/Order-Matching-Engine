"""Unit tests for the Exp 1 hardening helpers + a tiny live structural smoke.

Hardening addresses the three pre-registered soft spots: (1) n=20 seeds with
bootstrap CIs on Exp 1, (2) a 3x3 calibration robustness sweep around
(sens=0.1, decay=0.1), (3) baseline liquidity-gap characterization. The pure
verdict helpers (rent significance, zero-sum closure, CI separation) are unit
tested here; a 1-seed smoke confirms the report assembly runs against the
verified engine.
"""
import sys
import pathlib

_R = pathlib.Path(__file__).resolve().parents[1]
for _d in ("build", "agents", "simulation", "metrics", "experiments"):
    sys.path.insert(0, str(_R / _d))

import exp1_hardening as h


# --- pure verdict helpers ----------------------------------------------------

def test_rent_significant_when_ci_lower_above_zero():
    assert h.rent_significant({"lower": 25.6, "mean": 45.6, "upper": 70.0}) is True


def test_rent_not_significant_when_ci_brackets_zero():
    assert h.rent_significant({"lower": -4.3, "mean": 8.4, "upper": 24.2}) is False


def test_zero_sum_ok_within_tolerance():
    assert h.zero_sum_ok({"mean": 0.002, "lower": -0.01, "upper": 0.01}, tol=0.1) is True


def test_zero_sum_flagged_when_residual_large():
    assert h.zero_sum_ok({"mean": 5.0, "lower": 1.0, "upper": 9.0}, tol=0.1) is False


def test_ci_separated_when_intervals_do_not_overlap():
    # treatment gaps strictly above baseline gaps -> separated.
    lo = {"lower": 2.0, "upper": 5.0}
    hi = {"lower": 8.0, "upper": 12.0}
    assert h.ci_separated(lo, hi) is True


def test_ci_not_separated_when_intervals_overlap():
    lo = {"lower": 2.0, "upper": 9.0}
    hi = {"lower": 8.0, "upper": 12.0}
    assert h.ci_separated(lo, hi) is False


# --- live structural smoke (tiny n, real engine) -----------------------------

def test_harden_exp1_report_shape_smoke():
    rep = h.harden_exp1(n_seeds=2, boot_seed=0)
    assert rep["n_seeds"] == 2
    for key in ("hft_rent", "mm_pnl", "nt_pnl", "liquidity_gaps", "zero_sum_residual"):
        ci = rep["treatment_ci"][key]
        assert {"mean", "lower", "upper", "n"} <= set(ci)
    assert "rent_significant" in rep["verdict"]


def test_calibration_sweep_smoke_covers_grid():
    rep = h.calibration_sweep(n_seeds=2, sens_grid=[0.1], decay_grid=[0.1], boot_seed=0)
    assert len(rep["cells"]) == 1
    cell = rep["cells"][0]
    assert cell["sens"] == 0.1 and cell["decay"] == 0.1
    assert "hft_rent" in cell["treatment_ci"]
