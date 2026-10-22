"""analysis/confirmatory_figures.py: each figure renders from a driver-shaped summary; gaps are skipped.

The fixtures are small summaries with the keys each driver writes. Where a
driver's own pure verdict function is cheap (exp16 h2a / h2d_verdict, exp18 h5a,
exp20 h7a, exp15 classify / classify_p10b) it builds the block, so the schema is
the driver's; the rest mirror the drivers' summary code. Values sit near the
closed forms, so a render of them is a realistic layout check. Set BCS_SMOKE_DIR
to a directory of real smoke outputs (tests/test_confirmatory_report.py's
layout) to render from those as well.
"""
from __future__ import annotations

import json
import math
import os
import sys
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "analysis"))
import confirmatory_figures as cf  # noqa: E402  (puts experiments/ on sys.path)

import exp15_heldout_who_pays as x15  # noqa: E402
import exp16_race_incidence as x16  # noqa: E402
import exp18_fee_schedules as x18  # noqa: E402
import exp20_maker_strategies as x20  # noqa: E402
import operating_point as op  # noqa: E402
from bcs_theory import race_win_probability  # noqa: E402

SMOKE = "smoke — hand-built test fixture; not a result"
EPS = op.JITTER_NS


def _ci(m: float, h: float) -> dict:
    return {"mean": m, "lower": m - h, "upper": m + h, "n": 2}


def _exp9() -> dict:
    cells = []
    for ratio, h_star in ((0.25, 203.9), (1.0, 567.1), (4.0, 1202.2)):
        for n in (2, 3, 5, 10):
            th = (n - 1) / n
            cells.append({"ratio": ratio, "n_firms": n, "h_star": h_star,
                          "race_loss": {**_ci(th + 0.002 * (n % 3 - 1), 0.004), "theory": th,
                                        "band": [th - 0.02, th + 0.02]},
                          "h_hat": {**_ci(h_star * (1 - 0.003 * (n % 2)), h_star * 0.012),
                                    "band": [h_star * 0.97, h_star * 1.03]}})
    return {"status": SMOKE, "final": {"cells": cells},
            "p1": {"primary_race_loss": "HIT", "h_hat_all_cells": "HIT", "h_hat_slope": "HIT"}}


def _exp10() -> dict:
    cells = [{"rent_multiple": m, "theory_n_star": int(m), "entry": {"n_star": int(m)},
              "waste_share": _ci(int(m) / m + 0.001, 0.0015), "theory_waste_share": int(m) / m}
             for m in (3.5, 5.5, 10.5)]
    return {"status": SMOKE, "p4": {"H4a": {"verdict": "HIT", "n_star": 5, "prediction_n_star": 5,
                                            "waste_band": [0.859, 0.959]},
                                    "H4b": {"verdict": "HIT", "cells": cells}}}


def _exp17() -> dict:
    hs = (482, 510, 539, 567, 596, 624, 652)
    rw = {str(h): 228.1 * math.exp(-(h - 482) / 1000) for h in hs}
    return {"status": SMOKE,
            "H3a": {**_ci(1.306, 0.009), "h": 567, "theory": 1.3014, "band": [1.171, 1.432],
                    "verdict": "HIT"},
            "H3b": {"laplace_per_snipe": {str(h): {**_ci(1002.0, 5.0), "theory": 1000.0,
                                                   "band": [950.0, 1050.0], "verdict": "HIT"} for h in hs},
                    "rw_per_snipe": {h: _ci(v - 0.3, 0.5) for h, v in rw.items()},
                    "rw_per_snipe_theory": rw, "verdict": "HIT"}}


def _exp16() -> dict:
    races = {(k, lf): (round(4000 * race_win_probability(lf * EPS, EPS, k)) + 3, 4000)
             for k in x16.K_RACE for lf in x16.LAG_FRACS}
    counts = {"paper": dict(zip(x16.H2D_LATENCIES_MS, (0, 0, 1, 4, 9, 13))),
              "tape": dict(zip(x16.H2D_LATENCIES_MS, (0, 1, 1, 2, 5, 8)))}
    by_band = {b: {**x16.h2d_verdict(c), "by_latency_ms": {str(lat): {"count": v, "n_cells": 18}
                                                           for lat, v in c.items()}}
               for b, c in counts.items()}
    return {"status": SMOKE, "H2a": x16.h2a(races),
            "H2d": {"verdict": by_band["paper"]["verdict"], "by_band": by_band}}


def _exp12() -> dict:
    steps = [{"from_M": a, "to_M": b, "diff": _ci(d, 0.35), "verdict": "HIT"}
             for a, b, d in ((1, 2, -2.1), (2, 3, -0.6), (3, 5, -0.4))]
    return {"status": SMOKE, "space": {"half_spreads_ticks": [0.5, 1, 2, 4, 8, 16, 32, 64]},
            "p8": {"H8a": {"verdict": "MISS", "steps": steps, "top_M": 5,
                           "top_M_half_spread_ticks": _ci(0.62, 0.05), "breakeven_ticks": 0.43,
                           "band": [0.344, 0.516], "top_M_verdict": "MISS"}}}


def _exp15() -> dict:
    rng = np.random.default_rng(15)
    scored = []
    for i in range(30):
        lat = sorted(rng.uniform(0.5, 10.0, 2 + i % 3).tolist())
        deltas = [-x - rng.normal(0, 1.5) for x in lat]
        scored.append({"latencies": lat, "deltas": deltas, "closely_spaced": i % 4 == 0,
                       "p10b_payer": None if i % 5 == 0 else int(np.argmax(lat))})
    band = {"p10": x15.classify(scored), "p10b": x15.classify_p10b(scored)}
    return {"status": SMOKE, "bands": {"paper": band, "tape": band}}


def _exp18() -> dict:
    rng = np.random.default_rng(18)
    fills = {f: rng.uniform(900, 1100, 6) for f in (0.0, *x18.H5A_FT)}
    snipes = {f: math.exp(-f / x18.B) * rng.uniform(0.79, 0.83, 6) for f in fills}
    net = {f: x18.B * fills[f] * rng.uniform(0.98, 1.02, 6) for f in fills}
    return {"status": SMOKE, "H5a": x18.h5a(snipes, net, fills)}


def _exp19() -> dict:
    arm = {e: {"vbar_ticks": v,
               **{b: {"participation_over_predicted": _ci(1.0 + s, 0.04),
                      "deadweight_usd_per_s": _ci(0.8 + s, 0.2), "verdict": "HIT"}
                  for b, s in (("paper", 0.01), ("tape", -0.02))}}
           for e, v in (("design_e=0.5", 215.6), ("design_e=4.0", 26.9))}
    return {"status": SMOKE, "H6a": {"verdict": "HIT", "tape_band_verdict": "HIT", "by_arm": arm}}


def _exp20() -> dict:
    rng = np.random.default_rng(20)
    cells = {}
    for k in x20.K_RACE:
        for lf in x20.LAG_FRACS:
            n_a, n_b = rng.integers(40, 60, 6), rng.integers(40, 60, 6)
            loss_b = rng.uniform(9, 11, 6) * n_b
            loss_a = (1 - race_win_probability(lf * EPS, EPS, k)) * rng.uniform(9, 11, 6) * n_a
            cells[(k, lf)] = (loss_a, n_a, loss_b, n_b)
    return {"status": SMOKE, "H7a": x20.h7a(cells)}


def _exp11() -> dict:
    arms = {"fast/stale/continuous": {"maker_sniping_loss": _ci(1.25, 0.04)}}
    cells = []
    for tau, loss in ((1_000_000, 0.043), (10_000_000, 0.003), (100_000_000, -0.0005)):
        arms[f"fast/stale/tau={tau}ns/time/engine"] = {"maker_sniping_loss": _ci(loss, 0.009 * loss + 0.001)}
        cells.append({"tau_ns": tau, "verdict": "HIT"})
    return {"status": SMOKE, "arms": arms,
            "p9": {"H9a": {"verdict": "HIT", "cells": cells}, "H9c": {"verdict": "HIT"}}}


def _exp13() -> dict:
    names = list(cf.FACTOR_LABELS)
    s1 = dict(zip(names, (0.31, 0.38, 0.01, 0.03, 0.02, 0.01, 0.05, 0.0, 0.0, 0.08, 0.04, 0.02, 0.01, 0.0)))
    st = dict(zip(names, (0.05, 0.06, 0.02, 0.10, 0.03, 0.02, 0.12, 0.33, 0.08, 0.41, 0.05, 0.22, 0.04, 0.01)))

    def table(vals):
        return {"indices": {n: {"S1": v, "S1_lo": v - 0.03, "S1_hi": v + 0.03,
                                "ST": v, "ST_lo": v - 0.03, "ST_hi": v + 0.03} for n, v in vals.items()}}

    return {"status": SMOKE, "n_base": 512, "sobol": {"sniper_rent": table(s1), "maker_delta": table(st)},
            "p11": {"H11a": {"verdict": "HIT", "S1_sniper_latency": _ci(0.01, 0.02),
                             "S1_jump_rate_plus_b": _ci(0.69, 0.06)},
                    "H11b": {"verdict": "INCONCLUSIVE",
                             "top3_total_order": ["snipe_ratio", "adverse_sensitivity", "n_snipers"]},
                    "H11c": {"verdict": "HIT"}}}


FAKES = {"exp9_bcs_replication": _exp9, "exp10_speed_entry": _exp10, "exp17_jump_vs_walk": _exp17,
         "exp16_race_incidence": _exp16, "exp12_endogenous_makers": _exp12,
         "exp15_heldout_who_pays": _exp15, "exp18_fee_schedules": _exp18,
         "exp19_noise_heterogeneity": _exp19, "exp20_maker_strategies": _exp20,
         "exp11_batch_semantics": _exp11, "exp13_global_sensitivity": _exp13}


def write_fakes(results_dir: Path, status: str = SMOKE, only=None) -> None:
    results_dir.mkdir(parents=True, exist_ok=True)
    for name, build in FAKES.items():
        if only is None or name in only:
            (results_dir / f"{name}.json").write_text(json.dumps(build() | {"status": status}, default=float))


def _texts(fig) -> list:
    return [t.get_text() for ax in fig.axes for t in ax.texts]


@pytest.mark.parametrize("name", list(cf.FIGURES))
def test_each_figure_renders_from_a_driver_shaped_summary(name):
    # Arrange
    fn, drivers = cf.FIGURES[name]

    # Act
    fig = fn(*(FAKES[d]() for d in drivers))

    # Assert: panels drawn, and every primary badge says which verdict it shows
    assert fig.axes
    primaries = [t for t in _texts(fig) if " primary " in t]
    assert primaries and all("95%, before Holm" in t for t in primaries)
    plt.close(fig)


def test_a_figure_takes_a_path_and_shows_the_holm_verdict_when_given(tmp_path):
    # Arrange
    path = tmp_path / "exp11_batch_semantics.json"
    path.write_text(json.dumps(_exp11()))

    # Act
    fig = cf.batch_p9(path, holm={"P9": ("INCONCLUSIVE", "Holm")})

    # Assert
    assert "P9 primary (Holm): INCONCLUSIVE" in _texts(fig)
    plt.close(fig)


def test_cli_skips_missing_and_non_confirmatory_inputs(tmp_path, capsys):
    # Arrange: only exp11, and it is a smoke file
    write_fakes(tmp_path / "res", only={"exp11_batch_semantics"})

    # Act
    code = cf.main(["--results-dir", str(tmp_path / "res"), "--out-dir", str(tmp_path / "fig")])

    # Assert: nothing drawn, every figure skipped with its reason, no crash
    out = capsys.readouterr().out
    assert code == 0 and not list((tmp_path / "fig").glob("*"))
    assert "SKIP batch_p9: exp11_batch_semantics.json not confirmatory" in out
    assert "SKIP headline_bcs_recovery: exp9_bcs_replication.json missing" in out
    assert out.count("SKIP ") == len(cf.FIGURES)


def test_cli_renders_confirmatory_inputs_and_smoke_only_when_allowed(tmp_path):
    # Arrange
    write_fakes(tmp_path / "conf", status=op.CONFIRMATORY_STATUS, only={"exp11_batch_semantics"})
    write_fakes(tmp_path / "smoke")

    # Act
    conf = cf.render_all(tmp_path / "conf", tmp_path / "fig_conf")
    smoke = cf.render_all(tmp_path / "smoke", tmp_path / "fig_smoke", allow_smoke=True)

    # Assert
    assert conf["batch_p9"] == "rendered" and conf["race_p2"] == "skipped"
    assert {p.name for p in (tmp_path / "fig_conf").iterdir()} == {"batch_p9.png", "batch_p9.svg"}
    assert set(smoke.values()) == {"smoke"}
    assert cf.STAMP in (tmp_path / "fig_smoke" / "race_p2.svg").read_text()


def test_holm_report_from_another_results_dir_is_ignored(tmp_path):
    # Arrange
    report = tmp_path / "confirmatory_results.json"
    report.write_text(json.dumps({"results_dir": str(tmp_path / "elsewhere"), "complete": True,
                                  "primaries": [{"pid": "P9", "holm_verdict": "HIT"}]}))

    # Act / Assert
    assert cf.load_holm(report, tmp_path / "elsewhere") == {"P9": ("HIT", "Holm")}
    assert cf.load_holm(report, tmp_path / "res") is None
    assert cf.load_holm(tmp_path / "absent.json", tmp_path) is None


@pytest.mark.skipif(not os.environ.get("BCS_SMOKE_DIR"), reason="set BCS_SMOKE_DIR to real smoke outputs")
def test_real_smoke_outputs_render(tmp_path):
    result = cf.render_all(Path(os.environ["BCS_SMOKE_DIR"]), tmp_path, allow_smoke=True)
    assert "failed" not in result.values()
