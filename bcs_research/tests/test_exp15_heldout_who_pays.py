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


def test_generator_starts_with_p10s_list_and_avoids_the_papers_sets():
    cs = x.generate_configs(30)
    assert len(cs) == 39 and [tuple(int(t) for t in c["ticks"]) for c in cs[:9]] == list(x.P10_LISTED)
    assert [c["closely_spaced"] for c in cs[:9]] == [True, True, True] + [False] * 6
    for c in cs:
        assert len(set(c["ticks"])) == len(c["ticks"]) and frozenset(c["ticks"]) not in x.PAPER_CONFIGS
        assert sorted(c["slots"]) == list(range(len(c["ticks"])))
        assert c["k"] in x.K_GRID and c["ratio"] in x.TAPE_BAND
        if c["family"] == "close":
            assert c["closely_spaced"]
    assert {len(c["ticks"]) for c in cs[9:]} == set(x.MAKER_COUNTS)
    assert x.generate_configs(30) == cs                     # fixed generator seed


def test_a_matched_pair_maps_latencies_to_slots_and_scores():
    c = {**x.generate_configs(0)[4], "index": 4}           # {1, 7} ticks
    cfg = {**x.market_cfg(), "duration_s": 2}
    row = x.run_config_seed(c, 4001, cfg)
    assert row["latency_ns"] == [int(c["ticks"][r] * x.TICK_NS) for r in c["slots"]]
    assert len(row["delta"]) == 2 and row["edge_off"] != row["edge_on"]
    s = x.score_config(c, [row, row])
    assert s["hit"] == x.is_hit(row["latency_ns"], row["delta"]) and s["n_seeds"] == 2


# --- P10b: the ex-ante queue-front predictor -----------------------------------

MS = 1_000_000
D = 10 * MS            # the operating point's update cadence


def test_queue_front_share_is_the_gap_before_each_makers_phase():
    # Phases 3 and 7 ms: a snipe landing in (7, 13] ms finds maker 0 at the front
    # (its burst is due next), one in (3, 7] finds maker 1.
    assert x.queue_front_shares([203 * MS, 207 * MS], D) == pytest.approx([0.6, 0.4])
    assert x.queue_front_payer([203 * MS, 207 * MS], D) == 0
    # Three makers at phases 0, 1, 5 ms: arcs 5, 1, 4 ms. Latency rank is irrelevant.
    assert x.queue_front_shares([300 * MS, 101 * MS, 205 * MS], D) == pytest.approx([0.5, 0.1, 0.4])
    assert x.queue_front_payer([300 * MS, 101 * MS, 205 * MS], D) == 0


def test_equal_phases_split_their_gap_and_a_top_tie_names_no_one():
    # P10's integer-tick sets: 2 x ticks x 100 ms is a multiple of 10 ms, one phase.
    assert x.queue_front_shares([200 * MS, 400 * MS, 600 * MS], D) == pytest.approx([1 / 3] * 3)
    assert x.queue_front_payer([200 * MS, 400 * MS], D) is None
    # Two tied at phase 0 share the 5 ms arc before them; the maker at 5 ms leads alone.
    assert x.queue_front_shares([200 * MS, 400 * MS, 205 * MS], D) == pytest.approx([0.25, 0.25, 0.5])
    assert x.queue_front_payer([200 * MS, 400 * MS, 205 * MS], D) == 2


def test_a_maker_that_cancels_before_the_snipe_is_off_the_stale_touch():
    assert x.queue_front_shares([50_000, 205 * MS], D, sniper_latency_ns=100_000,
                                jitter_ns=20_000) == pytest.approx([0.0, 1.0])
    with pytest.raises(ValueError):           # its cancel races the snipe: undefined here
        x.queue_front_shares([100_000, 205 * MS], D, sniper_latency_ns=100_000, jitter_ns=80_000)


def _first_snipe_shares(one_way_ns, seed=1):
    """Who the scheduler puts at the front: each jump's first filled snipe's maker."""
    import bcs_engine as be
    from event_scheduler import EventScheduler
    from event_sniper import EventSniper
    from fundamental_paths import JumpDiffusionPath
    from strategic_maker import MakerPolicy, StrategicMaker
    path = JumpDiffusionPath(1_000_000, 0.0, D, 60 * 1_000 * MS, seed=seed,
                             jump_intensity_per_s=5.0, jump_scale=1000.0)
    makers = [StrategicMaker(1 + i, MakerPolicy(half_spread=50, qty=10, post_only=True),
                             md_latency_ns=lat, order_latency_ns=lat)
              for i, lat in enumerate(one_way_ns)]
    agents = makers + [EventSniper(50, qty=1, md_latency_ns=5_000, order_latency_ns=5_000)]
    h = be.EngineHarness()
    h.add_symbol(1)
    h.start(disable_circuit_breaker=True)
    res = EventScheduler(h, 1, seed=seed).run(agents, path)
    h.stop()
    counts = [0] * len(makers)
    for r in res.races:
        first = next((a for a in r.arrivals if a.participant_id == 50
                      and a.outcome in ("filled", "partial")), None)
        if first is not None:
            tr = next(t for t in res.trades if t.t_ns == first.arrival_ns)
            counts[(tr.seller_id if tr.buyer_id == 50 else tr.buyer_id) - 1] += 1
    return [c / sum(counts) for c in counts]


def test_queue_front_shares_match_the_scheduler():
    """The derivation checked against the engine: ~280 jumps, binomial sd ~0.03."""
    for one_way in ((1_500_000, 3_500_000), (1_000_000, 500_000, 2_500_000)):
        predicted = x.queue_front_shares([2 * v for v in one_way], D, sniper_latency_ns=10_000)
        assert _first_snipe_shares(one_way) == pytest.approx(predicted, abs=0.08)


# --- confirmatory mode ----------------------------------------------------------

def _scored(lat, deltas, close=False, payer=None):
    return {"latencies": lat, "deltas": deltas, "closely_spaced": close, "p10b_payer": payer}


def test_p10b_counts_a_tie_as_not_right_and_reports_the_named_rate_beside():
    right = [_scored([1, 2], [-5.0, 1.0], payer=0)] * 9
    tied = [_scored([1, 2], [-5.0, 1.0], payer=None)]
    c = x.classify_p10b(right + tied)
    assert c["hit_rate"]["k"] == 9 and c["hit_rate"]["n"] == 10
    assert c["n_no_prediction"] == 1 and c["hit_rate_where_named"]["n"] == 9
    assert x.classify_p10b([_scored([1, 2], [-5.0, 1.0], payer=1)] * 20)["verdict"] == "MISS"


def test_tick_secondary_band_and_miss_lines():
    slow_pays = _scored([1, 2], [1.0, -5.0])                  # a hit: the slowest pays
    close_hit = _scored([1, 2], [1.0, -5.0], close=True)
    close_miss = _scored([1, 2], [-5.0, 1.0], close=True)
    # 80% overall, closely spaced all wrong: inside [0.60, 0.95] and below 0.50
    assert x.classify_tick([slow_pays] * 160 + [close_miss] * 40)["verdict"] == "HIT"
    # closely spaced all right: ">= 70% on closely spaced" is the miss line
    assert x.classify_tick([slow_pays] * 30 + [close_hit] * 10)["verdict"] == "MISS"
    assert x.classify_tick([_scored([1, 2], [-5.0, 1.0])] * 40)["verdict"] == "MISS"


def test_confirmatory_runs_one_event_and_one_tick_cell():
    cfg = x.confirm_configs(0)[0]                          # P10's {1, 2}
    seed = 1
    on = x.run_market_op(cfg, seed, 0.0049, horizon_s=2)
    off = x.run_market_op(cfg, seed, None, horizon_s=2)
    assert len(on["edge"]) == len(off["edge"]) == 2 and on["latency_ns"] == off["latency_ns"]
    assert sorted(on["latency_ns"]) == [100_000_000, 200_000_000]
    t = x.run_tick(cfg, seed, 10)
    assert len(t["maker_pnls"]) == 2 and sorted(t["latency_us"]) == [1000.0, 2000.0]
    assert x.p10b_payer(cfg) is None                        # integer ticks share one phase
