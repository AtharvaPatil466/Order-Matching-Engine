"""Unit tests for the bootstrap CI module.

Every experiment from Exp 2 onward reports bootstrap CIs rather than bare point
estimates, so this is shared infrastructure. Tests cover the statistical
contract (mean recovery, bracketing, monotonicity in the CI level),
determinism under a fixed seed, degenerate inputs, boundary validation, the
per-seed-rows convenience table, and JSON safety.
"""
import sys
import pathlib

_R = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(_R / "metrics"))

import json
import statistics

import pytest

from bootstrap import bootstrap_ci, bootstrap_ci_table


# --- 1. statistical contract -------------------------------------------------

def test_mean_matches_sample_mean():
    values = [1.0, 2.0, 3.0, 4.0, 5.0]
    ci = bootstrap_ci(values, seed=0)
    assert ci["mean"] == pytest.approx(statistics.fmean(values))


def test_n_reports_sample_size():
    assert bootstrap_ci([1.0, 2.0, 3.0], seed=0)["n"] == 3


def test_ci_brackets_the_mean():
    values = [10.0, 12.0, 9.0, 11.0, 13.0, 8.0, 14.0, 7.0]
    ci = bootstrap_ci(values, seed=1)
    assert ci["lower"] <= ci["mean"] <= ci["upper"]


def test_std_matches_sample_std():
    values = [4.0, 8.0, 15.0, 16.0, 23.0, 42.0]
    ci = bootstrap_ci(values, seed=0)
    # population std (ddof=0), matching numpy default
    expected = statistics.pstdev(values)
    assert ci["std"] == pytest.approx(expected, rel=1e-9)


def test_higher_confidence_level_is_wider():
    values = [10.0, 12.0, 9.0, 11.0, 13.0, 8.0, 14.0, 7.0, 6.0, 15.0]
    narrow = bootstrap_ci(values, ci=0.80, seed=2)
    wide = bootstrap_ci(values, ci=0.99, seed=2)
    narrow_width = narrow["upper"] - narrow["lower"]
    wide_width = wide["upper"] - wide["lower"]
    assert wide_width >= narrow_width


# --- 2. determinism ----------------------------------------------------------

def test_same_seed_is_reproducible():
    values = [10.0, 12.0, 9.0, 11.0, 13.0, 8.0]
    a = bootstrap_ci(values, seed=7)
    b = bootstrap_ci(values, seed=7)
    assert a == b


def test_different_seed_changes_ci_for_nondegenerate_input():
    # Many distinct values so the percentile bounds (not just the underlying
    # resample distribution) almost surely differ across seeds. With few/tied
    # values the summary percentiles can coincide even though resampling differs.
    values = [float(x) for x in range(30)]
    a = bootstrap_ci(values, seed=7)
    b = bootstrap_ci(values, seed=8)
    # Point mean is seed-independent; the resampled CI bounds should differ.
    assert a["mean"] == pytest.approx(b["mean"])
    assert (a["lower"], a["upper"]) != (b["lower"], b["upper"])


# --- 3. degenerate inputs ----------------------------------------------------

def test_constant_values_give_zero_width_ci():
    ci = bootstrap_ci([5.0, 5.0, 5.0, 5.0], seed=0)
    assert ci["lower"] == pytest.approx(5.0)
    assert ci["upper"] == pytest.approx(5.0)
    assert ci["mean"] == pytest.approx(5.0)
    assert ci["std"] == pytest.approx(0.0)


def test_single_value_does_not_raise():
    ci = bootstrap_ci([42.0], seed=0)
    assert ci["mean"] == pytest.approx(42.0)
    assert ci["lower"] == pytest.approx(42.0)
    assert ci["upper"] == pytest.approx(42.0)
    assert ci["n"] == 1


# --- 4. boundary validation --------------------------------------------------

def test_empty_values_raises():
    with pytest.raises(ValueError):
        bootstrap_ci([], seed=0)


def test_ci_out_of_range_raises():
    with pytest.raises(ValueError):
        bootstrap_ci([1.0, 2.0], ci=1.5, seed=0)
    with pytest.raises(ValueError):
        bootstrap_ci([1.0, 2.0], ci=0.0, seed=0)


def test_nonpositive_n_bootstrap_raises():
    with pytest.raises(ValueError):
        bootstrap_ci([1.0, 2.0], n_bootstrap=0, seed=0)


# --- 5. per-seed-rows table --------------------------------------------------

def test_table_builds_ci_per_metric_key():
    rows = [
        {"hft_rent": 10.0, "mm_pnl": -8.0},
        {"hft_rent": 12.0, "mm_pnl": -9.0},
        {"hft_rent": 11.0, "mm_pnl": -7.0},
    ]
    table = bootstrap_ci_table(rows, ["hft_rent", "mm_pnl"], seed=0)
    assert set(table) == {"hft_rent", "mm_pnl"}
    assert table["hft_rent"]["mean"] == pytest.approx(11.0)
    assert table["mm_pnl"]["mean"] == pytest.approx(-8.0)
    assert table["hft_rent"]["n"] == 3


def test_table_raises_on_empty_rows():
    with pytest.raises(ValueError):
        bootstrap_ci_table([], ["hft_rent"], seed=0)


def test_table_raises_on_missing_key():
    rows = [{"hft_rent": 10.0}, {"hft_rent": 12.0}]
    with pytest.raises(KeyError):
        bootstrap_ci_table(rows, ["does_not_exist"], seed=0)


# --- 6. json safety ----------------------------------------------------------

def test_result_is_json_serializable():
    ci = bootstrap_ci([1.0, 2.0, 3.0], seed=0)
    json.dumps(ci)  # must not raise
    table = bootstrap_ci_table([{"x": 1.0}, {"x": 2.0}], ["x"], seed=0)
    json.dumps(table)
