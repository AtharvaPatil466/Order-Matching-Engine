#!/usr/bin/env python3
"""Step 6b — is "the maker pays" at M > 1 redistribution, or does every maker lose?

Why this exists. exp7 reports `mm_pnl` SUMMED across competing makers, and at the
tape-measured band that sum turns negative for every M > 1 tested. But §4.6's
per-maker decomposition at ratio 1.0 shows the makers are not alike: the fastest
earned +2,014 while the slowest lost -2,393. A negative aggregate is therefore
consistent with two very different worlds:

  redistribution  the fast maker still gains at the tape band and the slow ones
                  fund it — in which case "liquidity providers bear the tax" is
                  wrong as a statement about the class, and the right statement
                  is about SLOW liquidity providers.
  uniform         every maker loses — in which case competition genuinely
                  restores the BCS prediction for liquidity provision as a class.

exp7's artifact cannot answer this: `run_cell` bootstraps `CELL_METRICS`, which
carries `mm_pnl` but not the per-maker vector. This driver re-runs the affected
cells storing each maker's seed-matched PnL delta separately.

Scope. Only the tape-measured band (snipe qty 10 and 20, ratios 0.0049 and
0.0097) at M in {2, 3}, across exp6's full k grid, at n=100 — the cells the
question is actually about. The full 63-cell surface at n=100 would be ~105 min
for an answer the band already gives.

Both latency schemes are run, because the fastest maker's latency IS the variable
under test: under `zero_fast` the leader is co-located with the snipers and
cannot be picked off at all, under `calibrated` it merely leads by 4 ticks.

Each maker also carries `snipe_quote_ratio_per_maker` = hft_qty / its own quoted
size. `_split_qty` divides aggregate depth across the makers, so this is M times
the swept aggregate ratio — the ratio-rescaling channel of §4.8(d), recorded in
the artifact rather than left to be inferred from the maker count.

Run: bcs_research/.venv/bin/python bcs_research/experiments/exp7b_per_maker_pnl.py [n_seeds]
"""
from __future__ import annotations

import json
import sys
import time
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_ROOT = _HERE.parent
for _d in ("build", "agents", "simulation", "metrics", "experiments"):
    _p = str(_ROOT / _d)
    if _p not in sys.path:
        sys.path.insert(0, _p)

from bootstrap import bootstrap_ci                     # noqa: E402
from exp1_primary import CFG, _run, _split_qty         # noqa: E402
from exp6_incidence_surface import K_GRID              # noqa: E402
from exp7_3d_surface import RESIDUAL_ABORT_ABS, maker_latencies  # noqa: E402
from run_calibrated import calibrated_cfg              # noqa: E402

MAKER_COUNTS = (2, 3)
SCHEMES = ("zero_fast", "calibrated")

# The band step 3b measured on the real tape: 10/2061 = 0.0049, 20/2061 = 0.0097,
# bracketing the unconditional median of 0.0072.
TAPE_BAND_QTY = (10, 20)

# Console shorthand. NOT a truncation: "maker_gains" and "maker_pays" share a
# four-character prefix, so slicing the verdict would render the two opposite
# findings identically.
VERDICT_ABBREV = {"maker_gains": "GAIN", "maker_pays": "PAYS",
                  "indeterminate": "  ? "}


def _verdict(ci: dict) -> str:
    """Sign of a maker's PnL delta at 95% bootstrap confidence.

    Same convention as exp6's `classify_incidence`, applied one maker at a time:
    a CI straddling zero is its own verdict, not folded into whichever side the
    point estimate happens to fall on.
    """
    if ci["lower"] > 0.0:
        return "maker_gains"
    if ci["upper"] < 0.0:
        return "maker_pays"
    return "indeterminate"


def run_cell(hft_qty: int, n_hfts: int, arm_cfg: dict, base_rows: list[dict],
             n_seeds: int, boot_seed: int = 0) -> dict:
    """One band cell, with each maker's delta bootstrapped separately."""
    cell_cfg = {**arm_cfg, "hft_qty": hft_qty, "n_hft": n_hfts}
    treat = [_run(s, cell_cfg, with_hft=True, per_maker=True)
             for s in range(1, n_seeds + 1)]

    lats = [int(x) for x in treat[0]["maker_latencies"]]
    qtys = _split_qty(arm_cfg["quote_qty"], len(lats))
    per_maker = []
    for i, lat in enumerate(lats):
        deltas = [t["maker_pnls"][i] - b["maker_pnls"][i]
                  for t, b in zip(treat, base_rows)]
        ci = bootstrap_ci(deltas, seed=boot_seed)
        per_maker.append({
            "maker_index": i,
            "latency_us": lat,
            "quoted_qty": qtys[i],
            "snipe_quote_ratio_per_maker": hft_qty / qtys[i],
            "delta_ci": ci,
            "verdict": _verdict(ci),
        })

    agg = bootstrap_ci([t["mm_pnl"] - b["mm_pnl"]
                        for t, b in zip(treat, base_rows)], seed=boot_seed)
    resid = bootstrap_ci([t["zero_sum_residual"] for t in treat], seed=boot_seed)
    return {
        "hft_qty": hft_qty,
        "snipe_quote_ratio": hft_qty / arm_cfg["quote_qty"],
        "n_hfts": n_hfts,
        "n_seeds": n_seeds,
        "aggregate_delta": agg,
        "aggregate_verdict": _verdict(agg),
        "per_maker": per_maker,
        "zero_sum_residual": resid,
        # The question in one field: does any maker gain while the sum does not?
        "is_redistribution": (_verdict(agg) != "maker_gains"
                              and any(m["verdict"] == "maker_gains"
                                      for m in per_maker)),
    }


def run_arm(n_makers: int, scheme: str, cfg: dict, n_seeds: int,
            boot_seed: int = 0) -> dict:
    lats = maker_latencies(n_makers, scheme)
    qtys = _split_qty(cfg["quote_qty"], n_makers)
    arm_cfg = {**cfg, "mm_latencies_us": lats}
    print(f"\n{'=' * 96}\nM={n_makers} [{scheme}] lat={lats} qty={qtys}\n{'=' * 96}",
          flush=True)

    t0 = time.time()
    base = [_run(s, {**arm_cfg, "n_hft": 0}, with_hft=False, per_maker=True)
            for s in range(1, n_seeds + 1)]

    hdr = f"{'ratio':>8}{'k':>4}{'aggregate':>20}"
    for lat in lats:
        hdr += f"{'maker@' + str(lat) + 'us':>22}"
    print(hdr, flush=True)

    cells = []
    for qty in TAPE_BAND_QTY:
        for k in K_GRID:
            c = run_cell(qty, k, arm_cfg, base, n_seeds, boot_seed)
            cells.append(c)
            a = c["aggregate_delta"]
            line = (f"{c['snipe_quote_ratio']:>8.4f}{k:>4}"
                    f"{a['mean']:>9.0f} {VERDICT_ABBREV[_verdict(a)]:<10}")
            for m in c["per_maker"]:
                line += (f"{m['delta_ci']['mean']:>11.0f} "
                         f"{VERDICT_ABBREV[m['verdict']]:<10}")
            print(line + ("   <-- REDISTRIBUTION" if c["is_redistribution"] else ""),
                  flush=True)

    elapsed = time.time() - t0
    resid_max = max(abs(c["zero_sum_residual"]["mean"]) for c in cells)
    n_gain = sum(1 for c in cells for m in c["per_maker"]
                 if m["verdict"] == "maker_gains")
    print(f"  arm done in {elapsed:.0f}s, max |residual| = {resid_max:.2e}, "
          f"per-maker gains {n_gain}/{len(cells) * n_makers}", flush=True)
    return {
        "n_makers": n_makers,
        "latency_scheme": scheme,
        "latencies_us": lats,
        "quoted_qty": qtys,
        "cells": cells,
        "max_abs_residual": resid_max,
        "elapsed_sec": elapsed,
        "n_per_maker_gain_cells": n_gain,
        "n_per_maker_cells": len(cells) * n_makers,
        "any_redistribution": any(c["is_redistribution"] for c in cells),
    }


def main(n_seeds: int = 100, out_name: str = "exp7b_per_maker_pnl.json",
         boot_seed: int = 0) -> dict:
    cfg = {**CFG, **calibrated_cfg()}
    t_start = time.time()
    print(f"Per-maker PnL decomposition — calibrated flow, "
          f"quote_qty={cfg['quote_qty']}, {n_seeds} seeds, M in {MAKER_COUNTS}, "
          f"schemes {SCHEMES}, band ratios "
          f"{[round(q / cfg['quote_qty'], 4) for q in TAPE_BAND_QTY]}")

    arms = [run_arm(m, s, cfg, n_seeds, boot_seed)
            for s in SCHEMES for m in MAKER_COUNTS]

    resid_max = max(a["max_abs_residual"] for a in arms)
    elapsed = time.time() - t_start
    report = {
        "config": cfg,
        "n_seeds": n_seeds,
        "grid_hft_qty": list(TAPE_BAND_QTY),
        "grid_n_hfts": list(K_GRID),
        "grid_n_makers": list(MAKER_COUNTS),
        "latency_schemes": list(SCHEMES),
        "quote_qty": cfg["quote_qty"],
        "arms": arms,
        "max_abs_residual": resid_max,
        "residual_abort_threshold": RESIDUAL_ABORT_ABS,
        "conservation_ok": resid_max < RESIDUAL_ABORT_ABS,
        "any_redistribution": any(a["any_redistribution"] for a in arms),
        "elapsed_sec": elapsed,
    }
    out_dir = _ROOT / "results" / "experiments"
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / out_name).write_text(json.dumps(report, indent=2, sort_keys=True))

    print(f"\n{'=' * 96}\nSUMMARY — max |residual| {resid_max:.2e} "
          f"(threshold {RESIDUAL_ABORT_ABS:.0e}, ok={report['conservation_ok']})")
    print(f"redistribution anywhere in the tape band: {report['any_redistribution']}")
    for a in arms:
        print(f"  M={a['n_makers']} [{a['latency_scheme']:<10}] "
              f"per-maker gain cells {a['n_per_maker_gain_cells']:>3}/"
              f"{a['n_per_maker_cells']:<3} redistribution={a['any_redistribution']}")
    print(f"\nelapsed {elapsed / 60:.1f} min, wrote results/experiments/{out_name}")
    return report


if __name__ == "__main__":
    main(n_seeds=int(sys.argv[1]) if len(sys.argv) > 1 else 100)
