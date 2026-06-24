"""Unit tests for the phase-transition detector.

Exp 2 asks whether HFT rent rises *smoothly* with the latency advantage (a
single linear trend) or undergoes a threshold/phase transition (flat, then a
steeper segment). We model that as linear vs continuous two-segment (hinge)
least-squares fits compared by BIC. Tests use synthetic data with a known
generating model so the detector's verdict and recovered breakpoint can be
checked against ground truth.
"""
import sys
import pathlib

_R = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(_R / "analysis"))

import numpy as np
import pytest

from phase_transition import fit_linear, fit_segmented, detect_phase_transition


def test_linear_fit_recovers_slope_and_intercept():
    x = np.arange(20, dtype=float)
    y = 5.0 + 3.0 * x
    fit = fit_linear(x, y)
    assert fit["a"] == pytest.approx(5.0, abs=1e-6)
    assert fit["b"] == pytest.approx(3.0, abs=1e-6)
    assert fit["rss"] == pytest.approx(0.0, abs=1e-9)


def test_segmented_recovers_known_breakpoint():
    # Flat (slope ~0) up to c0=10, then steep slope above it.
    c0 = 10.0
    x = np.arange(0, 21, dtype=float)
    y = np.where(x <= c0, 0.0, 4.0 * (x - c0))
    fit = fit_segmented(x, y)
    assert fit["breakpoint"] == pytest.approx(c0, abs=1.0)
    assert fit["rss"] == pytest.approx(0.0, abs=1e-6)


def test_detector_prefers_segmented_for_hinge_data():
    rng = np.random.default_rng(0)
    c0 = 10.0
    x = np.repeat(np.arange(0, 21, dtype=float), 5)
    y = np.where(x <= c0, 0.0, 4.0 * (x - c0)) + rng.normal(0, 0.5, size=x.size)
    res = detect_phase_transition(x, y)
    assert res["prefers_segmented"] is True
    assert res["delta_bic"] > 10.0          # strong evidence
    assert res["breakpoint"] == pytest.approx(c0, abs=2.0)


def test_detector_prefers_linear_for_linear_data():
    rng = np.random.default_rng(1)
    x = np.repeat(np.arange(0, 21, dtype=float), 5)
    y = 2.0 + 3.0 * x + rng.normal(0, 0.5, size=x.size)
    res = detect_phase_transition(x, y)
    assert res["prefers_segmented"] is False
    assert res["delta_bic"] < 0.0           # linear wins on BIC


def test_too_few_points_raises():
    with pytest.raises(ValueError):
        fit_segmented(np.array([1.0, 2.0, 3.0]), np.array([1.0, 2.0, 3.0]))


def test_mismatched_lengths_raise():
    with pytest.raises(ValueError):
        fit_linear(np.array([1.0, 2.0]), np.array([1.0]))


def test_result_is_json_serializable():
    import json
    x = np.repeat(np.arange(0, 21, dtype=float), 3)
    y = 2.0 + 3.0 * x
    json.dumps(detect_phase_transition(x, y))
