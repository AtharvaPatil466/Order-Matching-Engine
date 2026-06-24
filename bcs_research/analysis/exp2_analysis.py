#!/usr/bin/env python3
"""Exp 2 analysis — phase-transition verdict + significance threshold.

Consumes results/experiments/exp2_latency_sweep.json (produced by the sweep) and
emits the headline statistical findings, keeping the run-the-sims step and the
analyse-the-sims step separate and independently reproducible.

Two distinct "threshold" statements are reported, because they answer different
questions and need not coincide:

  1. FUNCTIONAL FORM. Fit HFT rent vs latency to a linear model and a continuous
     two-segment (knee) model on the per-seed points, compare by BIC. A knee is
     evidence of a phase transition; a linear preference is the simpler
     monotone-scaling story.

  2. SIGNIFICANCE THRESHOLD. The smallest swept latency at which the bootstrap
     95% CI of HFT rent excludes zero. This is a statistical-power statement
     (when does rent become detectable?), not a claim about the mean's shape.

Run: bcs_research/.venv/bin/python bcs_research/analysis/exp2_analysis.py
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_ROOT = _HERE.parent
sys.path.insert(0, str(_HERE))

import numpy as np                              # noqa: E402
from phase_transition import detect_phase_transition  # noqa: E402

US_PER_TICK = 1000  # dt_us; latency resolves at this granularity (see exp2 sweep)


def _per_seed_points(report: dict, metric: str = "hft_rent") -> tuple:
    xs, ys = [], []
    for cell in report["cells"]:
        for row in cell["treatment_rows"]:
            xs.append(cell["mm_latency_us"])
            ys.append(row[metric])
    return np.array(xs, dtype=float), np.array(ys, dtype=float)


def _significance_threshold_us(report: dict, metric: str = "hft_rent"):
    """Smallest latency whose treatment CI lower bound for `metric` exceeds 0."""
    for cell in sorted(report["cells"], key=lambda c: c["mm_latency_us"]):
        if cell["treatment_ci"][metric]["lower"] > 0.0:
            return cell["mm_latency_us"]
    return None


def analyze(report: dict) -> dict:
    xs, ys = _per_seed_points(report)
    pt = detect_phase_transition(xs, ys)
    sig_us = _significance_threshold_us(report)
    lin = pt["linear"]
    seg = pt["segmented"]
    return {
        "n_points": lin["n"],
        "linear_slope_per_tick": lin["b"] * US_PER_TICK,
        "linear_intercept": lin["a"],
        "phase_transition": {
            "prefers_segmented": pt["prefers_segmented"],
            "strong_evidence": pt["strong_evidence"],
            "delta_bic_linear_minus_segmented": pt["delta_bic"],
            "knee_us": seg["breakpoint"],
            "pre_slope_per_tick": seg["b1"] * US_PER_TICK,
            "post_slope_per_tick": (seg["b1"] + seg["b2"]) * US_PER_TICK,
        },
        "significance_threshold_us": sig_us,
        "significance_threshold_ticks": (sig_us / US_PER_TICK) if sig_us is not None else None,
        "verdict": (
            "phase_transition" if pt["strong_evidence"] else "linear_scaling"
        ),
    }


def main() -> dict:
    in_path = _ROOT / "results" / "experiments" / "exp2_latency_sweep.json"
    report = json.loads(in_path.read_text())
    out = analyze(report)

    out_path = _ROOT / "results" / "experiments" / "exp2_phase_transition.json"
    out_path.write_text(json.dumps(out, indent=2, sort_keys=True))

    pt = out["phase_transition"]
    print("Experiment 2 — phase-transition analysis")
    print(f"  points fitted          : {out['n_points']}")
    print(f"  linear slope           : ${out['linear_slope_per_tick']:.2f} rent per tick of staleness")
    print(f"  dBIC (linear-segmented): {pt['delta_bic_linear_minus_segmented']:.2f} "
          f"(>0 favors a knee)")
    print(f"  prefers segmented knee : {pt['prefers_segmented']} "
          f"(strong={pt['strong_evidence']})")
    print(f"  best knee location     : {pt['knee_us']:.0f} us "
          f"(pre-slope ${pt['pre_slope_per_tick']:.2f}/tick, "
          f"post-slope ${pt['post_slope_per_tick']:.2f}/tick)")
    print(f"  CI significance thresh : {out['significance_threshold_us']} us "
          f"({out['significance_threshold_ticks']} ticks)")
    print(f"  VERDICT                : {out['verdict']}")
    return out


if __name__ == "__main__":
    main()
