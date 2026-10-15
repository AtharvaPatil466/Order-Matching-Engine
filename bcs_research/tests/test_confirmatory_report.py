"""analysis/confirmatory_report.py: Holm, the drivers' 95% verdicts reproduced, smoke refused.

Why the smoke fixture. The report rebuilds every checkpoint key and every
interval itself, so the only honest check is against what the drivers really
write. `smoke` runs each driver's own confirmatory code at toy sizes (seconds,
one worker) into a temporary directory, exactly as run_confirmatory.sh lays
results out. Two stand-ins keep it fast: exp12's simulator is replaced by a
deterministic payoff (its solver, verdicts and JSON are the driver's), and
exp13's rows come from a function with known Sobol indices (its verdicts and
JSON are the driver's). Set BCS_SMOKE_DIR to a directory of real smoke runs
(e.g. 2 seeds x 60 s) to check against those instead.
"""
from __future__ import annotations

import importlib
import json
import math
import os
import shutil
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "analysis"))
import confirmatory_report as cr  # noqa: E402  (puts experiments/ on sys.path)

import _confirm_common as cc  # noqa: E402
import operating_point as op  # noqa: E402

SEEDS = [1, 2]
TOY_S = 2.0


# --- Holm ---------------------------------------------------------------------------------

def test_holm_rejects_in_order_and_stops_at_the_first_failure():
    # Arrange: m = 4, thresholds 0.0125, 0.0167, 0.025, 0.05
    p = {"a": 0.010, "b": 0.040, "c": 0.030, "d": 0.005}

    # Act
    h = cr.holm(p, alpha=0.05)

    # Assert: c fails 0.025, so b is not rejected although 0.040 <= 0.05
    assert sorted(h, key=lambda k: h[k]["rank"]) == ["d", "a", "c", "b"]
    assert {k for k, v in h.items() if v["rejected"]} == {"d", "a"}
    assert h["c"]["threshold"] == pytest.approx(0.025)


def test_holm_counts_absent_primaries_in_the_family_of_eleven():
    # Arrange
    p = {"P1": 0.004, "P2": 0.0049}

    # Act
    h = cr.holm(p, alpha=0.05, m=11)

    # Assert: thresholds 0.05/11 and 0.05/10, as if nine more tests had p = 1
    assert h["P1"]["threshold"] == pytest.approx(0.05 / 11) and h["P1"]["rejected"]
    assert h["P2"]["threshold"] == pytest.approx(0.005) and h["P2"]["rejected"]
    assert not cr.holm({"P1": 0.0046}, m=11)["P1"]["rejected"]
    with pytest.raises(ValueError):
        cr.holm(p, m=1)


def test_grid_p_value_is_the_smallest_holm_threshold_that_hits():
    # Arrange
    grid = cr.holm_thresholds()

    # Act / Assert
    assert grid[0] == pytest.approx(0.05 / 11) and grid[-1] == pytest.approx(0.05)
    assert cr.grid_p_value(lambda a: a >= 0.006, grid) == pytest.approx(0.05 / 8)
    assert cr.grid_p_value(lambda a: False, grid) == 1.0


def test_holm_on_grid_ceilings_decides_exactly_as_on_the_p_values():
    # Arrange
    rng = np.random.default_rng(7)
    grid = cr.holm_thresholds()
    for _ in range(500):
        exact = {f"P{i}": float(x) for i, x in enumerate(rng.uniform(0, 0.012, 11))}
        ceiling = {k: cr.grid_p_value(lambda a, v=v: v <= a, grid) for k, v in exact.items()}

        # Act
        a, b = cr.holm(exact, m=11), cr.holm(ceiling, m=11)

        # Assert
        assert {k: v["rejected"] for k, v in a.items()} == {k: v["rejected"] for k, v in b.items()}


def test_level_helpers_hit_the_drivers_95_percent_exactly():
    # The drivers write 2.5 / 97.5 and z = 1.959963984540054; a = 0.05 must give the same.
    assert cr._pct(np.arange(101.0), 0.05) == (2.5, 97.5)
    assert cr._z(0.05) == 1.959963984540054
    assert 1 - cr.ALPHA == 0.95


def test_holm_levels_resample_20000_times_and_95_percent_keeps_the_drivers_2000():
    # Arrange
    values = np.random.default_rng(3).normal(size=40)
    holm_levels = cr.holm_thresholds()[:-1]                 # the last one is 0.05 itself

    # Act / Assert
    assert cr._n_boot(cr.ALPHA) == op.N_BOOTSTRAP == 2_000
    assert {cr._n_boot(a) for a in holm_levels} == {20_000}
    assert cr._boot(values, cr.ALPHA) == cc.boot(values)
    assert cr._boot(values, holm_levels[0]) == cr.bootstrap_ci(
        list(values), n_bootstrap=20_000, ci=1 - holm_levels[0], seed=0)


@pytest.mark.parametrize("zero_den", [False, True])
def test_ratio_ci_rederived_equals_the_drivers_at_2000_resamples(zero_den):
    # Arrange: cc.ratio_ci fixes its count at 2,000, so the report re-derives it
    rng = np.random.default_rng(11)
    num, den = rng.gamma(2.0, size=30), rng.gamma(3.0, size=30)
    if zero_den:
        den[:27] = 0.0                                   # resamples with a zero denominator

    # Act
    mine, driver = cr._ratio_ci(num, den, cr.ALPHA), cc.ratio_ci(num, den)

    # Assert: the same numbers, bit for bit
    assert mine == driver
    wide = cr._ratio_ci(num, den, cr.ALPHA / cr.FAMILY)
    assert wide["lower"] <= driver["lower"] and driver["upper"] <= wide["upper"]


# --- toy smoke outputs from the drivers' own code ---------------------------------------------

def _confirm_b(out: Path, name: str) -> None:
    """A CONFIRM-B driver's main() at a toy horizon (main has no horizon flag)."""
    x = importlib.import_module(name)
    kw = {"horizon_s": TOY_S}
    if name in ("exp17_jump_vs_walk", "exp18_fee_schedules"):
        kw["bcs_duration_s"] = TOY_S
    rows = cc.run_pending(x.tasks(SEEDS, **kw), cc.call, out / "checkpoints" / f"{name}.jsonl",
                          1, name)
    cc.write_report(out / f"{name}.json", {"experiment": name, "status": cc.SMOKE_STATUS,
                                           "seeds": SEEDS, **x.summarize(rows, SEEDS)})


def _fake_exp12_run(market, profile, latencies_ns, seed, horizon_s):
    """Deterministic stand-in for one exp12 run: payoffs peak at a 4-tick half-spread."""
    nets = [(1.0 - 0.01 * (p.half_spread - 4) ** 2 + 0.01 * ((seed + i) % 3)) if p.quote else 0.0
            for i, p in enumerate(profile)]
    quoting = [p.half_spread for p in profile if p.quote]
    return {"maker_net": nets, "maker_fills": [10 * (i + 1) if p.quote else 0
                                               for i, p in enumerate(profile)],
            "half_spread_ticks": min(quoting) + 0.1 * (seed % 3) if quoting else None}


def _exp12(out: Path) -> None:
    import exp12_endogenous_makers as x12
    with pytest.MonkeyPatch.context() as mp:
        for attr in ("CONFIRM_REPORT", "CONFIRM_CONFIRM"):      # confirmatory() rebinds them
            mp.setattr(x12, attr, getattr(x12, attr))
        mp.setattr(x12, "simulate_op", _fake_exp12_run)
        x12.confirmatory(["--confirmatory", "--horizon-s", str(TOY_S), "--report-seeds", "4",
                          "--confirm-seeds", "2", "--m-grid", "1", "2", "--h8d-ks", "3",
                          "--workers", "1", "--out-dir", str(out)])


def _exp13(out: Path) -> None:
    """exp13's summaries from rows with known indices: rent is all lambda_j and b."""
    import exp13_global_sensitivity as x13
    pts = x13.confirm_points(2048)
    rows = [{"sniper_rent": math.log(p["jump_rate"]) + math.log(p["jump_scale_bp"]) + 10,
             "maker_delta": math.log(p["adverse_sensitivity"]) * math.log(p["snipe_ratio"]),
             "noise_delta": 0.0} for p in pts]
    keyed = {x13.design_key(i, 2048): r for i, r in enumerate(rows)}
    (out / "checkpoints").mkdir(parents=True, exist_ok=True)
    (out / "checkpoints" / "exp13_global_sensitivity.jsonl").write_text(
        "".join(json.dumps({"key": k, "value": v}) + "\n" for k, v in keyed.items()))
    d2 = len(x13.PARAMS) + 2
    for n, name in ((512, "exp13_global_sensitivity"), (2048, "exp13_global_sensitivity_n2048")):
        ordered = [keyed[x13.design_key(i, n)] for i in range(d2 * n)]
        p11 = x13.p11_verdicts(ordered, n, n)
        (out / f"{name}.json").write_text(json.dumps(
            {"status": f"smoke — toy rows, N = {n}", "n_base": n, "d": len(x13.PARAMS),
             "p11": p11, "p11_at_prereg_n512": x13.p11_verdicts(ordered, n, 512)}))


@pytest.fixture(scope="module")
def smoke(tmp_path_factory) -> Path:
    if os.environ.get("BCS_SMOKE_DIR"):
        return Path(os.environ["BCS_SMOKE_DIR"])
    out = tmp_path_factory.mktemp("smoke")
    d = ["--workers", "1", "--out-dir", str(out)]
    importlib.import_module("exp9_bcs_replication").main(
        ["--confirmatory", "--seeds", "2", "--duration-s", "3", "--ratios", "1", "--n-firms", "2", "3"] + d)
    importlib.import_module("exp10_speed_entry").confirmatory(
        ["--confirmatory", "--seeds", "2", "--duration-s", "3"] + d)
    importlib.import_module("exp11_batch_semantics").main(
        ["--confirmatory", "--seeds", "2", "--horizon-s", "5"] + d)
    importlib.import_module("exp15_heldout_who_pays").main(
        ["--confirmatory", "--seeds-confirm", "1", "--horizon-s", str(TOY_S), "--configs", "0"] + d)
    for name in ("exp16_race_incidence", "exp17_jump_vs_walk", "exp18_fee_schedules",
                 "exp19_noise_heterogeneity", "exp20_maker_strategies"):
        _confirm_b(out, name)
    _exp12(out)
    _exp13(out)
    return out


def _as_confirmatory(src: Path, dst: Path) -> Path:
    """A copy of the smoke outputs relabelled confirmatory, to drive the Holm path."""
    shutil.copytree(src, dst)
    for f in dst.glob("*.json"):
        s = json.loads(f.read_text())
        ext = "; EXTENSION" if f.stem.endswith("_n2048") else ""
        f.write_text(json.dumps(s | {"status": op.CONFIRMATORY_STATUS + ext}))
    return dst


# --- each primary reproduces its driver at 95% ---------------------------------------------------

def _driver_intervals(pid: str, s: dict) -> list:
    """The intervals each driver wrote for its primary, in the order the report builds them."""
    if pid == "P1":
        return ([c["race_loss"] for c in s["final"]["cells"]]
                + list(s["final"]["h_hat_slope_on_race_loss"].values()))
    if pid == "P2":
        import exp16_race_incidence as x
        return [s["H2a"]["cells"][f"k={k},lag/eps={lf}"] for k in x.K_RACE for lf in x.LAG_FRACS]
    if pid == "P3":
        return [s["H3a"]]
    if pid == "P4":
        h = s["p4"]["H4a"]
        return [h["entrant_5"], h["entrant_6"]] + ([h["waste_share"]] if h["waste_share"] else [])
    if pid == "P5":
        return [s["H5a"]["tests"][f"{t}_f{f:g}"] for f in (100.0, 300.0)
                for t in ("snipes_ratio", "net_per_snipe")]
    if pid == "P6":
        return [s["H6a"]["by_arm"][f"design_e={e}"]["paper"][k] for e in (0.5, 4.0)
                for k in ("participation_over_predicted", "deadweight_usd_per_s")]
    if pid == "P7":
        import exp20_maker_strategies as x
        return [s["H7a"]["cells"][f"k={k},lag/eps={lf}"] for k in x.K_RACE for lf in x.LAG_FRACS]
    if pid == "P8":
        h = s["p8"]["H8a"]
        top = h["top_M_half_spread_ticks"]
        return [st["diff"] for st in h["steps"] if st["diff"]] + ([top] if top else [])
    if pid == "P9":
        return [c[k] for c in s["p9"]["H9a"]["cells"] for k in ("batch_minus_continuous", "batch_loss")]
    if pid == "P10":
        p = s["bands"]["paper"]["p10"]
        return [p["hit_rate"], p["spearman_negative_share"], p["closely_spaced_hit_rate"]]
    h = s["p11"]["H11a"]
    return [h["S1_sniper_latency"], h["S1_jump_rate_plus_b"]]


def _bounds(items) -> np.ndarray:
    return np.array([[x["lower"], x["upper"]] for x in items], float)


@pytest.mark.parametrize("primary", cr.PRIMARIES, ids=[p.pid for p in cr.PRIMARIES])
def test_each_primary_reproduces_its_drivers_95_percent_verdict_and_intervals(smoke, primary):
    # Arrange
    d = cr.load_driver(smoke, primary.driver)
    assert d is not None, f"no smoke output for {primary.driver}"

    # Act
    at95 = primary.evaluate(d.summary, d.rows, cr.ALPHA)

    # Assert
    assert at95["verdict"] == primary.driver_verdict(d.summary)
    mine = [p for p in at95["parts"] if p["lower"] is not None]
    np.testing.assert_array_equal(_bounds(mine), _bounds(_driver_intervals(primary.pid, d.summary)))


@pytest.mark.parametrize("primary", cr.PRIMARIES, ids=[p.pid for p in cr.PRIMARIES])
def test_holm_level_intervals_are_nested(smoke, primary):
    # Arrange: Holm's strictest and loosest adjusted levels, both at 20,000 resamples
    d = cr.load_driver(smoke, primary.driver)

    # Act
    wide = primary.evaluate(d.summary, d.rows, cr.ALPHA / cr.FAMILY)["parts"]
    narrow = primary.evaluate(d.summary, d.rows, cr.ALPHA / 2)["parts"]

    # Assert: nested intervals are what make a primary's p-value well defined
    for w, n in zip(wide, narrow):
        if n["lower"] is None or not np.isfinite([n["lower"], n["upper"]]).all():
            continue
        assert w["lower"] <= n["lower"] and n["upper"] <= w["upper"], n["label"]


# --- refusal and the confirmatory path --------------------------------------------------------------

def test_a_smoke_status_json_is_refused(smoke, tmp_path):
    # Arrange
    out = tmp_path / "report.md"

    # Act
    report = cr.build_report(smoke)
    code = cr.main(["--results-dir", str(smoke), "--out", str(out)])

    # Assert: nothing counted, nothing written, and the report says so
    assert all(e["holm_verdict"] == "REFUSED: not confirmatory" for e in report["primaries"])
    assert report["confirmatory_drivers"] == [] and report["n_primaries_confirmatory"] == 0
    assert any(x.startswith("NOT CONFIRMATORY: exp16_race_incidence.json") for x in report["deviations"])
    assert all("NOT CONFIRMATORY" in s["verdict_95"] for s in report["secondaries"])
    assert "REFUSED / MISSING" in cr.render_markdown(report)
    assert code == 2 and not out.exists()


def test_a_bare_smoke_status_is_refused_even_without_any_verdicts(tmp_path):
    # Arrange
    (tmp_path / "exp16_race_incidence.json").write_text(json.dumps({"status": cc.SMOKE_STATUS}))

    # Act
    report = cr.build_report(tmp_path)

    # Assert
    p2 = next(e for e in report["primaries"] if e["pid"] == "P2")
    assert p2["state"] == "refused" and p2["holm_verdict"] == "REFUSED: not confirmatory"
    assert "MISSING: exp9_bcs_replication.json (driver not finished or not run)" in report["deviations"]


def test_confirmatory_outputs_go_through_holm_and_are_written(smoke, tmp_path):
    # Arrange
    results = _as_confirmatory(smoke, tmp_path / "results")
    out = tmp_path / "paper" / "confirmatory_results.md"

    # Act
    code = cr.main(["--results-dir", str(results), "--out", str(out)])
    report = json.loads(out.with_suffix(".json").read_text())

    # Assert
    assert code == 0 and report["complete"] and report["n_primaries_confirmatory"] == 11
    for e in report["primaries"]:
        assert e["matches_driver"], e["pid"]
        assert e["holm_verdict"] in ("HIT", "MISS", "INCONCLUSIVE")
        if e["recomputed_95"] == "MISS":
            assert e["holm_verdict"] == "MISS"                           # MISS is never adjusted
        if e["recomputed_95"] != "HIT":
            assert e["p"] == 1.0                                         # no HIT claim open
        if e["holm_verdict"] == "HIT":
            assert e["recomputed_95"] == "HIT" and e["p"] <= e["holm_threshold"]
    ranks = sorted(e["holm_rank"] for e in report["primaries"])
    assert ranks == list(range(1, 12))
    if os.environ.get("BCS_SMOKE_DIR") is None:                          # the toy exp13 rows
        p11 = next(e for e in report["primaries"] if e["pid"] == "P11")
        assert p11["holm_verdict"] == "HIT" and p11["p"] == pytest.approx(0.05 / 11)
        assert [r["role"] for r in report["p11"]] == ["of record", "extension, not of record"]
    assert "# Confirmatory results" in out.read_text()


def test_a_partial_batch_is_labelled_and_never_more_lenient(smoke, tmp_path):
    # Arrange: only exp13 has finished
    results = _as_confirmatory(smoke, tmp_path / "results")
    for f in results.glob("*.json"):
        if not f.stem.startswith("exp13"):
            f.unlink()

    # Act
    report = cr.build_report(results)

    # Assert: a finished primary with a HIT open is ranked first, against alpha/11; the rest
    # are MISSING and can never be rejected
    p11 = next(e for e in report["primaries"] if e["pid"] == "P11")
    if p11["p"] < 1:
        assert p11["holm_rank"] == 1 and p11["holm_threshold"] == pytest.approx(0.05 / 11)
    assert sum(e["holm_verdict"] == "MISSING" for e in report["primaries"]) == 10
    assert not report["complete"] and "PARTIAL BATCH" in cr.render_markdown(report)
