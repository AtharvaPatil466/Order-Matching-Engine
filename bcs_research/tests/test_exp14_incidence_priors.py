"""Experiment 14's priors match their sources, and its summaries are right on
synthetic cell grids whose answers are known."""
import json
import sys
from pathlib import Path

_R = Path(__file__).resolve().parents[1]
for _d in ("build", "simulation", "agents", "metrics", "analysis", "calibration", "experiments"):
    if str(_R / _d) not in sys.path:
        sys.path.insert(0, str(_R / _d))

import numpy as np  # noqa: E402
import pytest  # noqa: E402

import exp14_incidence_priors as x  # noqa: E402

PROXY = json.loads((_R / "results" / "calibration" / "race_proxy_v2.json").read_text())


@pytest.mark.parametrize("source", sorted(x.RATIO_SOURCES))
def test_ratio_prior_draws_match_the_tapes_published_quantiles(source):
    src = PROXY["by_window_ms"]["0"][x.RATIO_SOURCES[source]]
    probs = [float(k[1:]) / 100 for k in x.QUANTILE_KEYS]
    want = [src[k] for k in x.QUANTILE_KEYS]
    draws = x.ratio_prior(source)(np.random.default_rng(1).random(200_000))
    np.testing.assert_allclose(np.quantile(draws, probs), want, rtol=0.05)


def test_ratio_medians_are_the_two_headline_values():
    assert x.ratio_prior("tape")(0.5) == pytest.approx(0.0517, abs=1e-4)
    assert x.ratio_prior("fresh_snapshot")(0.5) == pytest.approx(0.1786, abs=1e-4)


def test_ratio_nodes_are_equal_probability_bin_midpoints():
    nodes = x.ratio_nodes("tape", 4)
    ppf = x.ratio_prior("tape")
    np.testing.assert_allclose(nodes, ppf(np.array([0.125, 0.375, 0.625, 0.875])))
    assert np.all(np.diff(nodes) > 0)


def test_fee_tiers_run_from_vip0_to_vip9_and_never_rise():
    assert len(x.FEE_TIERS) == 10
    assert x.FEE_TIERS[0] == (2.0, 5.0) and x.FEE_TIERS[9] == (0.0, 1.7)
    maker, taker = np.array(x.FEE_TIERS).T
    assert np.all(np.diff(maker) <= 0) and np.all(np.diff(taker) <= 0)
    assert x.TIER_PRIOR.sum() == pytest.approx(1.0) and x.MAKER_PRIOR.sum() == pytest.approx(1.0)
    assert x.fee_table(9) == {"maker": (0.0, 1.7), "sniper": (0.0, 1.7), "noise": (2.0, 5.0)}


def test_point_sets_the_snipers_threshold_to_their_tiers_taker_fee():
    import exp13_global_sensitivity as x13
    from event_sniper import EventSniper
    agents, _, _ = x13.build(x.point(0.05, 2, 9, seed=3), 1.0, True)
    snipers = [a for a in agents if isinstance(a, EventSniper)]
    assert len(snipers) == x13.BASE_POINT["n_snipers"]
    assert snipers[0].threshold == pytest.approx(1.7 / x13.TICK_BP)
    assert snipers[0].qty == round(0.05 * x13.DEPTH_LOTS)


def test_weighted_quantile_on_a_two_point_distribution():
    assert x.weighted_quantile([0.0, 10.0], [3, 1], [0.1, 0.5, 0.95]) == pytest.approx(
        [0.0, 10 * (0.5 - 0.375) / 0.5, 10.0])


def _synthetic(n_nodes=2, n_seeds=3):
    """Tax only at the top tier, borne wholly by makers; scaled by the ratio node."""
    shape = (n_nodes, len(x.MAKER_COUNTS), len(x.FEE_TIERS), n_seeds)
    tax = np.zeros(shape)
    tax[:, :, -1, :] = (1.0 + np.arange(n_nodes))[:, None, None]
    zeros = np.zeros(shape)
    return {"sniper_rent": tax, "maker_delta": -tax, "noise_delta": zeros, "tax": tax,
            "gap_rate": zeros, "sniper_fills": (tax > 0).astype(float), "identity_residual": zeros}


def test_summary_on_a_grid_where_makers_bear_all_of_a_top_tier_tax():
    r = x.analyse_source(_synthetic(), n_nodes=2)
    s = r["summary"]
    assert s["p_makers_bear_majority"] == pytest.approx(0.1)        # the VIP 9 mass
    assert s["p_no_snipe"] == pytest.approx(0.9) and s["p_noise_bears_majority"] == 0.0
    assert s["maker_share_of_mean_tax"] == pytest.approx(1.0)
    assert s["maker_share"] == pytest.approx([1.0] * len(x.QS))
    d = r["spread_drivers_first_order"]["tax"]
    assert d["n_makers"] == pytest.approx(0.0, abs=1e-12)
    assert d["fee_tier"] > d["ratio"] > 0 and d["interaction"] > 0
    assert r["by_fee_tier"]["VIP9"]["tax"] == pytest.approx(1.5)
    assert r["by_fee_tier"]["VIP0"]["tax"] == 0.0


def test_cells_without_a_snipe_carry_no_incidence_whatever_their_deltas():
    a = _synthetic()
    a["noise_delta"] = a["noise_delta"].copy()
    a["noise_delta"][:, :, 0, :] = 0.7          # stream noise at VIP 0, where no one snipes
    a["tax"] = -(a["maker_delta"] + a["noise_delta"])
    s = x.analyse_source(a, n_nodes=2)["summary"]
    assert s["p_makers_bear_majority"] == pytest.approx(0.1)
    assert s["p_noise_bears_majority"] == 0.0 and s["p_no_snipe"] == pytest.approx(0.9)
    assert s["maker_share_of_mean_tax"] == pytest.approx(1.0)


def test_seed_bootstrap_is_deterministic_and_zero_width_without_seed_noise():
    a, b = x.analyse_source(_synthetic(), 2), x.analyse_source(_synthetic(), 2)
    assert a == b
    ci = a["bootstrap_ci"]["mean_tax"]
    assert ci["lo"] == pytest.approx(ci["hi"])


# --- confirmatory mode (descriptive) ------------------------------------------------

def test_confirmatory_runs_at_the_operating_point_and_says_it_has_no_prediction(tmp_path):
    import exp14_incidence_priors as x14
    base = x14.confirm_base()
    assert (base["sniper_lat_ns"], base["jitter_ns"], base["depth_lots"]) == (100_000, 80_000, 6_180)
    assert base["lag_over_eps"] == 0.0
    r = x14.confirmatory(["--confirmatory", "--seeds", "1", "--horizon-s", "1", "--nodes", "1",
                          "--workers", "1", "--out-dir", str(tmp_path)])
    assert r["status"].startswith("smoke") and r["prediction"].startswith("none")
    assert set(r["sources"]) == set(x14.RATIO_SOURCES)
    assert r["max_identity_residual"] < 1e-6
