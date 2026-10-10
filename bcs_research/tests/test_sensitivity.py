"""analysis/sensitivity.py on cases with closed forms.

Ishigami (Ishigami & Homma 1990; indices from Saltelli et al. 2008, "Global
Sensitivity Analysis: The Primer", §4.9) with a = 7, b = 0.1 on U(-pi, pi)^3:
S1 = (0.3139, 0.4424, 0), ST = (0.5576, 0.4424, 0.2437). The estimator must
recover them, its bootstrap CIs must cover them, and it must equal scipy's.
"""
import sys
from pathlib import Path

_R = Path(__file__).resolve().parents[1]
if str(_R / "analysis") not in sys.path:
    sys.path.insert(0, str(_R / "analysis"))

import numpy as np  # noqa: E402
import pytest  # noqa: E402
from scipy import stats  # noqa: E402

import sensitivity as s  # noqa: E402

A_ISH, B_ISH = 7.0, 0.1
_V = A_ISH ** 2 / 8 + B_ISH * np.pi ** 4 / 5 + B_ISH ** 2 * np.pi ** 8 / 18 + 0.5
_V1 = 0.5 * (1 + B_ISH * np.pi ** 4 / 5) ** 2
_V2 = A_ISH ** 2 / 8
_V13 = B_ISH ** 2 * np.pi ** 8 * (1 / 18 - 1 / 50)
S1_TRUE = np.array([_V1, _V2, 0.0]) / _V
ST_TRUE = np.array([_V1 + _V13, _V2, _V13]) / _V


def ishigami(x):
    x = np.atleast_2d(x)
    return (np.sin(x[:, 0]) + A_ISH * np.sin(x[:, 1]) ** 2
            + B_ISH * x[:, 2] ** 4 * np.sin(x[:, 0]))


def _ishigami_outputs(n, seed=1):
    params = [s.Param(f"x{i}", -np.pi, np.pi) for i in range(3)]
    A, B = s.saltelli_design(3, n, seed)

    def f(U):
        return ishigami(np.array([[r[p.name] for p in params] for r in s.values(params, U)]))
    return f(A), f(B), np.stack([f(s.ab_matrix(A, B, i)) for i in range(3)])


def test_closed_form_constants_are_the_textbook_ishigami_indices():
    np.testing.assert_allclose(S1_TRUE, [0.3139, 0.4424, 0.0], atol=1e-4)
    np.testing.assert_allclose(ST_TRUE, [0.5576, 0.4424, 0.2437], atol=1e-4)


def test_estimator_recovers_the_ishigami_indices():
    s1, st = s.sobol_indices(*_ishigami_outputs(2 ** 14))
    np.testing.assert_allclose(s1, S1_TRUE, atol=0.02)
    np.testing.assert_allclose(st, ST_TRUE, atol=0.02)


def test_bootstrap_intervals_cover_the_true_indices_and_shrink_with_n():
    small = s.sobol_with_ci(*_ishigami_outputs(2 ** 9), names=("x0", "x1", "x2"))
    big = s.sobol_with_ci(*_ishigami_outputs(2 ** 13), names=("x0", "x1", "x2"))
    for i, name in enumerate(("x0", "x1", "x2")):
        r = big["indices"][name]
        assert r["S1_lo"] - 0.01 <= S1_TRUE[i] <= r["S1_hi"] + 0.01
        assert r["ST_lo"] - 0.01 <= ST_TRUE[i] <= r["ST_hi"] + 0.01
        width = lambda t: t["ST_hi"] - t["ST_lo"]  # noqa: E731
        assert width(big["indices"][name]) < width(small["indices"][name])


def test_estimator_matches_scipy_sobol_indices_on_the_same_outputs():
    f_A, f_B, f_AB = _ishigami_outputs(2 ** 10)
    ours = s.sobol_indices(f_A, f_B, f_AB)
    ref = stats.sobol_indices(func={"f_A": f_A[None], "f_B": f_B[None], "f_AB": f_AB[:, None]},
                              n=2 ** 10)
    np.testing.assert_allclose(ours[0], ref.first_order, rtol=1e-10, atol=1e-12)
    np.testing.assert_allclose(ours[1], ref.total_order, rtol=1e-10, atol=1e-12)


def test_design_and_bootstrap_are_deterministic_given_their_seeds():
    A1, B1 = s.saltelli_design(4, 64, seed=3)
    A2, B2 = s.saltelli_design(4, 64, seed=3)
    assert np.array_equal(A1, A2) and np.array_equal(B1, B2)
    assert not np.array_equal(A1, s.saltelli_design(4, 64, seed=4)[0])
    out = _ishigami_outputs(2 ** 8)
    assert s.sobol_with_ci(*out, names="abc") == s.sobol_with_ci(*out, names="abc")


def test_design_rejects_non_power_of_two_sizes():
    with pytest.raises(ValueError):
        s.saltelli_design(3, 100, seed=0)


def test_constant_output_has_zero_indices():
    s1, st = s.sobol_indices(np.ones(8), np.ones(8), np.ones((2, 8)))
    assert not s1.any() and not st.any()


@pytest.mark.parametrize("kind,lo,hi,u,want", [
    ("uniform", 2.0, 4.0, 0.25, 2.5),
    ("loguniform", 0.25, 4.0, 0.5, 1.0),
    ("integer", 1, 21, 0.0, 1),
    ("integer", 1, 21, 0.999999, 21),
    ("integer", 1, 5, 0.5, 3),
])
def test_param_maps_the_unit_interval_onto_its_range(kind, lo, hi, u, want):
    assert s.Param("p", lo, hi, kind).value(u) == pytest.approx(want)


def test_integer_param_is_uniform_on_its_support():
    p = s.Param("k", 1, 5, "integer")
    draws = [p.value(u) for u in (np.arange(10_000) + 0.5) / 10_000]
    assert np.bincount(draws)[1:].tolist() == [2000] * 5


def test_quantile_prior_reproduces_its_knots_and_matches_draws():
    probs, qs = [0.1, 0.25, 0.5, 0.75, 0.9, 0.99], [0.0025, 0.0095, 0.052, 0.48, 2.67, 132.7]
    ppf = s.quantile_prior(probs, qs)
    np.testing.assert_allclose(ppf(np.array(probs)), qs, rtol=1e-12)
    draws = ppf(np.random.default_rng(0).random(200_000))
    np.testing.assert_allclose(np.quantile(draws, probs), qs, rtol=0.05)
    assert ppf(0.0) < qs[0] and ppf(1.0) > qs[-1]            # tails extended, not clamped


def test_quantile_prior_rejects_non_monotone_input():
    with pytest.raises(ValueError):
        s.quantile_prior([0.1, 0.5], [2.0, 1.0])


def test_grid_indices_are_exact_on_additive_and_product_grids():
    a, b = np.array([0.0, 1.0, 2.0]), np.array([0.0, 3.0])
    add = s.grid_indices(np.add.outer(a, b), [np.ones(3), np.ones(2)])
    va, vb = np.var(a), np.var(b)
    np.testing.assert_allclose(add["first_order"], [va / (va + vb), vb / (va + vb)])
    assert add["interaction"] == pytest.approx(0.0, abs=1e-12)
    # Y = x1 * x2 with x1, x2 = +-1 equiprobable: no main effects, all interaction.
    prod = s.grid_indices(np.multiply.outer([-1.0, 1.0], [-1.0, 1.0]), [[1, 1], [1, 1]])
    assert prod["first_order"] == [0.0, 0.0] and prod["interaction"] == pytest.approx(1.0)
    # Unequal weights enter the mean and variance.
    w = s.grid_indices(np.array([0.0, 1.0])[:, None], [[3, 1], [1]])
    assert w["mean"] == pytest.approx(0.25) and w["variance"] == pytest.approx(0.1875)
