"""Step-5 surface checks: the baseline-reuse shortcut and the incidence classifier.

Two things can silently corrupt the incidence surface. First, the surface
computes the no-HFT baseline ONCE and differences all 49 cells against it — a
~2x cost saving that is only valid if the baseline really is invariant to
`hft_qty` and `n_hft`. If it is not, every delta in the surface is wrong and the
boundary is meaningless. Second, the boundary is a claim about the SIGN of a
noisy quantity, so a cell whose CI brackets zero must not be silently counted as
a gain. These tests pin both.
"""
import sys
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
for _d in ("build", "agents", "simulation", "metrics", "experiments"):
    _p = str(_ROOT / _d)
    if _p not in sys.path:
        sys.path.insert(0, _p)

from exp1_primary import CFG, _run                              # noqa: E402
from exp6_incidence_surface import (                            # noqa: E402
    QTY_GRID, classify_incidence, find_boundary,
)


def _cell(n_hfts, mean, lower, upper):
    """A minimal cell shaped like run_cell's output, for classifier tests."""
    return {"n_hfts": n_hfts,
            "delta_ci": {"mm_pnl": {"mean": mean, "lower": lower, "upper": upper}}}


def test_baseline_is_invariant_to_snipe_size_and_hft_count():
    """The shortcut the whole surface rests on: no-HFT arm ignores both knobs.

    If this ever fails, `baseline_rows` must move inside the cell loop.
    """
    ref = _run(1, {**CFG, "hft_qty": 50, "n_hft": 3}, with_hft=False)
    for hft_qty, n_hft in ((10, 1), (2061, 21), (700, 8)):
        other = _run(1, {**CFG, "hft_qty": hft_qty, "n_hft": n_hft}, with_hft=False)
        assert other == ref


def test_indeterminate_cells_are_not_counted_as_gains():
    """A CI straddling zero is its own verdict, whatever the point estimate."""
    assert classify_incidence(_cell(1, 1327.0, 900.0, 1700.0)) == "maker_gains"
    assert classify_incidence(_cell(21, -5213.0, -6000.0, -4400.0)) == "maker_pays"
    assert classify_incidence(_cell(8, 120.0, -300.0, 540.0)) == "indeterminate"
    assert classify_incidence(_cell(8, -120.0, -540.0, 300.0)) == "indeterminate"


def test_boundary_brackets_the_flip_and_never_interpolates():
    """§4.6's reported pattern — gains to k<=5, pays by k>=8 — brackets as 5/8."""
    cells = [_cell(1, 1327.0, 900.0, 1700.0),
             _cell(3, 800.0, 400.0, 1200.0),
             _cell(5, 300.0, 100.0, 500.0),
             _cell(8, -1000.0, -1400.0, -600.0),
             _cell(21, -5213.0, -6000.0, -4400.0)]
    b = find_boundary(cells)
    assert b == {"k_last_maker_gains": 5, "k_first_not_gains": 8,
                 "flips_to": "maker_pays"}


def test_no_flip_returns_none_rather_than_a_fabricated_boundary():
    """The ratio-1.0 row should find no flip: the maker pays at every k."""
    cells = [_cell(k, -1000.0 * k, -1200.0 * k, -800.0 * k) for k in (1, 3, 5, 8, 21)]
    assert find_boundary(cells) is None


def test_grid_nests_the_two_published_cells_exactly():
    """50 is §4.6's robustness arm, 2061 its main arm (== calibrated quote_qty)."""
    assert 50 in QTY_GRID
    assert 2061 in QTY_GRID
