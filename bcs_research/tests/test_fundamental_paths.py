"""Fundamental paths for the event model: nesting, conversion and moments.

Moment tolerances are stated per test as multiples of the estimator's
standard error, wide enough (>= 4 SE) that a correct generator fails with
probability well under 1e-4, narrow enough to catch a wrong unit (a factor
sqrt(10) or 1e4) or a wrong jump law.
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

_ROOT = Path(__file__).resolve().parents[1]
for _d in ("agents", "simulation"):
    sys.path.insert(0, str(_ROOT / _d))

from fundamental_paths import CorrelatedSignalPath, JumpDiffusionPath  # noqa: E402
from fundamental_value import FundamentalValueProcess  # noqa: E402

V0 = 100 * 10_000          # 100.00 in the engine's fixed-point space
NS_PER_S = 1_000_000_000


def _calib(**over):
    d = {"v0": V0, "sigma_bp_per_sqrt_s": 1.0, "jump_intensity_per_s": 0.0,
         "jump_size_dist": "laplace", "jump_scale_bp": 0.0, "update_interval_ms": 100}
    return {**d, **over}


def _values(path):
    return np.array([u.value for u in path.updates()])


@pytest.mark.parametrize("seed", [0, 1, 42])
def test_degenerate_config_reproduces_fundamental_value_process_bit_for_bit(seed):
    # Arrange: the tick model's exp1 setting — 1000 us ticks, per-step sigma 20
    dt_us, sigma, n_steps = 1000, 20.0, 5000
    old = FundamentalValueProcess(v0=V0, sigma=sigma, dt_us=dt_us, seed=seed)
    old_values = [old.step(t) for t in range(0, n_steps * dt_us, dt_us)]

    # Act
    path = JumpDiffusionPath(v0=V0, sigma_step=sigma, update_interval_ns=dt_us * 1000,
                             duration_ns=n_steps * dt_us * 1000, seed=seed)

    # Assert: same values, same grid, exact float equality
    ups = path.updates()
    assert [u.value for u in ups] == old_values
    assert [u.t_ns for u in ups] == [k * dt_us * 1000 for k in range(n_steps)]
    assert not any(u.is_jump for u in ups)
    assert [path.value_at(k * dt_us * 1000) for k in range(n_steps)] == old_values


def test_value_at_is_v0_before_first_update_and_holds_between_updates():
    # Arrange
    path = JumpDiffusionPath(v0=V0, sigma_step=5.0, update_interval_ns=1000,
                             duration_ns=10_000, seed=3)
    ups = path.updates()

    # Act / Assert
    assert path.value_at(-1) == V0
    assert path.value_at(1500) == ups[1].value          # held from t=1000
    assert path.value_at(10**12) == ups[-1].value       # past the end: last value


def test_same_seed_same_path_different_seed_different_path():
    # Arrange
    kw = dict(v0=V0, sigma_step=5.0, update_interval_ns=10**8, duration_ns=100 * NS_PER_S,
              jump_intensity_per_s=0.2, jump_scale=50.0)

    # Act
    a, b, c = (JumpDiffusionPath(seed=s, **kw) for s in (7, 7, 8))

    # Assert
    assert a.updates() == b.updates()
    assert a.updates() != c.updates()


def test_updates_are_strictly_ascending_and_jumps_are_flagged():
    # Arrange
    path = JumpDiffusionPath(v0=V0, sigma_step=5.0, update_interval_ns=10**8,
                             duration_ns=500 * NS_PER_S, seed=11,
                             jump_intensity_per_s=0.1, jump_scale=100.0)

    # Act
    ups = path.updates()
    times = [u.t_ns for u in ups]

    # Assert: 5000 grid updates plus the jumps, none on the grid by chance
    assert times == sorted(set(times))
    n_jumps = sum(u.is_jump for u in ups)
    assert n_jumps > 0
    assert len(ups) == 5000 + n_jumps


def test_jump_landing_on_a_grid_point_is_published_once_and_flagged():
    # Arrange: a 1 ns grid, so every jump coincides with a grid update
    path = JumpDiffusionPath(v0=V0, sigma_step=1.0, update_interval_ns=1, duration_ns=50,
                             seed=5, jump_intensity_per_s=2e8, jump_scale=100.0)

    # Act
    ups = path.updates()

    # Assert: one update per ns, ~10 of them carry a jump
    assert [u.t_ns for u in ups] == list(range(50))
    assert 0 < sum(u.is_jump for u in ups) < 50
    assert all(path.value_at(u.t_ns) == u.value for u in ups)


def test_diffusion_is_unchanged_by_adding_jumps_for_the_same_seed():
    # Arrange
    kw = dict(v0=V0, sigma_step=5.0, update_interval_ns=10**8,
              duration_ns=200 * NS_PER_S, seed=9)
    plain = JumpDiffusionPath(**kw)
    jumpy = JumpDiffusionPath(jump_intensity_per_s=0.2, jump_scale=100.0, **kw)
    jump_times = np.array([u.t_ns for u in jumpy.updates() if u.is_jump])
    grid = [u.t_ns for u in plain.updates()]

    # Act: V_jumpy - V_plain should be the cumulative jump sum, constant between jumps
    diff = np.array([jumpy.value_at(t) - plain.value_at(t) for t in grid])
    n_jumps_before = np.searchsorted(jump_times, grid, side="right")

    # Assert
    assert len(jump_times) > 5
    for k in np.unique(n_jumps_before):
        assert np.ptp(diff[n_jumps_before == k]) < 1e-6


def test_from_calibration_sigma_is_one_second_sd_in_bp():
    # Arrange: 20,000 s at 100 ms; 1 s changes have sd = sigma_bp (bp of v0).
    # SE of a sample sd over n=20,000 is 0.5%; tolerance 3% (6 SE).
    path = JumpDiffusionPath.from_calibration(_calib(sigma_bp_per_sqrt_s=1.5),
                                              duration_ns=20_000 * NS_PER_S, seed=1)

    # Act
    per_second = _values(path)[::10]                    # grid index 10k is t = k s
    sd_bp = np.diff(per_second).std() / V0 * 1e4

    # Assert
    assert path.update_interval_ns == 100_000_000
    assert abs(sd_bp - 1.5) / 1.5 < 0.03


def test_diffusion_variance_scales_linearly_with_horizon():
    # Arrange: Var(10 s change) / Var(1 s change) = 10. With 4,000 ten-second
    # windows the ratio's SE is ~2.4%; tolerance 12% (5 SE).
    path = JumpDiffusionPath.from_calibration(_calib(), duration_ns=40_000 * NS_PER_S, seed=2)
    v = _values(path)

    # Act
    ratio = np.diff(v[::100]).var() / np.diff(v[::10]).var()

    # Assert
    assert abs(ratio - 10.0) / 10.0 < 0.12


def test_jump_count_is_poisson_with_the_configured_intensity():
    # Arrange: 400 paths of 100 s at 0.5 jumps/s -> count ~ Poisson(50).
    # SE of the mean is 0.35 (tolerance 1.5, ~4 SE); SE of the dispersion
    # index var/mean is ~0.07 (tolerance 0.3, ~4 SE).
    d = _calib(jump_intensity_per_s=0.5, jump_scale_bp=2.0, update_interval_ms=1000)

    # Act
    counts = np.array([
        sum(u.is_jump for u in JumpDiffusionPath.from_calibration(
            d, duration_ns=100 * NS_PER_S, seed=s).updates())
        for s in range(400)])

    # Assert
    assert abs(counts.mean() - 50.0) < 1.5
    assert abs(counts.var(ddof=1) / counts.mean() - 1.0) < 0.3


def test_jump_sizes_are_laplace_with_scale_in_bp():
    # Arrange: no diffusion, so each jump update's move IS the jump size.
    # Laplace(b): E|X| = b (SE 0.7% at n~20,000; tolerance 4%) and kurtosis 6
    # versus 3 for a normal (tolerance 1.5).
    path = JumpDiffusionPath.from_calibration(
        _calib(sigma_bp_per_sqrt_s=0.0, jump_intensity_per_s=2.0, jump_scale_bp=3.0,
               update_interval_ms=1000),
        duration_ns=10_000 * NS_PER_S, seed=4)
    ups = path.updates()
    prev = np.array([V0] + [u.value for u in ups[:-1]])
    sizes = np.array([u.value for u in ups]) - prev
    jump_bp = sizes[[u.is_jump for u in ups]] / V0 * 1e4

    # Act
    mean_abs = np.abs(jump_bp).mean()
    kurtosis = ((jump_bp - jump_bp.mean()) ** 4).mean() / jump_bp.var() ** 2

    # Assert
    assert len(jump_bp) > 15_000
    assert abs(mean_abs - 3.0) / 3.0 < 0.04
    assert abs(kurtosis - 6.0) < 1.5


def test_from_calibration_rejects_missing_keys_and_ignores_extras():
    # Arrange
    d = _calib(source="alphaforge", n_days=12)
    del d["jump_scale_bp"]

    # Act / Assert
    with pytest.raises(ValueError, match="jump_scale_bp"):
        JumpDiffusionPath.from_calibration(d, duration_ns=NS_PER_S)
    JumpDiffusionPath.from_calibration(_calib(source="x"), duration_ns=NS_PER_S)


def test_invalid_parameters_raise():
    with pytest.raises(ValueError):
        JumpDiffusionPath(v0=V0, sigma_step=-1.0, update_interval_ns=1, duration_ns=10)
    with pytest.raises(ValueError):
        JumpDiffusionPath(v0=V0, sigma_step=1.0, update_interval_ns=0, duration_ns=10)
    with pytest.raises(ValueError):
        JumpDiffusionPath(v0=V0, sigma_step=1.0, update_interval_ns=1, duration_ns=10,
                          jump_size_dist="cauchy")


# --- CorrelatedSignalPath ---------------------------------------------------

def _lead(seed=1, duration_s=200, **over):
    return JumpDiffusionPath.from_calibration(
        _calib(jump_intensity_per_s=0.1, jump_scale_bp=2.0, **over),
        duration_ns=duration_s * NS_PER_S, seed=seed)


def test_zero_tracking_error_with_unit_beta_makes_v_the_last_signal():
    # Arrange
    path = CorrelatedSignalPath(_lead(), beta=1.0, tracking_error_bp_per_sqrt_s=0.0)

    # Act
    ups = path.updates()

    # Assert
    assert all(path.value_at(u.t_ns) == u.value for u in ups)


def test_signal_mirrors_the_lead_times_and_jumps_scaled_by_beta():
    # Arrange
    lead = _lead()
    path = CorrelatedSignalPath(lead, beta=0.5)

    # Act
    ups, lead_ups = path.updates(), lead.updates()

    # Assert
    assert [(u.t_ns, u.is_jump) for u in ups] == [(u.t_ns, u.is_jump) for u in lead_ups]
    np.testing.assert_allclose([u.value - V0 for u in ups],
                               [0.5 * (u.value - V0) for u in lead_ups], atol=1e-9)


def test_tracking_error_is_the_one_second_sd_of_value_minus_signal():
    # Arrange: 20,000 one-second increments of V - signal; sd SE 0.5%, tolerance 3%.
    lead = _lead(duration_s=20_000)
    path = CorrelatedSignalPath(lead, beta=1.0, tracking_error_bp_per_sqrt_s=0.4, seed=1)
    seconds = np.arange(20_000) * NS_PER_S
    signal = np.array([path._signal(lead.value_at(t)) for t in seconds])
    value = np.array([path.value_at(t) for t in seconds])

    # Act
    gap_bp = np.diff(value - signal) / V0 * 1e4
    lead_bp = np.diff(signal) / V0 * 1e4

    # Assert: right size, and independent of the lead although the seed is
    # shared (|corr| < 4 / sqrt(n) = 0.028)
    assert abs(gap_bp.std() - 0.4) / 0.4 < 0.03
    assert abs(np.corrcoef(gap_bp, lead_bp)[0, 1]) < 0.028
