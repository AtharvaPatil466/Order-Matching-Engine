"""jumps: Lee–Mykland threshold, jump-robust scale and the jump/diffusion split."""
import math
import pathlib
import sys

import numpy as np

_R = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(_R / "calibration"))

from jumps import JumpAccumulator, lm_critical_z, local_sigma  # noqa: E402


def _diffusion(hours, sigma_bp, dt_ms=10, seed=0):
    """Log-mid in bp on a dt_ms grid with sigma_bp per sqrt(s)."""
    rng = np.random.default_rng(seed)
    n = int(hours * 3_600_000 / dt_ms)
    ts = np.arange(n, dtype=np.int64) * dt_ms
    steps = rng.standard_normal(n) * sigma_bp * math.sqrt(dt_ms / 1e3)
    return ts, np.cumsum(steps)


def test_lm_critical_value_grows_with_number_of_tests():
    z_1s, z_10ms = lm_critical_z(86_400), lm_critical_z(8_640_000)
    assert 5.0 < z_1s < z_10ms < 6.5


def test_local_sigma_recovers_diffusive_scale_and_ignores_a_jump():
    # Arrange: 6 h of 1 bp/sqrt(s) diffusion with one 50 bp jump in it.
    ts, lm = _diffusion(6, 1.0)
    lm[len(lm) // 2:] += 50.0

    s = local_sigma(ts, lm)

    assert abs(s - 1.0) < 0.15        # realized vol would be ~1.2 with the jump


def test_local_sigma_refuses_a_too_short_span():
    ts, lm = _diffusion(0.2, 1.0)     # 12 one-minute returns
    assert local_sigma(ts, lm) is None


def test_accumulator_finds_planted_jumps_and_splits_variance():
    # Arrange: 2 h of 0.3 bp/sqrt(s) diffusion plus 20 jumps of +-5 bp.
    ts, lm = _diffusion(2, 0.3, seed=1)
    at = np.linspace(1000, len(lm) - 1000, 20).astype(int)
    for i, k in enumerate(at):
        lm[k:] += 5.0 if i % 2 else -5.0
    acc = JumpAccumulator(10)

    acc.add(ts, lm, sigma=0.3)
    s = acc.summary()

    assert s["n_jumps"] == 20
    assert math.isclose(s["jump_intensity_per_s"], 20 / 7200, rel_tol=1e-3)
    assert abs(s["sigma_diffusive_bp_per_sqrt_s"] - 0.3) < 0.02
    assert abs(s["jump_scale_bp_laplace_mle"] - 5.0) < 0.1
    # total RV = diffusive RV + squared jumps, exactly
    tot, dif = s["sigma_total_bp_per_sqrt_s"], s["sigma_diffusive_bp_per_sqrt_s"]
    jq = s["jump_share_of_variance"] * tot ** 2
    assert math.isclose(tot ** 2, dif ** 2 + jq, rel_tol=1e-9)
