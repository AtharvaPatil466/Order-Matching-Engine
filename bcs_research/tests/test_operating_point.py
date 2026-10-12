"""The operating point equals PREREGISTRATION §5, and build_market builds it.

Every confirmatory driver imports its values from operating_point.py, so a
value that drifted there would drift everywhere at once. Each §5 number is
pinned here at the precision §5 prints it.
"""
import sys
from pathlib import Path

_R = Path(__file__).resolve().parents[1]
if str(_R / "experiments") not in sys.path:
    sys.path.insert(0, str(_R / "experiments"))

import pytest  # noqa: E402

import operating_point as op  # noqa: E402


def test_values_equal_section_5():
    p = op.PATH
    assert p["update_interval_ms"] == 10
    assert round(p["sigma_bp_per_sqrt_s"], 3) == 0.242
    assert round(p["jump_intensity_per_s"], 3) == 0.200
    assert round(p["jump_scale_bp"], 3) == 0.705
    assert p["jump_size_dist"] == "laplace"
    assert round(op.V0) == 630_927
    assert op.TICK_BP * op.BP == pytest.approx(1.0)        # one engine unit is one tick
    assert round(op.INVESTOR_RATE_PER_S, 1) == 16.3
    assert op.INVESTOR_MEDIAN_LOTS == 3
    assert op.QUOTE_LOTS == 6_180
    assert op.PAPER_BAND == (0.0049, 0.0097) and op.TAPE_BAND == (0.052, 0.18)
    assert op.SNIPER_LATENCY_NS == 100_000 and op.JITTER_NS == 80_000
    assert (op.FEES.maker_bps, op.FEES.taker_bps) == (0.0, 1.7)
    assert op.HORIZON_S == 30 * 60 and op.N_SEEDS == 100
    assert op.K_GRID == (1, 2, 3, 5, 6, 7, 8, 13, 21)
    assert op.CONFIRMATORY_STATUS == ("confirmatory — pre-registration signed off "
                                      "2026-10-12 (6c46c01)")


def test_snipe_size_is_ratio_times_quote():
    assert [op.snipe_lots(r) for r in op.PAPER_BAND] == [30, 60]
    assert [op.snipe_lots(r) for r in op.TAPE_BAND] == [321, 1112]
    assert op.snipe_lots(1.0) == op.QUOTE_LOTS
    with pytest.raises(ValueError):
        op.snipe_lots(0)


def test_build_market_wires_the_prediction_knobs():
    mk = op.build_market(7, maker_latencies_ns=(50_000, 300_000), k=5, snipe_ratio=0.18,
                         horizon_s=1)
    assert [m.md_latency_ns for m in mk.makers] == [50_000, 300_000]
    assert [m.order_latency_ns for m in mk.makers] == [50_000, 300_000]
    assert sum(m.policy.qty for m in mk.makers) == op.QUOTE_LOTS
    assert len(mk.snipers) == 5
    assert {s.qty for s in mk.snipers} == {1112}
    assert {s.md_latency_ns + s.order_latency_ns for s in mk.snipers} == {op.SNIPER_LATENCY_NS}
    assert mk.snipers[0].threshold == pytest.approx(1.7 * op.BP)
    racers = {a.participant_id for a in mk.makers + mk.snipers}
    assert mk.race_jitter_ns == dict.fromkeys(racers, op.JITTER_NS)   # investors do not race
    assert mk.agents[-1] is mk.noise and mk.noise.participant_id not in mk.race_jitter_ns
    assert op.build_market(7, k=0, horizon_s=1).snipers == []


def test_build_market_rejects_mismatched_policies():
    with pytest.raises(ValueError):
        op.build_market(1, maker_latencies_ns=(1, 2), policies=[op.default_policy()], horizon_s=1)


def test_arms_of_one_seed_share_path_and_investors():
    a = op.simulate(3, k=3, horizon_s=5)
    b = op.simulate(3, k=0, horizon_s=5)
    assert [u.value for u in a.path.updates()] == [u.value for u in b.path.updates()]
    na = next(x for x in a.agents if x.participant_id == op.NOISE)
    nb = next(x for x in b.agents if x.participant_id == op.NOISE)
    assert na.arrivals == nb.arrivals > 0
    edge = op.edge_by_participant(a.trades, a.path)
    assert abs(sum(edge.values())) < 1e-9


@pytest.mark.parametrize("lo,hi,verdict", [(0.1, 0.2, "HIT"), (0.3, 0.4, "MISS"),
                                           (-0.5, -0.1, "MISS"), (0.0, 0.3, "INCONCLUSIVE")])
def test_band_verdict(lo, hi, verdict):
    assert op.band_verdict(lo, hi, 0.0, 0.25) == verdict


def test_wilson_matches_known_interval():
    w = op.wilson(8, 10)
    assert w["mean"] == 0.8
    assert w["lower"] == pytest.approx(0.4902, abs=1e-4)
    assert w["upper"] == pytest.approx(0.9433, abs=1e-4)


def test_cell_log_resumes_and_ignores_a_torn_line(tmp_path):
    p = tmp_path / "ck.jsonl"
    log = op.CellLog(p)
    log.put("a", {"x": 1})
    with p.open("a") as fh:
        fh.write('{"key": "b", "val')            # killed mid-write
    again = op.CellLog(p)
    assert "a" in again and again.get("a") == {"x": 1} and "b" not in again


def _square(x):
    return x * x


def test_run_cached_skips_logged_keys_and_logs_new_ones(tmp_path):
    log = op.CellLog(tmp_path / "ck.jsonl")
    log.put("a", -1)                                  # resumed: must not be recomputed
    out = op.run_cached(_square, {"a": (2,), "b": (3,)}, log, workers=1)
    assert out == {"a": -1, "b": 9}
    assert op.CellLog(tmp_path / "ck.jsonl").get("b") == 9


def test_smoke_status_is_confirmatory_only_at_full_size():
    assert op.smoke_status(100, 1800) == op.CONFIRMATORY_STATUS
    assert op.smoke_status(2, 60).startswith("smoke — NOT confirmatory")
