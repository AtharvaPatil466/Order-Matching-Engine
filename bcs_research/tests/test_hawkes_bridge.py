"""Unit tests for the Hawkes-bridge pure helpers.

The bridge maps a BCS simulation tape to (a) rolling Hawkes branching ratios and
(b) a pre-gap/normal labelling, then a Mann-Whitney U test. The numerically
delicate calibrator is tested separately; here we test the deterministic glue:
de-tying intra-tick trade timestamps, the pre-gap classification window logic,
and the rank-biserial effect size.
"""
import sys
import pathlib

_R = pathlib.Path(__file__).resolve().parents[1]
for _d in ("analysis", "hawkes", "metrics"):
    sys.path.insert(0, str(_R / _d))

import numpy as np
import pytest

from hawkes_bridge import detie_trade_times_us, classify_pre_gap, rank_biserial


# --- de-tie intra-tick timestamps --------------------------------------------

def test_detie_spreads_equal_timestamps_within_tick():
    out = detie_trade_times_us([1000, 1000, 1000, 3000], dt_us=1000)
    assert out[0] == pytest.approx(1000.0)
    assert 1000.0 < out[1] < out[2] < 2000.0      # spread inside the tick
    assert out[3] == pytest.approx(3000.0)
    assert np.all(np.diff(out) > 0)               # strictly increasing overall


def test_detie_preserves_distinct_timestamps():
    out = detie_trade_times_us([1000, 2000, 3000], dt_us=1000)
    assert list(out) == [1000.0, 2000.0, 3000.0]


def test_detie_empty():
    assert len(detie_trade_times_us([], dt_us=1000)) == 0


# --- pre-gap classification --------------------------------------------------

def test_classify_window_ending_before_gap_is_pre_gap():
    # window ends at 1000us, a gap starts at 1200us (within 500us lead) -> pre-gap
    branching = [(1000, 0.8), (5000, 0.3)]
    pre, normal = classify_pre_gap(branching, gap_starts_us=[1200], lead_us=500)
    assert pre == [0.8]
    assert normal == [0.3]


def test_classify_gap_after_window_is_not_pre_gap_if_beyond_lead():
    branching = [(1000, 0.8)]
    pre, normal = classify_pre_gap(branching, gap_starts_us=[2000], lead_us=500)
    assert pre == []
    assert normal == [0.8]


def test_classify_gap_at_or_before_window_end_is_not_pre_gap():
    # a gap already started at/under the window end is not "preceding" it
    branching = [(1000, 0.8)]
    pre, normal = classify_pre_gap(branching, gap_starts_us=[1000], lead_us=500)
    assert pre == []
    assert normal == [0.8]


def test_classify_no_gaps_all_normal():
    pre, normal = classify_pre_gap([(1000, 0.5), (2000, 0.6)], gap_starts_us=[], lead_us=500)
    assert pre == []
    assert normal == [0.5, 0.6]


# --- rank-biserial effect size -----------------------------------------------

def test_rank_biserial_one_when_group1_dominates():
    assert rank_biserial([5.0, 6.0, 7.0], [1.0, 2.0, 3.0]) == pytest.approx(1.0)


def test_rank_biserial_minus_one_when_group1_dominated():
    assert rank_biserial([1.0, 2.0, 3.0], [5.0, 6.0, 7.0]) == pytest.approx(-1.0)


def test_rank_biserial_zero_for_identical_distributions():
    assert rank_biserial([1.0, 2.0, 3.0], [1.0, 2.0, 3.0]) == pytest.approx(0.0)
