"""Experiment 13's market pieces on short real runs, and its design bookkeeping.

The estimator itself is tested on Ishigami in test_sensitivity.py. Here: the
per-class fee re-pricing equals accounting.build_ledger on the same trades,
the welfare identities close, a run is reproducible, the gap counter fires
only on an IOC that empties a side, and the design rows are laid out as
analyse() assumes.
"""
import sys
from pathlib import Path

_R = Path(__file__).resolve().parents[1]
for _d in ("build", "simulation", "agents", "metrics", "analysis", "calibration", "experiments"):
    if str(_R / _d) not in sys.path:
        sys.path.insert(0, str(_R / _d))

import numpy as np  # noqa: E402
import pytest  # noqa: E402

import bcs_engine as be  # noqa: E402
import exp13_global_sensitivity as x  # noqa: E402
from accounting import FeeSchedule, build_ledger  # noqa: E402
from event_scheduler import EventScheduler  # noqa: E402

SHORT_S = 5.0
# Near-zero fee: snipers pick off diffusion steps, so a 5-s run has sniper fills.
POINT = dict(x.BASE_POINT, snipe_ratio=0.3, fee_over_b=0.02, n_makers=2, n_snipers=3,
             lag_over_eps=0.5, seed=7)


def _trades(p, with_snipers):
    agents, path, jitter = x.build(p, SHORT_S, with_snipers)
    h = be.EngineHarness()
    h.add_symbol(1)
    h.start(disable_circuit_breaker=True)
    try:
        return EventScheduler(h, 1, seed=p["seed"], snapshot_every_ns=x.NS_PER_S,
                              race_jitter_ns=jitter).run(agents, path).trades
    finally:
        h.stop()


@pytest.fixture(scope="module")
def run():
    return x.simulate(POINT, SHORT_S, True)


def test_fee_repricing_equals_build_ledger_on_the_same_trades(run):
    f = POINT["fee_over_b"] * POINT["jump_scale_bp"]
    ledger = build_ledger(_trades(POINT, True), FeeSchedule(0.0, f))
    want = dict.fromkeys(x.CLASSES, 0.0)
    for pid, acc in ledger.accounts.items():
        want[x._class_of(pid)] += acc.fees
    got = x._fees(run, x.fee_table(POINT))
    assert run["sniper_fills"] > 0
    for c in x.CLASSES:
        assert got[c] == pytest.approx(want[c], rel=1e-9, abs=1e-12)
    # A two-sided schedule too: makers pay on passive fills, takers on aggressive.
    table = {"maker": (1.5, 4.0), "sniper": (1.5, 4.0), "noise": (1.5, 4.0)}
    ledger = build_ledger(_trades(POINT, True), FeeSchedule(1.5, 4.0))
    assert sum(x._fees(run, table).values()) == pytest.approx(ledger.exchange_revenue, rel=1e-9)


def test_welfare_identities_close_and_runs_are_reproducible():
    row = x.outcomes(POINT, SHORT_S)
    assert abs(row["identity_residual"]) < 1e-9 and row["edge_residual"] < 1e-9
    assert row["sniper_rent"] > 0 and row["maker_delta"] < 0
    assert x.outcomes(POINT, SHORT_S) == row
    assert x.run_points([POINT], SHORT_S, workers=2) == [row]


class _FakeHarness:
    def __init__(self):
        self.bid, self.ask = 99, 101

    def best_bid(self, sym):
        return self.bid

    def best_ask(self, sym):
        return self.ask

    def submit_order(self, sym, oid, pid, side, price, qty, otype, tif):
        if otype == be.OrderType.IOC and side == be.Side.Buy:
            self.ask = 0           # the IOC took the whole ask
        return True


def test_gap_counter_counts_only_iocs_that_empty_a_two_sided_book():
    g = x._GapHarness(_FakeHarness())
    g.submit_order(1, 1, 1, be.Side.Buy, 101, 5, be.OrderType.Limit, None)
    assert g.gaps == 0
    g.submit_order(1, 2, 1, be.Side.Buy, 101, 5, be.OrderType.IOC, None)
    assert g.gaps == 1
    g.submit_order(1, 3, 1, be.Side.Buy, 101, 5, be.OrderType.IOC, None)   # already one-sided
    assert g.gaps == 1 and g.best_bid(1) == 99                           # delegation


def test_design_rows_are_a_then_b_then_each_ab_i():
    n, d = 4, len(x.PARAMS)
    points, A, B = x.design_points(n)
    assert len(points) == n * (d + 2)
    for i, name in enumerate(x.NAMES):
        block = points[(2 + i) * n:(3 + i) * n]
        for j in range(n):
            changed = {k for k in x.NAMES if block[j][k] != points[j][k]}
            assert changed <= {name}                         # AB_i = A except column i
            assert block[j][name] == points[n + j][name]     # ... which comes from B


def test_parameter_ranges_follow_p11():
    p = {q.name: q for q in x.PARAMS}
    assert (p["n_snipers"].lo, p["n_snipers"].hi) == (1, 21)
    assert (p["n_makers"].lo, p["n_makers"].hi) == (1, 5)
    assert (p["snipe_ratio"].lo, p["snipe_ratio"].hi) == (0.003, 1.0)
    assert (p["fee_over_b"].lo, p["fee_over_b"].hi) == (0.0, 0.3)
    assert (p["lag_over_eps"].lo, p["lag_over_eps"].hi) == (0.0, 2.0)
    assert p["sniper_lat_ns"].hi / p["sniper_lat_ns"].lo == pytest.approx(100.0)
    for name in ("jump_rate", "jump_scale_bp", "noise_rate", "adverse_sensitivity", "decay_per_s"):
        assert p[name].hi / p[name].lo == pytest.approx(16.0)


def test_recommend_n_scales_the_widest_interval_as_one_over_root_n():
    idx = {"seed": {"ST": 0.0, "ST_lo": 0.0, "ST_hi": 0.0},
           "a": {"ST": 0.5, "ST_lo": 0.4, "ST_hi": 0.6},      # half-width 0.1
           "b": {"ST": 0.2, "ST_lo": 0.15, "ST_hi": 0.25}}
    rec = x.recommend_n({"y": {"indices": idx}}, n=64, target_half_width=0.05)
    assert rec["y"] == {"widest_half_width": pytest.approx(0.1), "n_needed": 256}


def test_analyse_reads_rows_in_design_order():
    n, d = 64, len(x.PARAMS)
    points, _, _ = x.design_points(n)
    # Output = n_snipers only: all of the variance on that factor.
    rows = [{k: float(pt["n_snipers"]) for k in x.OUTCOMES} for pt in points]
    t = x.analyse(rows, n, n_boot=50)["sniper_rent"]["indices"]
    assert t["n_snipers"]["ST"] == pytest.approx(1.0, abs=0.15)
    assert max(abs(r["ST"]) for k, r in t.items() if k != "n_snipers") < 1e-12
    assert np.isfinite([r["S1"] for r in t.values()]).all()


# --- confirmatory mode ------------------------------------------------------------

def test_the_first_512_rows_of_the_2048_design_are_the_512_design():
    import exp13_global_sensitivity as x13
    big, small = x13.confirm_points(2048), x13.confirm_points(512)
    d2 = len(x13.PARAMS) + 2
    for b in range(d2):
        assert big[b * 2048: b * 2048 + 512] == small[b * 512: (b + 1) * 512]
    assert small[0]["jitter_ns"] == 80_000 and small[0]["depth_lots"] == 6_180


def test_p11_verdicts_on_a_function_with_known_indices():
    import math
    import exp13_global_sensitivity as x13
    n = 512
    pts = x13.confirm_points(n)
    mid = {k: sum(math.log(p[k]) for p in pts) / len(pts) for k in ("adverse_sensitivity", "snipe_ratio")}

    def c(p, k):
        return math.log(p[k]) - mid[k]
    rows = [{"sniper_rent": math.log(p["jump_rate"]) + math.log(p["jump_scale_bp"]) + 10,
             "maker_delta": c(p, "adverse_sensitivity") * c(p, "snipe_ratio"),
             "noise_delta": 0.0} for p in pts]
    v = x13.p11_verdicts(rows, n, n)
    assert v["H11a"]["verdict"] == "HIT"          # no latency, all lambda_j and b
    assert v["H11b"]["verdict"] == "HIT"          # a pure interaction of the two named factors
    assert v["H11c"]["verdict"] == "HIT"          # zero toll
    assert x13.p11_verdicts(rows, n, 256)["n_base"] == 256
