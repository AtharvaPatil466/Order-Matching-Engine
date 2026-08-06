#!/usr/bin/env python3
"""Research step 6 — the incidence surface crossed with competing liquidity supply.

Why this exists. §4.7 maps a (snipe-to-quote ratio, HFT count) incidence boundary
and places the real tape inside the maker-gains region, but it holds the maker
count at ONE throughout so as to nest §4.6's published cells exactly. §5.2 lists
that as the limit most likely to move the boundary, on §4.6's own evidence: five
competing makers drop rent 50x (4.421 -> 0.089 bp) relative to the one-maker
baseline, so the single-maker regime is an extreme rather than a central case.
This driver adds the maker-count axis and asks the one question §4.7 cannot:
does the inversion survive competitive liquidity supply?

What is swept. exp6's 63-cell surface (7 snipe quantities x 9 HFT counts) at
M in {1, 2, 3, 5} makers. M=1 is exp6's own configuration, so the M=1 column
nests it by construction rather than by re-specification; `verify_nesting()`
checks that claim numerically against the stored n=100 artifact instead of
asserting it.

Two latency schemes are run for M > 1, because the calibration and the brief
disagree about the fastest maker and the disagreement is material:

  calibrated  exp4_competing_makers.latency_set(M) — fastest at 1000 us, the
              p25 top-of-book recovery quartile measured on 7.2 days of
              BTCUSDT-perp (step 3a). This is the set §4.6 published.
  zero_fast   the same set with the fastest maker moved to 0 us, i.e. a maker
              co-located with the snipers and so never pickable-off. This is a
              stronger form of competitive supply than the tape measures; it is
              reported as the optimistic bound on what maker competition can do,
              not as a calibrated arm.

The aggregation caveat of §5.2's sixth limit bears directly on the reading here.
`_split_qty` holds AGGREGATE top-of-book depth at `quote_qty` and divides it
across the M makers, so the swept ratio is the snipe measured against aggregate
depth — which is exactly the quantity the tape reports. At M > 1 the swept ratio
and the tape-measured ratio are therefore the same object, and the bracket that
limit describes closes.

Run: bcs_research/.venv/bin/python bcs_research/experiments/exp7_3d_surface.py [n_seeds]
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

from exp1_primary import CFG                                      # noqa: E402
from exp4_competing_makers import latency_set                     # noqa: E402
from exp6_incidence_surface import (                              # noqa: E402
    K_GRID, QTY_GRID, baseline_rows, classify_incidence, find_boundary, run_cell,
)
from run_calibrated import calibrated_cfg                         # noqa: E402

MAKER_COUNTS = (1, 2, 3, 5)

# Snipe quantities whose realized ratios bracket the band step 3b measured on the
# real tape (p50 0.0072 unconditional). 10/2061 = 0.0049 and 20/2061 = 0.0097.
TAPE_BAND_QTY = (10, 20)

# The row §4.7 reports the flip on: 50/2061 = 0.0243, where the single-maker
# baseline's incidence turns indeterminate at k=7.
FLIP_ROW_QTY = 50

# The main calibrated arm: hft_qty == quote_qty, ratio 1.0, k=3 (§4.1's cell).
MAIN_ARM_QTY = 2061
MAIN_ARM_K = 3

# Abort threshold for the zero-sum conservation identity. Observed residuals run
# ~1e-9 absolute against rents of order 1e5 — a relative error near 1e-14, i.e.
# double-precision accumulation noise. 1e-6 sits ~3 decades above anything the
# engine has produced and ~11 below the smallest economically meaningful figure
# in this experiment, so it separates "floating-point noise" from "the identity
# broke" without needing a relative test that would be ill-defined in the cells
# where rent is near zero.
RESIDUAL_ABORT_ABS = 1e-6


def maker_latencies(n: int, scheme: str) -> list[int]:
    """Latency set for n competing makers under the named scheme.

    M=1 is the paper's single 3-tick maker under BOTH schemes: with one maker
    there is no "fastest competitor" to reposition, and holding it fixed is what
    makes the M=1 column nest exp6.
    """
    lats = latency_set(n)
    if n == 1 or scheme == "calibrated":
        return lats
    if scheme == "zero_fast":
        return [0] + lats[1:]
    raise ValueError(f"unknown latency scheme: {scheme!r}")


def _cell_residual_max(cell: dict) -> float:
    return abs(cell["treatment_ci"]["zero_sum_residual"]["mean"])


def run_arm(n_makers: int, scheme: str, cfg: dict, n_seeds: int,
            boot_seed: int = 0) -> dict:
    """One (maker count, latency scheme) arm: the full 63-cell surface."""
    lats = maker_latencies(n_makers, scheme)
    arm_cfg = {**cfg, "mm_latencies_us": lats}
    label = f"M={n_makers} [{scheme}] lat={lats}"
    print(f"\n{'=' * 78}\n{label}\n{'=' * 78}", flush=True)
    print(f"{'ratio':>8}{'k':>5}{'rent':>12}{'rent bp':>10}{'dMM':>12}"
          f"{'dNT':>12}{'gaps':>9}{'residual':>11}  incidence", flush=True)

    t0 = time.time()
    base = baseline_rows(arm_cfg, n_seeds)
    rows = []
    for qty in QTY_GRID:
        cells = [run_cell(qty, k, arm_cfg, base, n_seeds, boot_seed) for k in K_GRID]
        for c in cells:
            t, d = c["treatment_ci"], c["delta_ci"]
            print(f"{c['snipe_quote_ratio']:>8.4f}{c['n_hfts']:>5}"
                  f"{t['hft_rent']['mean']:>12.1f}{t['hft_rent_bp']['mean']:>10.3f}"
                  f"{d['mm_pnl']['mean']:>12.1f}{d['nt_pnl']['mean']:>12.1f}"
                  f"{t['liquidity_gaps']['mean']:>9.2f}"
                  f"{t['zero_sum_residual']['mean']:>11.1e}"
                  f"  {classify_incidence(c)}", flush=True)
        rows.append({
            "hft_qty": qty,
            "snipe_quote_ratio": qty / cfg["quote_qty"],
            "cells": cells,
            "boundary": find_boundary(cells),
        })
        print(f"    -> ratio {qty / cfg['quote_qty']:.4f}: "
              f"{rows[-1]['boundary'] or 'no flip on this grid'}", flush=True)

    elapsed = time.time() - t0
    all_cells = [c for r in rows for c in r["cells"]]
    resid_max = max(_cell_residual_max(c) for c in all_cells)
    print(f"  arm done in {elapsed:.0f}s, max |residual| = {resid_max:.2e}", flush=True)
    return {
        "n_makers": n_makers,
        "latency_scheme": scheme,
        "latencies_us": lats,
        "rows": rows,
        "max_abs_residual": resid_max,
        "elapsed_sec": elapsed,
        "baseline_liquidity_gaps": float(
            statistics.fmean([r["liquidity_gaps"] for r in base])),
    }


def _row(arm: dict, qty: int) -> dict:
    return next(r for r in arm["rows"] if r["hft_qty"] == qty)


def _cell(arm: dict, qty: int, k: int) -> dict:
    return next(c for c in _row(arm, qty)["cells"] if c["n_hfts"] == k)


def summarize_arm(arm: dict) -> dict:
    """The four readings the incidence question turns on, per arm."""
    band = {}
    for qty in TAPE_BAND_QTY:
        row = _row(arm, qty)
        labels = {c["n_hfts"]: classify_incidence(c) for c in row["cells"]}
        gains_k = [k for k, lab in labels.items() if lab == "maker_gains"]
        band[f"{row['snipe_quote_ratio']:.4f}"] = {
            "hft_qty": qty,
            "labels_by_k": labels,
            "maker_gains_at_all_k": all(v == "maker_gains" for v in labels.values()),
            "maker_gains_k_max": max(gains_k) if gains_k else None,
            "maker_gains_region_exists": bool(gains_k),
            "boundary": row["boundary"],
        }
    main = _cell(arm, MAIN_ARM_QTY, MAIN_ARM_K)
    return {
        "n_makers": arm["n_makers"],
        "latency_scheme": arm["latency_scheme"],
        "latencies_us": arm["latencies_us"],
        "tape_band": band,
        "flip_row_0p0243": {
            "hft_qty": FLIP_ROW_QTY,
            "boundary": _row(arm, FLIP_ROW_QTY)["boundary"],
            "labels_by_k": {c["n_hfts"]: classify_incidence(c)
                            for c in _row(arm, FLIP_ROW_QTY)["cells"]},
        },
        "main_arm_k3_ratio1": {
            "hft_rent_mean": main["treatment_ci"]["hft_rent"]["mean"],
            "hft_rent_ci": [main["treatment_ci"]["hft_rent"]["lower"],
                            main["treatment_ci"]["hft_rent"]["upper"]],
            "hft_rent_bp": main["treatment_ci"]["hft_rent_bp"]["mean"],
        },
        "max_abs_residual": arm["max_abs_residual"],
        "elapsed_sec": arm["elapsed_sec"],
    }


def verify_nesting(cfg: dict, boot_seed: int = 0) -> dict:
    """Check the M=1 column reproduces exp6 exactly, on three diagnostic cells.

    exp6's artifact stores bootstrap CIs, not per-seed rows, so the check runs
    the SAME cells at exp6's n=100 through this driver's M=1 code path and
    compares the stored floats bit-for-bit. Equality is the claim: identical
    config through identical code must give identical output, and any drift means
    the M=1 column is not the nest it is advertised as.
    """
    stored = json.loads(
        (_ROOT / "results" / "experiments" / "exp6_incidence_surface.json").read_text())
    n = stored["n_seeds"]
    arm_cfg = {**cfg, "mm_latencies_us": maker_latencies(1, "calibrated")}
    probes = ((10, 1), (50, 7), (MAIN_ARM_QTY, MAIN_ARM_K))

    print(f"\nValidity check — M=1 nesting against exp6 at n={n}", flush=True)
    base = baseline_rows(arm_cfg, n)
    checks = []
    for qty, k in probes:
        got = run_cell(qty, k, arm_cfg, base, n, boot_seed)
        want = next(c for r in stored["rows"] if r["hft_qty"] == qty
                    for c in r["cells"] if c["n_hfts"] == k)
        pairs = [("hft_rent", "treatment_ci"), ("hft_rent_bp", "treatment_ci"),
                 ("zero_sum_residual", "treatment_ci"), ("mm_pnl", "delta_ci"),
                 ("nt_pnl", "delta_ci")]
        diffs = {m: abs(got[grp][m]["mean"] - want[grp][m]["mean"])
                 for m, grp in pairs}
        exact = all(d == 0.0 for d in diffs.values())
        checks.append({"hft_qty": qty, "n_hfts": k, "exact": exact,
                       "abs_diffs": diffs,
                       "rent_this_run": got["treatment_ci"]["hft_rent"]["mean"],
                       "rent_exp6": want["treatment_ci"]["hft_rent"]["mean"]})
        print(f"  qty={qty:>5} k={k:>3}  exact={exact}  "
              f"rent {got['treatment_ci']['hft_rent']['mean']:.6f} vs exp6 "
              f"{want['treatment_ci']['hft_rent']['mean']:.6f}", flush=True)
    return {"n_seeds": n, "probes": checks,
            "all_exact": all(c["exact"] for c in checks)}


def main(n_seeds: int = 20, out_name: str = "exp7_3d_surface.json",
         boot_seed: int = 0, skip_nesting: bool = False) -> dict:
    cfg = {**CFG, **calibrated_cfg()}
    t_start = time.time()
    print(f"3D incidence surface — calibrated flow, quote_qty={cfg['quote_qty']}, "
          f"{n_seeds} seeds, M in {MAKER_COUNTS}, "
          f"{len(QTY_GRID)}x{len(K_GRID)} cells per arm")

    nesting = None if skip_nesting else verify_nesting(cfg, boot_seed)

    arms = [run_arm(1, "calibrated", cfg, n_seeds, boot_seed)]
    for scheme in ("calibrated", "zero_fast"):
        for m in MAKER_COUNTS:
            if m == 1:
                continue
            arms.append(run_arm(m, scheme, cfg, n_seeds, boot_seed))

    resid_max = max(a["max_abs_residual"] for a in arms)
    conservation_ok = resid_max < RESIDUAL_ABORT_ABS
    elapsed = time.time() - t_start

    report = {
        "config": cfg,
        "n_seeds": n_seeds,
        "grid_hft_qty": list(QTY_GRID),
        "grid_n_hfts": list(K_GRID),
        "grid_n_makers": list(MAKER_COUNTS),
        "quote_qty": cfg["quote_qty"],
        "latency_schemes": ["calibrated", "zero_fast"],
        "nesting_check": nesting,
        "arms": arms,
        "summary": [summarize_arm(a) for a in arms],
        "max_abs_residual": resid_max,
        "residual_abort_threshold": RESIDUAL_ABORT_ABS,
        "conservation_ok": conservation_ok,
        "elapsed_sec": elapsed,
        "n_engine_runs": len(arms) * (n_seeds + len(QTY_GRID) * len(K_GRID) * n_seeds),
    }
    out_dir = _ROOT / "results" / "experiments"
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / out_name).write_text(json.dumps(report, indent=2, sort_keys=True))

    print(f"\n{'=' * 78}\nSUMMARY — max |residual| {resid_max:.2e} "
          f"(threshold {RESIDUAL_ABORT_ABS:.0e}, ok={conservation_ok})")
    print(f"{'M':>3} {'scheme':<11}{'band .0049':>26}{'band .0097':>26}"
          f"{'flip @ .0243':>16}{'rent k3 r1.0':>14}")
    for s in report["summary"]:
        b1 = s["tape_band"]["0.0049"]
        b2 = s["tape_band"]["0.0097"]
        f = s["flip_row_0p0243"]["boundary"]
        flip = f"k={f['k_first_not_gains']}" if f else "no flip"
        print(f"{s['n_makers']:>3} {s['latency_scheme']:<11}"
              f"{('gains<=k' + str(b1['maker_gains_k_max']) if b1['maker_gains_region_exists'] else 'NO GAINS'):>26}"
              f"{('gains<=k' + str(b2['maker_gains_k_max']) if b2['maker_gains_region_exists'] else 'NO GAINS'):>26}"
              f"{flip:>16}{s['main_arm_k3_ratio1']['hft_rent_mean']:>14.1f}")
    print(f"\nelapsed {elapsed / 60:.1f} min, wrote results/experiments/{out_name}")
    return report


if __name__ == "__main__":
    main(n_seeds=int(sys.argv[1]) if len(sys.argv) > 1 else 20)
