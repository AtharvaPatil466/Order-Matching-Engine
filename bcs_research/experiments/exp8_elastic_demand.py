#!/usr/bin/env python3
"""Research step 7 — the incidence surface under spread-elastic uninformed demand.

Why this exists, and why the question changed. §5.2 long listed inelastic
uninformed demand as the assumption doing the most unexamined work: noise
traders cross at a fixed per-tick probability whatever the maker quotes (§3.3),
so the volume the maker earns its widened spread on cannot fall when it widens.
The original worry followed from the mechanism as the paper then stated it — the
maker gains because widening earns more from noise flow — and ran: make demand
elastic, the noise flow withdraws, the gain evaporates.

That premise was wrong. §4.7's leg decomposition showed the revenue leg is
ALREADY negative at the tape band: widening sheds volume faster than it recovers
edge per fill, and the maker's gain comes from the sniping leg shrinking plus
inventory carry. So elasticity does not attack the channel that produces the
gain; it deepens a leg that is already working against the maker. The question
this driver answers is therefore quantitative rather than existential: how far
does the maker-gains region retreat when the losing leg is made to lose harder?

Design. Noise-trader crossing probability becomes

    lambda_eff = lambda_per_tick * (base_half_spread / half_spread) ** alpha

read off the BOOK's prevailing half-spread (`NoiseTrader._effective_lambda`).
alpha = 0 recovers the pre-registered inelastic path exactly — bit-identical RNG
stream, so the alpha = 0 column nests exp6 rather than approximating it, which
`verify_nesting` checks numerically and gates the run on. alpha = 1 is unit
elasticity: doubling the spread halves arrivals. At the base spread lambda_eff
== lambda_per_tick for every alpha, so §3.7's fitted arrival intensity is
preserved by construction and elasticity changes only the response to widening,
never the calibration point.

Grid. exp6's full 7 x 9 surface (63 cells) at M = 1, matching §4.7 exactly, at
n = 20 seeds. The k grid keeps 6 and 7, which is where the alpha = 0 boundary
sits on the 0.0243 row and therefore where any movement has to be visible.

Run: bcs_research/.venv/bin/python bcs_research/experiments/exp8_elastic_demand.py [n_seeds]
"""
from __future__ import annotations

import json
import statistics
import sys
import time
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_ROOT = _HERE.parent
for _d in ("build", "agents", "simulation", "metrics", "experiments"):
    _p = str(_ROOT / _d)
    if _p not in sys.path:
        sys.path.insert(0, _p)

from bootstrap import bootstrap_ci                          # noqa: E402
from exp1_primary import CFG, _run                          # noqa: E402
from exp6_incidence_surface import K_GRID, QTY_GRID         # noqa: E402
from exp7_3d_surface import RESIDUAL_ABORT_ABS              # noqa: E402
from run_calibrated import calibrated_cfg                   # noqa: E402

ALPHAS = (0.0, 0.5, 1.0)

TAPE_BAND_QTY = (10, 20)      # ratios 0.0049 and 0.0097
FLIP_ROW_QTY = 50             # ratio 0.0243, where alpha=0 flips at k=6/7
MAIN_ARM_QTY = 2061           # ratio 1.0
MAIN_ARM_K = 3

VERDICT_ABBREV = {"maker_gains": "GAIN", "maker_pays": "PAYS",
                  "indeterminate": "  ? "}


def _verdict(ci: dict) -> str:
    """Sign of the maker's PnL delta at 95% bootstrap confidence.

    Same convention as exp6's `classify_incidence`: a CI straddling zero is its
    own verdict, never folded into whichever side the point estimate falls on.
    """
    if ci["lower"] > 0.0:
        return "maker_gains"
    if ci["upper"] < 0.0:
        return "maker_pays"
    return "indeterminate"


def run_cell(hft_qty: int, n_hfts: int, alpha: float, cfg: dict,
             base_rows: list[dict], n_seeds: int, boot_seed: int = 0) -> dict:
    cell_cfg = {**cfg, "hft_qty": hft_qty, "n_hft": n_hfts}
    treat = [_run(s, cell_cfg, with_hft=True, per_maker=True)
             for s in range(1, n_seeds + 1)]

    d_mm = [t["mm_pnl"] - b["mm_pnl"] for t, b in zip(treat, base_rows)]
    d_nt = [t["nt_pnl"] - b["nt_pnl"] for t, b in zip(treat, base_rows)]
    resid = [t["zero_sum_residual"] for t in treat]
    mm_ci = bootstrap_ci(d_mm, seed=boot_seed)
    rent_ci = bootstrap_ci([t["hft_rent"] for t in treat], seed=boot_seed)
    nt_ci = bootstrap_ci(d_nt, seed=boot_seed)

    return {
        "hft_qty": hft_qty,
        "snipe_quote_ratio": hft_qty / cfg["quote_qty"],
        "n_hfts": n_hfts,
        "alpha": alpha,
        "n_seeds": n_seeds,
        "maker_delta": mm_ci,
        "maker_verdict": _verdict(mm_ci),
        "hft_rent": {k: rent_ci[k] for k in ("mean", "lower", "upper")},
        "zero_sum_residual": {
            "mean": float(statistics.fmean(resid)),
            "max_abs": max(abs(r) for r in resid),
        },
        "noise_trader_delta": {k: nt_ci[k] for k in ("mean", "lower", "upper")},
        # Diagnostic: elasticity is supposed to suppress noise participation, so
        # a flat trade count across alpha would mean the knob never engaged.
        "n_trades_mean": float(statistics.fmean([t["n_trades"] for t in treat])),
        "spread_ticks_mean": float(statistics.fmean([t["spread_ticks"] for t in treat])),
    }


def find_boundary(cells: list[dict]) -> dict | None:
    """First k at which a maker-gains row stops gaining; bracketing pair only."""
    ordered = sorted(cells, key=lambda c: c["n_hfts"])
    labels = [c["maker_verdict"] for c in ordered]
    for i in range(len(ordered) - 1):
        if labels[i] == "maker_gains" and labels[i + 1] != "maker_gains":
            return {"k_last_maker_gains": ordered[i]["n_hfts"],
                    "k_first_not_gains": ordered[i + 1]["n_hfts"],
                    "flips_to": labels[i + 1]}
    return None


def run_arm(alpha: float, cfg: dict, n_seeds: int, boot_seed: int = 0) -> dict:
    arm_cfg = {**cfg, "spread_elasticity": alpha}
    print(f"\n{'=' * 86}\nalpha = {alpha}\n{'=' * 86}", flush=True)
    print(f"{'ratio':>8}{'k':>4}{'dMM':>11}{'dNT':>11}{'rent':>12}"
          f"{'trades':>9}{'spread':>9}{'resid':>10}  verdict", flush=True)

    t0 = time.time()
    base = [_run(s, {**arm_cfg, "n_hft": 0}, with_hft=False, per_maker=True)
            for s in range(1, n_seeds + 1)]

    rows = []
    for qty in QTY_GRID:
        cells = [run_cell(qty, k, alpha, arm_cfg, base, n_seeds, boot_seed)
                 for k in K_GRID]
        for c in cells:
            print(f"{c['snipe_quote_ratio']:>8.4f}{c['n_hfts']:>4}"
                  f"{c['maker_delta']['mean']:>11.0f}"
                  f"{c['noise_trader_delta']['mean']:>11.0f}"
                  f"{c['hft_rent']['mean']:>12.1f}"
                  f"{c['n_trades_mean']:>9.0f}{c['spread_ticks_mean']:>9.1f}"
                  f"{c['zero_sum_residual']['max_abs']:>10.1e}"
                  f"  {VERDICT_ABBREV[c['maker_verdict']]}", flush=True)
        rows.append({"hft_qty": qty, "snipe_quote_ratio": qty / cfg["quote_qty"],
                     "cells": cells, "boundary": find_boundary(cells)})
        print(f"    -> ratio {qty / cfg['quote_qty']:.4f}: "
              f"{rows[-1]['boundary'] or 'no flip on this grid'}", flush=True)

    elapsed = time.time() - t0
    all_cells = [c for r in rows for c in r["cells"]]
    resid_max = max(c["zero_sum_residual"]["max_abs"] for c in all_cells)
    print(f"  arm done in {elapsed:.0f}s, max |residual| = {resid_max:.2e}",
          flush=True)
    return {
        "alpha": alpha,
        "rows": rows,
        "max_abs_residual": resid_max,
        "elapsed_sec": elapsed,
        "baseline_n_trades": float(statistics.fmean([r["n_trades"] for r in base])),
        "baseline_spread_ticks": float(
            statistics.fmean([r["spread_ticks"] for r in base])),
    }


def _row(arm: dict, qty: int) -> dict:
    return next(r for r in arm["rows"] if r["hft_qty"] == qty)


def summarize_arm(arm: dict) -> dict:
    band = {}
    for qty in TAPE_BAND_QTY:
        row = _row(arm, qty)
        labels = {c["n_hfts"]: c["maker_verdict"] for c in row["cells"]}
        gains = [k for k, v in labels.items() if v == "maker_gains"]
        band[f"{row['snipe_quote_ratio']:.4f}"] = {
            "labels_by_k": labels,
            "maker_gains_region_exists": bool(gains),
            "maker_gains_at_all_k": len(gains) == len(labels),
            "maker_gains_k_max": max(gains) if gains else None,
            "boundary": row["boundary"],
        }
    main = next(c for c in _row(arm, MAIN_ARM_QTY)["cells"]
                if c["n_hfts"] == MAIN_ARM_K)
    return {
        "alpha": arm["alpha"],
        "tape_band": band,
        "flip_row_0p0243": {
            "boundary": _row(arm, FLIP_ROW_QTY)["boundary"],
            "labels_by_k": {c["n_hfts"]: c["maker_verdict"]
                            for c in _row(arm, FLIP_ROW_QTY)["cells"]},
        },
        "main_arm_k3_ratio1_rent": main["hft_rent"],
        "baseline_n_trades": arm["baseline_n_trades"],
        "max_abs_residual": arm["max_abs_residual"],
        "elapsed_sec": arm["elapsed_sec"],
    }


def verify_nesting(cfg: dict, boot_seed: int = 0) -> dict:
    """alpha=0 must reproduce exp6 exactly, else the elasticity hook moved something.

    Mandatory gate: the whole comparison across alpha is meaningless if the
    inelastic column is not the published surface.
    """
    stored = json.loads(
        (_ROOT / "results" / "experiments" / "exp6_incidence_surface.json").read_text())
    n = stored["n_seeds"]
    arm_cfg = {**cfg, "spread_elasticity": 0.0}
    print(f"\nValidity check — alpha=0 nesting against exp6 at n={n}", flush=True)

    base = [_run(s, {**arm_cfg, "n_hft": 0}, with_hft=False, per_maker=True)
            for s in range(1, n + 1)]
    checks = []
    for qty, k in ((10, 1), (50, 7), (MAIN_ARM_QTY, MAIN_ARM_K)):
        got = run_cell(qty, k, 0.0, arm_cfg, base, n, boot_seed)
        want = next(c for r in stored["rows"] if r["hft_qty"] == qty
                    for c in r["cells"] if c["n_hfts"] == k)
        d_rent = abs(got["hft_rent"]["mean"]
                     - want["treatment_ci"]["hft_rent"]["mean"])
        d_res = abs(got["zero_sum_residual"]["mean"]
                    - want["treatment_ci"]["zero_sum_residual"]["mean"])
        exact = (d_rent == 0.0 and d_res == 0.0)
        checks.append({"hft_qty": qty, "n_hfts": k, "exact": exact,
                       "abs_diff_hft_rent": d_rent,
                       "abs_diff_zero_sum_residual": d_res,
                       "rent_this_run": got["hft_rent"]["mean"],
                       "rent_exp6": want["treatment_ci"]["hft_rent"]["mean"]})
        print(f"  qty={qty:>5} k={k:>3}  exact={exact}  "
              f"rent {got['hft_rent']['mean']:.6f} vs exp6 "
              f"{want['treatment_ci']['hft_rent']['mean']:.6f}", flush=True)
    return {"n_seeds": n, "probes": checks,
            "all_exact": all(c["exact"] for c in checks)}


def main(n_seeds: int = 20, out_name: str = "exp8_elastic_demand.json",
         boot_seed: int = 0) -> dict:
    cfg = {**CFG, **calibrated_cfg()}
    t_start = time.time()
    print(f"Elastic demand surface — calibrated flow, quote_qty={cfg['quote_qty']}, "
          f"base_half_spread={cfg['base_half_spread']}, {n_seeds} seeds, "
          f"alpha in {ALPHAS}, {len(QTY_GRID)}x{len(K_GRID)} cells per arm")

    nesting = verify_nesting(cfg, boot_seed)
    if not nesting["all_exact"]:
        raise SystemExit("VALIDITY CHECK FAILED: alpha=0 does not nest exp6; "
                         "refusing to run the elastic arms.")

    arms = [run_arm(a, cfg, n_seeds, boot_seed) for a in ALPHAS]
    resid_max = max(a["max_abs_residual"] for a in arms)
    if resid_max >= RESIDUAL_ABORT_ABS:
        raise SystemExit(f"CONSERVATION FAILED: max |residual| {resid_max:.2e} "
                         f">= {RESIDUAL_ABORT_ABS:.0e}")

    elapsed = time.time() - t_start
    report = {
        "config": cfg,
        "n_seeds": n_seeds,
        "grid_alpha": list(ALPHAS),
        "grid_hft_qty": list(QTY_GRID),
        "grid_n_hfts": list(K_GRID),
        "quote_qty": cfg["quote_qty"],
        "nesting_check": nesting,
        "arms": arms,
        "summary": [summarize_arm(a) for a in arms],
        "max_abs_residual": resid_max,
        "residual_abort_threshold": RESIDUAL_ABORT_ABS,
        "conservation_ok": resid_max < RESIDUAL_ABORT_ABS,
        "elapsed_sec": elapsed,
    }
    out_dir = _ROOT / "results" / "experiments"
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / out_name).write_text(json.dumps(report, indent=2, sort_keys=True))

    print(f"\n{'=' * 86}\nSUMMARY — max |residual| {resid_max:.2e} "
          f"(ok={report['conservation_ok']})")
    print(f"{'alpha':>6}{'band .0049':>22}{'band .0097':>22}"
          f"{'flip @ .0243':>15}{'rent k3 r1.0':>14}{'base trades':>13}")
    for s in report["summary"]:
        b1, b2 = s["tape_band"]["0.0049"], s["tape_band"]["0.0097"]
        f = s["flip_row_0p0243"]["boundary"]

        def fmt(b):
            return (f"gains<=k{b['maker_gains_k_max']}"
                    if b["maker_gains_region_exists"] else "NO GAINS")

        print(f"{s['alpha']:>6}{fmt(b1):>22}{fmt(b2):>22}"
              f"{('k=' + str(f['k_first_not_gains']) if f else 'no flip'):>15}"
              f"{s['main_arm_k3_ratio1_rent']['mean']:>14.1f}"
              f"{s['baseline_n_trades']:>13.0f}")
    print(f"\nelapsed {elapsed / 60:.1f} min, wrote results/experiments/{out_name}")
    return report


if __name__ == "__main__":
    main(n_seeds=int(sys.argv[1]) if len(sys.argv) > 1 else 20)
