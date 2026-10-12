"""Exp 16 (P2): the race and gap metrics, every verdict rule, and determinism.

The confirmatory run is only as good as: the race-log reading that decides
"the maker's cancel arrived first"; the grid gap count, which must equal §3.3's
detector on the snapshots it replaces; and the four verdict rules, each checked
here on hand-made inputs that must come out HIT, MISS and INCONCLUSIVE.
"""
import random
import sys
from pathlib import Path

_R = Path(__file__).resolve().parents[1]
if str(_R / "experiments") not in sys.path:
    sys.path.insert(0, str(_R / "experiments"))

import numpy as np  # noqa: E402
import pytest  # noqa: E402

import _confirm_common as cc  # noqa: E402
import exp16_race_incidence as x16  # noqa: E402
import operating_point as op  # noqa: E402
from event_scheduler import Race, RaceArrival  # noqa: E402
from liquidity_gap_detector import count_liquidity_gaps  # noqa: E402

M, S1, S2 = 100, 200, 201


def _race(*pids):
    arr = tuple(RaceArrival(i, p, "cancel" if p == M else "ioc", i, "x") for i, p in enumerate(pids))
    return Race(0, None, arr)


def test_maker_won_reads_engine_order():
    assert cc.maker_won(_race(M, S1, S2), {M}, {S1, S2}) is True
    assert cc.maker_won(_race(S2, M, S1), {M}, {S1, S2}) is False
    assert cc.maker_won(_race(M), {M}, {S1, S2}) is None          # no sniper fired: no race
    assert cc.maker_won(_race(), {M}, {S1, S2}) is None


# --- grid_gaps is §3.3's detector on the grid --------------------------------------

def _snapshots(states, end, dt, value_at):
    """The scheduler's snapshots: the book after every event at or before s."""
    out, i, bid, ask = [], 0, 0, 0
    for s in range(0, end, dt):
        while i < len(states) and states[i][0] <= s:
            _, bid, ask = states[i]
            i += 1
        out.append({"t": s, "best_bid": bid, "best_ask": ask, "v": value_at(s)})
    return out


@pytest.mark.parametrize("seed", range(40))
def test_grid_gaps_equals_the_detector_on_materialised_snapshots(seed):
    rng = random.Random(seed)
    end, dt = 400, 10
    t, states = 0, []
    while True:
        t += rng.choice([0, 1, 3, 7, 12, 25])
        if t >= end:
            break
        states.append((t, rng.choice([0, 5, 5, 5]), rng.choice([0, 9, 9, 9])))
    jumps = sorted(rng.sample(range(end), 4))

    def value_at(s):
        return 100.0 + sum(2.0 for j in jumps if j <= s) + 0.1 * (s // 50)

    snaps = _snapshots(states, end, dt, value_at)
    first = next((i for i, x in enumerate(snaps) if x["best_bid"] > 0 and x["best_ask"] > 0), None)
    want = 0 if first is None else count_liquidity_gaps(snaps[first:], dt // 1, 0.5,
                                                        max_gap_us=60)
    got = x16.grid_gaps(states, end, dt, value_at, 0.5, max_gap_ns=60)
    assert got == want


def test_grid_gaps_skips_episodes_between_snapshots_and_before_quoting():
    states = [(0, 0, 0), (5, 5, 9), (12, 0, 9), (13, 5, 9), (31, 5, 0), (49, 5, 9)]
    # 12-13 falls between snapshots; 31-49 covers 40 and is counted.
    assert x16.grid_gaps(states, 100, 10, lambda s: 1.0, 1.0) == 1


# --- verdicts ---------------------------------------------------------------------

def test_h2a_hit_miss_inconclusive():
    th = {(k, lf): x16.race_win_probability(lf * op.JITTER_NS, op.JITTER_NS, k)
          for k in (1, 3) for lf in (0.0, 1.5)}
    exact = {c: (round(p * 100_000), 100_000) for c, p in th.items()}
    assert x16.h2a(exact)["verdict"] == "HIT"
    off = {**exact, (1, 0.0): (60_000, 100_000)}          # 0.60 against 0.50
    assert x16.h2a(off)["verdict"] == "MISS"
    small = {**exact, (1, 0.0): (50, 100)}                 # interval wider than the band
    assert x16.h2a(small)["verdict"] == "INCONCLUSIVE"


def _rent(k1_lag0, flat_noise=0.0, seed=0, n=400):
    rng = np.random.default_rng(seed)
    base = rng.exponential(100.0, n)
    out = {}
    for k in (1, 3, 8, 21):
        share = (k / (k + 1)) / (21 / 22)
        if k == 1:
            share = k1_lag0
        out[(k, 0.0)] = base * share * (1 + 0.001 * rng.standard_normal(n))
        out[(k, x16.SLOW_LAG)] = base * (1 + flat_noise * rng.standard_normal(n))
    return out


def test_h2b_hit_miss_inconclusive():
    assert x16.h2b(_rent((1 / 2) / (21 / 22)))["verdict"] == "HIT"
    assert x16.h2b(_rent(0.95))["verdict"] == "MISS"             # still flat at lag 0
    assert x16.h2b(_rent(0.62))["verdict"] == "INCONCLUSIVE"     # outside the band, below 0.65


def test_gap_verdict_bands():
    assert x16.gap_verdict({"lower": 1.0, "upper": 1.9}) == "HIT"
    assert x16.gap_verdict({"lower": 3.0, "upper": 4.0}) == "MISS"
    assert x16.gap_verdict({"lower": 1.5, "upper": 2.5}) == "INCONCLUSIVE"


def test_h2d_verdict():
    hit = {0.01: 0, 0.1: 1, 1: 2, 10: 5, 100: 9, 300: 14}
    assert x16.h2d_verdict(hit)["verdict"] == "HIT"
    assert x16.h2d_verdict({**hit, 0.1: 6})["verdict"] == "MISS"
    assert x16.h2d_verdict({**hit, 300: 5})["verdict"] == "MISS"
    assert x16.h2d_verdict({**hit, 300: 10})["verdict"] == "INCONCLUSIVE"
    non_monotone = {**hit, 10: 11, 100: 9}
    out = x16.h2d_verdict(non_monotone)
    assert not out["non_decreasing"] and out["verdict"] == "INCONCLUSIVE"


# --- shared statistics --------------------------------------------------------------

def test_tost_and_combine():
    assert cc.tost({"lower": 0.95, "upper": 1.05}, {"lower": 0.93, "upper": 1.07}, 0.9, 1.1) == "HIT"
    assert cc.tost({"lower": 1.2, "upper": 1.3}, {"lower": 1.15, "upper": 1.35}, 0.9, 1.1) == "MISS"
    assert cc.tost({"lower": 1.0, "upper": 1.2}, {"lower": 0.98, "upper": 1.22}, 0.9, 1.1) \
        == "INCONCLUSIVE"
    assert cc.combine(["HIT", "HIT"]) == "HIT"
    assert cc.combine(["HIT", "INCONCLUSIVE"]) == "INCONCLUSIVE"
    assert cc.combine(["INCONCLUSIVE", "MISS"]) == "MISS"


def test_ratio_ci_keeps_the_pairing():
    rng = np.random.default_rng(1)
    den = rng.exponential(10.0, 200)
    r = cc.ratio_ci(0.5 * den, den)
    assert r["mean"] == pytest.approx(0.5) and r["upper"] - r["lower"] < 1e-12


def test_paper_policy_maps_the_tick_decay_to_100_ms():
    p = cc.paper_policy()
    assert (1 - p.spread_decay) ** 10 == pytest.approx(0.9)       # ten 10-ms updates = one tick
    assert p.adverse_sensitivity == 0.015 and p.thin_liquidity and p.spread_cap_mult == 10


# --- runs ---------------------------------------------------------------------------

def test_race_run_is_deterministic_and_counts_races():
    a = x16.race_run(3, 0.0, 5, horizon_s=60)
    assert a == x16.race_run(3, 0.0, 5, horizon_s=60)
    assert 0 <= a["maker_wins"] <= a["races"] <= a["n_jumps"]


def test_slow_maker_never_wins():
    rows = [x16.race_run(1, x16.SLOW_LAG, s, horizon_s=120) for s in (1, 2)]
    assert sum(r["races"] for r in rows) > 0 and sum(r["maker_wins"] for r in rows) == 0


def test_end_to_end_summary_on_tiny_runs(tmp_path):
    seeds = [1, 2]
    rows = cc.run_pending(x16.tasks(seeds, horizon_s=1), cc.call, tmp_path / "ck.jsonl", 1, "t")
    s = x16.summarize(rows, seeds)
    for h in ("H2a", "H2b", "H2c", "H2d"):
        assert s[h]["verdict"] in ("HIT", "MISS", "INCONCLUSIVE")
    assert set(s["H2d"]["by_band"]) == {"paper", "tape"}
    again = cc.run_pending(x16.tasks(seeds, horizon_s=1), cc.call, tmp_path / "ck.jsonl", 1, "t")
    assert again == rows                                        # resumed from the checkpoint


def test_confirmatory_cli_takes_the_runners_out_dir(tmp_path):
    # run_confirmatory.sh passes --workers and --out-dir to every driver.
    import _confirm_common as cc
    a = cc.parse_args("exp16_race_incidence", "doc", ["--confirmatory", "--out-dir", str(tmp_path)])
    assert a.out == tmp_path / "exp16_race_incidence.json"
    assert a.checkpoint == tmp_path / "checkpoints" / "exp16_race_incidence.jsonl"
