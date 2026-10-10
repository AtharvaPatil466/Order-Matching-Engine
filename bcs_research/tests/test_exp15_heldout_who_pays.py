"""exp15: P10's predictor, its statistics and its decision rule.

The predictor and the rule are what the held-out test commits to in advance,
so each branch of the rule is pinned on hand-made configurations.
"""
import sys
from pathlib import Path

_R = Path(__file__).resolve().parents[1]
for _d in ("build", "simulation", "agents", "metrics", "experiments"):
    if str(_R / _d) not in sys.path:
        sys.path.insert(0, str(_R / _d))

import pytest  # noqa: E402

import exp15_heldout_who_pays as x  # noqa: E402


def test_the_slowest_maker_is_the_predicted_payer():
    assert x.predicted_payer([300, 100, 200]) == 0
    assert x.is_hit([100, 300, 200], [5.0, -9.0, -1.0])
    assert not x.is_hit([100, 300, 200], [-9.0, 5.0, -1.0])
    with pytest.raises(ValueError):
        x.predicted_payer([100, 100])


def test_spearman_sign_follows_rank_order():
    assert x.spearman([1, 2, 3], [3.0, 2.0, 1.0]) == pytest.approx(-1.0)
    assert x.spearman([1, 2], [1.0, 2.0]) == pytest.approx(1.0)
    assert x.spearman([1, 2, 3], [0.0, 0.0, 0.0]) == 0.0


def test_wilson_matches_the_textbook_values():
    w = x.wilson(8, 10)
    assert (round(w["lower"], 4), round(w["upper"], 4)) == (0.4902, 0.9433)
    assert x.wilson(40, 40)["lower"] == pytest.approx(0.9124, abs=1e-4)


def test_chance_pvalue_is_the_poisson_binomial_tail():
    assert x.chance_pvalue(2, [2, 2]) == pytest.approx(0.25)
    assert x.chance_pvalue(0, [2, 3]) == pytest.approx(1.0)
    assert x.chance_pvalue(2, [2, 3]) == pytest.approx(1 / 6)


def _configs(n_hit, n_miss, close=False):
    hit = {"latencies": [1, 2], "deltas": [1.0, -1.0], "closely_spaced": close}
    miss = {"latencies": [1, 2], "deltas": [-1.0, 1.0], "closely_spaced": close}
    return [hit] * n_hit + [miss] * n_miss


def test_rule_hit_needs_the_interval_above_the_band():
    assert x.classify(_configs(40, 0))["verdict"] == "HIT"
    out = x.classify(_configs(9, 0))          # 9/9: lower bound 0.70
    assert (out["verdict"], out["verdict_point_estimate"]) == ("INCONCLUSIVE", "HIT")


def test_rule_miss_on_overall_or_on_the_closely_spaced_sets():
    assert x.classify(_configs(10, 30))["verdict"] == "MISS"
    mixed = _configs(40, 0) + _configs(0, 12, close=True)
    assert x.classify(mixed)["closely_spaced_hit_rate"]["upper"] < 0.5
    assert x.classify(mixed)["verdict"] == "MISS"
