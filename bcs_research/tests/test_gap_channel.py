"""gap_channel: the exact one-sided metric, and the traced schedulers it reads.

The traced tick scheduler must reproduce exp1_primary._run exactly, or the
attribution would describe a different simulation; the traced event scheduler's
log must agree with the scheduler's own snapshots at every snapshot instant.
"""
import bisect
import sys
from pathlib import Path

_R = Path(__file__).resolve().parents[1]
for _d in ("build", "simulation", "agents", "metrics", "experiments"):
    if str(_R / _d) not in sys.path:
        sys.path.insert(0, str(_R / _d))

import gap_channel as g  # noqa: E402
from exp1_primary import CFG, _run  # noqa: E402


def test_episodes_skip_the_unquoted_start_and_zero_length_flickers():
    states = [(0, 0, 0), (5, 10, 0), (10, 10, 20),       # quoted from t = 10
              (12, 0, 20), (12, 10, 20),                 # emptied and refilled at once
              (15, 10, 0), (18, 10, 20),                 # a 3-long gap
              (30, 0, 20)]                               # open at the end
    assert g.one_sided_episodes(states, 40) == [(15, 18, True), (30, 40, False)]
    assert g.episode_stats(states, 40, 40) == (1, 3 / 40)


def test_episodes_empty_when_never_quoted_or_always_two_sided():
    assert g.one_sided_episodes([(0, 0, 0), (9, 5, 0)], 20) == []
    assert g.one_sided_episodes([(0, 5, 9), (4, 6, 9)], 20) == []


def test_traced_tick_run_reproduces_exp1_primary():
    from run_calibrated import calibrated_cfg
    for k, over in ((0, {}), (3, {}), (2, calibrated_cfg())):
        row = g.tick_run(2, k, cfg=over)
        ref = _run(2, {**CFG, **over, "n_hft": k}, with_hft=k > 0)
        assert row["gaps"] == ref["liquidity_gaps"]
        assert row["hft_rent"] == ref["hft_rent"] and row["mm_pnl"] == ref["mm_pnl"]
        assert row["hft_fills"] == ref["hft_fills"]


def test_every_one_sided_snapshot_gets_exactly_one_cause():
    row = g.tick_run(1, 8)
    causes = sum(v for key, v in row.items() if key.startswith("cause_"))
    assert causes == row["one_sided_snapshots"] > 0


def test_shifted_snipers_never_land_in_their_decision_tick():
    row = g.tick_run(1, 8, arm="shifted")
    assert row["zero_jitter_iocs"] == 0 and row["cause_hft_zero_jitter"] == 0
    assert g.tick_run(1, 8)["zero_jitter_iocs"] > 0


def test_event_book_log_matches_the_schedulers_snapshots():
    import exp11_batch_semantics as x11
    import bcs_engine as be
    cfg = {**g.event_cfg("slow"), "n_snipers": 3, "duration_s": 2}
    agents = x11.make_agents(cfg, 1, "stale")
    path = x11.make_path(cfg, 1)
    h = be.EngineHarness()
    h.add_symbol(1)
    h.start(disable_circuit_breaker=True)
    sched = g.TracedEventScheduler(h, 1, seed=1, snapshot_every_ns=cfg["snapshot_every_ns"],
                                   race_jitter_ns={a.participant_id: cfg["jitter_ns"] for a in agents})
    res = sched.run(agents, path)
    h.stop()
    times = [t for t, _, _ in sched.book_log]
    for s in res.snapshots:
        i = bisect.bisect_right(times, s.t_ns) - 1
        assert (sched.book_log[i][1:] if i >= 0 else (0, 0)) == (s.best_bid, s.best_ask)


def test_event_run_returns_both_gap_measures():
    row = g.event_run(1, 3, "lag0", duration_s=2)
    assert row["episodes_exact"] > 0 and 0 < row["one_sided_share_exact"] < 1
    assert row["gaps_snapshot_detector"] >= 0
