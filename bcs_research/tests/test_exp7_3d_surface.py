"""Step-6 surface checks: the latency schemes and the per-arm baseline.

Two things can silently corrupt the 3D surface, and they pull in opposite
directions from exp6's.

First, exp6 computes its no-HFT baseline ONCE for all 63 cells because the
baseline is provably invariant to `hft_qty` and `n_hft`
(test_exp6_incidence.py). That invariance does NOT extend to the maker-count
axis: adding makers changes the no-HFT arm itself, so exp7 must recompute the
baseline per arm. If a future edit "optimises" that back to a shared baseline,
every delta at M > 1 is differenced against the wrong control and the incidence
labels become meaningless. `test_baseline_depends_on_maker_latencies` pins it.

Second, the M=1 column is advertised as nesting exp6 exactly, and the whole
validity argument of §4.8(a) rests on that. It holds only if M=1 resolves to the
paper's single 3-tick maker under BOTH latency schemes — a `zero_fast` that
moved the lone maker to 0 us would silently re-specify the baseline column it is
supposed to reproduce.
"""
import sys
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[1]
for _d in ("build", "agents", "simulation", "metrics", "experiments"):
    _p = str(_ROOT / _d)
    if _p not in sys.path:
        sys.path.insert(0, _p)

from exp1_primary import CFG, _run, _split_qty        # noqa: E402
from exp4_competing_makers import latency_set         # noqa: E402
from exp7_3d_surface import maker_latencies           # noqa: E402


def test_m1_is_the_papers_single_maker_under_both_schemes():
    """The nesting guarantee of §4.8(a), pinned at the config layer."""
    assert maker_latencies(1, "calibrated") == [3000]
    assert maker_latencies(1, "zero_fast") == [3000]
    assert maker_latencies(1, "calibrated") == latency_set(1)


def test_calibrated_scheme_is_exactly_section_46s_set():
    """M > 1 calibrated arms must not re-specify §4.6's published latencies."""
    for m in (2, 3, 5):
        assert maker_latencies(m, "calibrated") == latency_set(m)
    assert maker_latencies(5, "calibrated") == [1000, 2000, 3000, 4000, 5000]


def test_zero_fast_moves_only_the_fastest_maker():
    """The brief's variant: fastest to 0 us, every other quantile untouched."""
    for m in (2, 3, 5):
        got = maker_latencies(m, "zero_fast")
        assert got[0] == 0
        assert got[1:] == latency_set(m)[1:]
        assert len(got) == m


def test_unknown_scheme_is_rejected():
    """A typo'd scheme must fail loudly, not silently fall back to calibrated."""
    with pytest.raises(ValueError):
        maker_latencies(3, "fastest_first")


def test_baseline_depends_on_maker_latencies():
    """Why exp7 cannot reuse one baseline across arms, unlike exp6 across cells.

    The no-HFT arm is invariant to the snipe knobs (exp6's shortcut) but NOT to
    the maker set, so each (M, scheme) arm needs its own control.
    """
    base_cfg = {**CFG, "n_hft": 0}
    one = _run(1, {**base_cfg, "mm_latencies_us": [3000]}, with_hft=False)
    three = _run(1, {**base_cfg, "mm_latencies_us": [1000, 3000, 5000]}, with_hft=False)
    assert one["mm_pnl"] != three["mm_pnl"]

    # ...and the two latency schemes at the same M are genuinely different arms,
    # so both must be run rather than one being inferred from the other.
    zero_fast = _run(1, {**base_cfg, "mm_latencies_us": [0, 3000, 5000]},
                     with_hft=False)
    assert zero_fast["mm_pnl"] != three["mm_pnl"]


def test_maker_count_does_not_change_aggregate_quoted_depth():
    """The swept ratio stays a snipe-against-aggregate-depth measure at every M.

    This is what lets §4.8 read the tape-measured ratio and the simulated ratio
    as the same object (§5.2's sixth limit). If `_split_qty` ever stopped
    conserving the total, the ratio axis would silently rescale with M.
    """
    for m in (1, 2, 3, 5):
        assert sum(_split_qty(2061, m)) == 2061
