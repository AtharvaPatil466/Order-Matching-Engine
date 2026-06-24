import sys, pathlib
_R = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(_R / "agents"))

import numpy as np
import pytest

from fundamental_value import FundamentalValueProcess


def test_init_value_before_step():
    v0 = 1_000_000
    fv = FundamentalValueProcess(v0=v0, sigma=10.0)
    assert fv.value == float(v0)
    assert fv.observe(0) == float(v0)


def test_init_invalid_args_raise():
    with pytest.raises(ValueError):
        FundamentalValueProcess(v0=1_000_000, sigma=-1.0)
    with pytest.raises(ValueError):
        FundamentalValueProcess(v0=1_000_000, sigma=10.0, dt_us=0)
    with pytest.raises(ValueError):
        FundamentalValueProcess(v0=1_000_000, sigma=10.0, dt_us=-5)


def test_reproducibility_same_seed_identical_path():
    fv_a = FundamentalValueProcess(v0=1_000_000, sigma=10.0, seed=42)
    fv_b = FundamentalValueProcess(v0=1_000_000, sigma=10.0, seed=42)

    path_a = [fv_a.step(t) for t in range(1000, 101_000, 1000)]
    path_b = [fv_b.step(t) for t in range(1000, 101_000, 1000)]

    assert path_a == path_b
    assert len(path_a) == 100


def test_reproducibility_different_seed_differs():
    fv_a = FundamentalValueProcess(v0=1_000_000, sigma=10.0, seed=1)
    fv_b = FundamentalValueProcess(v0=1_000_000, sigma=10.0, seed=2)

    path_a = [fv_a.step(t) for t in range(1000, 101_000, 1000)]
    path_b = [fv_b.step(t) for t in range(1000, 101_000, 1000)]

    assert path_a != path_b


def test_latency_observe():
    fv = FundamentalValueProcess(v0=1_000_000, sigma=10.0, seed=7)
    v1 = fv.step(1000)
    v2 = fv.step(2000)
    v3 = fv.step(3000)

    assert fv.observe(3000, latency_us=0) == v3
    assert fv.observe(3000, latency_us=1000) == v2
    assert fv.observe(3000, latency_us=2000) == v1
    assert fv.observe(500, latency_us=0) == float(1_000_000)


def test_random_walk_variance_loose():
    sigma = 10.0
    n = 5000
    fv = FundamentalValueProcess(v0=1_000_000, sigma=sigma, seed=123)

    values = [fv.step(t) for t in range(1000, (n + 1) * 1000, 1000)]
    increments = np.diff(np.concatenate(([float(1_000_000)], values)))

    assert len(increments) == n

    empirical_var = float(np.var(increments))
    assert 0.5 * sigma**2 <= empirical_var <= 1.5 * sigma**2

    mean_drift_per_step = abs(float(np.mean(increments)))
    assert mean_drift_per_step < 0.5
