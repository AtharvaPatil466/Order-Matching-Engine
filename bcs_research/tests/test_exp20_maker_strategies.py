"""Exp 20 (P7): the new agents and metrics, every verdict rule, and determinism.

H7a needs a twin that really never wins a race; H7b a skew log equal to the
quotes' actual displacement; H7c a sweep that takes every stale level and a
level tag on every order; H7d an A-S maker at the paper maker's base spread.
Each verdict rule is checked on HIT, MISS and INCONCLUSIVE inputs.
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
import exp20_maker_strategies as x20  # noqa: E402
import operating_point as op  # noqa: E402
import strategic_maker as sm  # noqa: E402
import bcs_engine as be  # noqa: E402
from contracts import SignalUpdate  # noqa: E402


def test_as_maker_has_the_paper_makers_base_spread_and_a_b_sized_inventory_term():
    m = sm.StrategicMaker(1, x20.as_policy())
    assert m._base_half == pytest.approx(cc.paper_policy().half_spread)
    assert m._as_shift * op.QUOTE_LOTS == pytest.approx(x20.B_TICKS)
    assert x20.as_policy().adverse_sensitivity == 0


class _View:
    def __init__(self, bid, ask):
        self.bid, self.ask = bid, ask

    def best_bid(self):
        return self.bid

    def best_ask(self):
        return self.ask


def test_sweep_sniper_limits_at_the_last_profitable_price():
    s = x20.SweepSniper(200, 999, threshold=10.0)
    (o,) = s.on_signal(0, SignalUpdate(0, 120.5, True), _View(90, 100))
    assert o.side == be.Side.Buy and o.price == 110 and o.quantity == 999
    (o,) = s.on_signal(0, SignalUpdate(0, 70.0, True), _View(90, 100))
    assert o.side == be.Side.Sell and o.price == 81
    assert s.on_signal(0, SignalUpdate(0, 105.0, True), _View(90, 100)) == []


def test_ladder_maker_tags_levels_per_side():
    m = x20.LadderMaker(1, op.default_policy(qty=5, n_levels=3, level_spacing=7))
    m.on_signal(0, SignalUpdate(0, 1000.0), _View(0, 0))
    by_level = {}
    for oid, q in m.live_orders.items():
        by_level.setdefault(m.level_of[oid], []).append(q.price)
    asks = sorted(p for lvl in by_level for p in by_level[lvl] if p > 1000)
    assert [m.level_of[o] for o, q in m.live_orders.items() if q.price in asks] == [0, 1, 2]
    assert asks[1] - asks[0] == 7


def test_shift_log_records_the_skew_of_the_live_quotes():
    m = x20.ShiftLogMaker(1, op.default_policy(qty=5, inventory_skew=0.5))
    m.inventory = 10
    m.on_signal(0, SignalUpdate(0, 1000.0), _View(0, 0))       # requote at q = 10
    m.on_signal(1, SignalUpdate(1, 1100.0, True), _View(0, 0))  # jump: logs the live shift
    assert m.jump_shifts == [5.0]


def test_non_cancelling_twin_ignores_jumps_only():
    m = x20.NonCancellingMaker(1, op.default_policy(qty=5))
    assert m.on_signal(0, SignalUpdate(0, 1000.0, True), _View(0, 0)) == []
    assert len(m.on_signal(1, SignalUpdate(1, 1000.0), _View(0, 0))) == 2


def test_per_jump_ratio_ci():
    la, na = np.full(50, 30.0), np.full(50, 10.0)
    r = x20.per_jump_ratio_ci(la, na, 2 * la, 2 * na)
    assert r["mean"] == pytest.approx(1.0) and r["upper"] - r["lower"] < 1e-12


def _ci(lo, hi):
    return {"mean": (lo + hi) / 2, "lower": lo, "upper": hi}


def test_h7a_hit_miss_inconclusive():
    n = 100
    rng = np.random.default_rng(0)

    def cells(scale_k1=1.0, noise=0.0):
        out = {}
        for k in (1, 3):
            for lf in (0.0, 1.5):
                th = 1 - x20.race_win_probability(lf * op.JITTER_NS, op.JITTER_NS, k)
                if k == 1 and lf == 0:
                    th *= scale_k1
                lb = rng.exponential(100, n)
                la = lb * th * (1 + noise * rng.standard_normal(n))
                out[(k, lf)] = (la, np.full(n, 5.0), lb, np.full(n, 5.0))
        return out
    assert x20.h7a(cells())["verdict"] == "HIT"
    assert x20.h7a(cells(scale_k1=1.4))["verdict"] == "MISS"
    assert x20.h7a(cells(noise=1.0))["verdict"] == "INCONCLUSIVE"


def test_h7b_cell():
    assert x20.h7b_cell(_ci(1.03, 1.06), 1.045)["verdict"] == "HIT"
    assert x20.h7b_cell(_ci(1.20, 1.30), 1.045)["verdict"] == "MISS"
    assert x20.h7b_cell(_ci(0.90, 0.98), 1.005)["verdict"] == "MISS"     # skew cut the loss
    assert x20.h7b_cell(_ci(0.98, 1.12), 1.045)["verdict"] == "INCONCLUSIVE"


def test_h7d_and_h7e_verdicts():
    assert x20.h7d_verdict(1, _ci(-5, -1)) == "HIT"
    assert x20.h7d_verdict(6, _ci(-5, -1)) == "MISS"
    assert x20.h7d_verdict(1, _ci(0, 3)) == "MISS"
    assert x20.h7d_verdict(4, _ci(-5, -1)) == "INCONCLUSIVE"
    assert x20.h7d_verdict(1, _ci(-2, 3)) == "INCONCLUSIVE"
    assert x20.h7e_cell(_ci(-3, -1), _ci(1, 2)) and not x20.h7e_cell(_ci(-3, 1), _ci(1, 2))
    assert x20.h7e_verdict(6) == "HIT" and x20.h7e_verdict(5) == "MISS"


def test_non_cancelling_maker_loses_every_snipeable_race():
    rows = [x20.h7a_run("non_cancelling", 1, 0.0, s, horizon_s=200) for s in (1, 2)]
    fast = [x20.h7a_run("cancelling", 1, 0.0, s, horizon_s=200) for s in (1, 2)]
    assert sum(r["snipeable"] for r in rows) > 0
    assert sum(r["loss_usd"] for r in rows) > sum(r["loss_usd"] for r in fast) > 0
    assert rows[0] == x20.h7a_run("non_cancelling", 1, 0.0, 1, horizon_s=200)


def test_ladder_run_snipes_deeper_levels_less():
    rows = [x20.h7c_run(s, horizon_s=300) for s in (1, 2, 3)]
    loss = np.sum([r["loss_by_level_usd"] for r in rows], axis=0)
    assert loss[0] > loss[1] > 0


def test_end_to_end_summary_on_tiny_runs(tmp_path):
    seeds = [1, 2]
    rows = cc.run_pending(x20.tasks(seeds, horizon_s=2), cc.call, tmp_path / "ck.jsonl", 1, "t")
    s = x20.summarize(rows, seeds)
    assert all(s[h]["verdict"] in ("HIT", "MISS", "INCONCLUSIVE")
               for h in ("H7a", "H7b", "H7c", "H7d", "H7e"))
    assert math.isfinite(s["H7d"]["by_band"]["paper"]["as_minus_paper_pnl_with_hft_usd"]["mean"])
