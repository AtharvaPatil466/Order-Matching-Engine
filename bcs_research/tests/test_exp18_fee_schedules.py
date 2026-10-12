"""Exp 18 (P5): the fee closed form, the schedule splits, the verdicts, fee-aware runs.

P5 prints H5b's all-in half-spreads, so the solver must give all six; splits
of one F must really share it; a sniper must stop firing below its fee and
book it; and each verdict rule is checked on HIT, MISS and INCONCLUSIVE inputs.
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
import exp18_fee_schedules as x18  # noqa: E402


@pytest.mark.parametrize("F,n", sorted(x18.H5B_TABLE))
def test_all_in_half_spread_matches_p5s_table(F, n):
    assert x18.net_half_spread(F, n) + F == pytest.approx(x18.H5B_TABLE[(F, n)], abs=0.1)


@pytest.mark.parametrize("F", [100.0, 300.0])
def test_splits_share_the_total_fee(F):
    s = x18.schedules_at(F)
    assert set(s) == {"taker_heavy", "maker_rebate"}
    assert all(f_m + f_t == pytest.approx(F) for f_m, f_t in s.values())
    assert s["maker_rebate"][0] < 0 < s["taker_heavy"][0]


def test_rent_factor_uses_bp_over_bp():
    assert x18.rent_factor("zero") == 1.0
    assert x18.rent_factor("taker_heavy") == pytest.approx(math.exp(-5 / x18.op.PATH["jump_scale_bp"]))


def _arr(x, n=200, noise=0.0, seed=0):
    return x * (1 + noise * np.random.default_rng(seed).standard_normal(n))


def test_h5a_hit_miss_inconclusive():
    def run(scale, noise=0.0):
        snipes = {0.0: _arr(0.3)}
        net, fills = {}, {}
        for f in x18.H5A_FT:
            snipes[f] = _arr(0.3 * math.exp(-f / 1000) * scale, noise=noise, seed=int(f))
            fills[f] = _arr(100.0)
            net[f] = _arr(100_000.0)
        return x18.h5a(snipes, net, fills)["verdict"]
    assert run(1.0) == "HIT"
    assert run(1.3) == "MISS"
    assert run(1.0, noise=1.0) == "INCONCLUSIVE"


def test_h5b_hit_miss_inconclusive():
    def table(shift=0.0, noise=0.0):
        out = {}
        for F in x18.H5B_F:
            for n in x18.H5B_N:
                th = x18.net_half_spread(F, n) + F
                for name in x18.schedules_at(F):
                    bump = shift if name == "maker_rebate" else 0.0
                    out[(F, n, name)] = list(_arr(th * (1 + bump), noise=noise, seed=n))
        return out
    assert x18.h5b(table())["verdict"] == "HIT"
    assert x18.h5b(table(shift=0.10))["verdict"] == "MISS"      # splits 10% apart
    assert x18.h5b(table(noise=0.2))["verdict"] == "INCONCLUSIVE"


def test_h5c_verdict():
    assert x18.h5c_verdict({"zero": 4, "taker_heavy": 5, "maker_rebate": 4}, True) == "HIT"
    assert x18.h5c_verdict({"zero": 4, "taker_heavy": 3, "maker_rebate": 9}, True) == "MISS"
    assert x18.h5c_verdict({"zero": 4, "taker_heavy": 5, "maker_rebate": 4}, False) \
        == "INCONCLUSIVE"


def test_taker_fee_thins_snipes_and_is_booked():
    free = x18.bcs_task(567, 2, 0.0, 0.0, 4, duration_s=60)
    fee = x18.bcs_task(567, 2, 300.0, 0.0, 4, duration_s=60)
    assert fee["n_jumps"] == free["n_jumps"]                    # same path (CRN)
    assert 0 < fee["n_sniper_trades"] < free["n_sniper_trades"]
    assert fee["sniper_net"] == pytest.approx(fee["sniper_edge"] - 300 * fee["sniper_fills"])
    assert fee == x18.bcs_task(567, 2, 300.0, 0.0, 4, duration_s=60)


def test_end_to_end_summary_on_tiny_runs(tmp_path):
    seeds = [1, 2]
    rows = cc.run_pending(x18.tasks(seeds, horizon_s=1, bcs_duration_s=5), cc.call,
                          tmp_path / "ck.jsonl", 1, "t")
    s = x18.summarize(rows, seeds)
    assert all(s[h]["verdict"] in ("HIT", "MISS", "INCONCLUSIVE") for h in ("H5a", "H5b", "H5c"))
    assert set(s["H5c"]["by_band"]) == {"paper", "tape"}
