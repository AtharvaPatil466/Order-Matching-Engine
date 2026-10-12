"""Exp 19 (P6): the exponential-gain investor, the pooled participation statistic,
the verdicts, and determinism.

H6a's whole test is participation against e^(-dh/vbar), so the investor must
cross with probability e^(-all-in/vbar) and keep its draws aligned across arms;
the pooled statistic must read 1 when the law holds exactly.
"""
import math
import sys
from pathlib import Path

_R = Path(__file__).resolve().parents[1]
if str(_R / "experiments") not in sys.path:
    sys.path.insert(0, str(_R / "experiments"))

import numpy as np  # noqa: E402
import pytest  # noqa: E402

import _confirm_common as cc  # noqa: E402
import exp19_noise_heterogeneity as x19  # noqa: E402


class _View:
    def __init__(self, bid, ask):
        self.bid, self.ask = bid, ask

    def best_bid(self):
        return self.bid

    def best_ask(self):
        return self.ask


def _investor(seed=3, vbar=50.0, fee_bps=0.0):
    return x19.ExpGainInvestor(300, 10.0, vbar, fee_bps, 1000.0, 3, 1.0, seed)


def test_crossing_probability_is_exponential_in_the_all_in_spread():
    inv = _investor(vbar=50.0, fee_bps=1.0)          # fee 0.1 tick at price ~1000
    view = _View(990, 1010)                          # 10 ticks either side of the signal
    for t in range(20_000):
        inv.on_wakeup(t, "arrive", view)
    want = math.exp(-(10 + 1e-4 * 1010) / 50)
    assert inv.crossings / inv.arrivals == pytest.approx(want, abs=0.01)


def test_draws_stay_aligned_whatever_the_book():
    a, b = _investor(), _investor()
    for t in range(500):
        a.on_wakeup(t, "arrive", _View(999, 1001))
        b.on_wakeup(t, "arrive", _View(900, 1100))
    assert a._rng.random() == b._rng.random()        # same number of draws consumed
    assert a.crossings > b.crossings


def test_participation_ci_reads_one_when_the_law_holds():
    vbar, rng = 40.0, np.random.default_rng(0)
    h0 = rng.uniform(1, 2, 50)
    h = np.array([h0 + d for d in (2.0, 5.0)])
    part0 = np.exp(-h0.mean() / vbar) * np.ones(50)
    part = np.array([np.exp(-(h0.mean() + d) / vbar) * np.ones(50) for d in (2.0, 5.0)])
    r = x19.participation_ci(part, h, part0, h0, vbar)
    assert r["mean"] == pytest.approx(1.0, abs=1e-3)


def _ci(lo, hi):
    return {"mean": (lo + hi) / 2, "lower": lo, "upper": hi}


def test_h6a_verdict():
    assert x19.h6a_verdict(_ci(0.95, 1.05), _ci(0.1, 0.3)) == "HIT"
    assert x19.h6a_verdict(_ci(0.95, 1.05), _ci(-0.1, 0.3)) == "MISS"     # deadweight includes 0
    assert x19.h6a_verdict(_ci(1.2, 1.3), _ci(0.1, 0.3)) == "MISS"
    assert x19.h6a_verdict(_ci(1.05, 1.15), _ci(0.1, 0.3)) == "INCONCLUSIVE"


def test_h6b_verdict():
    assert x19.h6b_verdict(_ci(-1.0, -0.2), 1.0) == "HIT"
    assert x19.h6b_verdict(_ci(0.01, 0.2), 1.0) == "MISS"
    assert x19.h6b_verdict(_ci(-0.1, 0.2), 1.0) == "INCONCLUSIVE"


def test_h6c_verdict_branches():
    assert x19.h6c_verdict(_ci(0.4, 0.6), 16)["verdict"] == "HIT"
    assert x19.h6c_verdict(_ci(0.4, 0.6), 14)["verdict"] == "MISS"
    assert x19.h6c_verdict(_ci(3.5, 4.5), 9)["verdict"] == "HIT"
    assert x19.h6c_verdict(_ci(3.5, 4.5), 10)["verdict"] == "MISS"
    assert x19.h6c_verdict(_ci(0.8, 1.2), 18)["verdict"] == "INCONCLUSIVE"   # straddles e = 1


def test_design_puts_one_arm_in_each_branch():
    assert [x19.H_BASE_ALLIN / x19.vbar_for(e) for e in x19.DESIGN_E] == pytest.approx([0.5, 4])
    assert x19.INF["info_lag_s"] * 1e9 > 2 * cc.PAPER_LATENCY_NS


def test_run_is_deterministic_and_the_informed_trades():
    a = x19.run(0.5, 3, 0.0049, 2, horizon_s=130)
    assert a == x19.run(0.5, 3, 0.0049, 2, horizon_s=130)
    assert a["informed_lots"] > 0 and 0 < a["crossings"] <= a["arrivals"]


def test_end_to_end_summary_on_tiny_runs(tmp_path):
    seeds = [1, 2]
    rows = cc.run_pending(x19.tasks(seeds, horizon_s=3), cc.call, tmp_path / "ck.jsonl", 1, "t")
    s = x19.summarize(rows, seeds)
    assert all(s[h]["verdict"] in ("HIT", "MISS", "INCONCLUSIVE") for h in ("H6a", "H6b", "H6c"))
