#!/usr/bin/env python3
"""Exp 4 analysis — minimum batch interval that tames the arms race.

Consumes results/experiments/exp4_batch_auction.json and reports, relative to the
continuous LOB baseline, two reduction curves and the smallest batch interval
that reaches given thresholds:

  rent_elimination = 1 - rent(interval) / rent(continuous)
  mm_recovery      = 1 - mm_loss(interval) / mm_loss(continuous)
                     where mm_loss = -delta(MM PnL) >= 0

Both are reported because they answer the same question with different
robustness. HFT rent in the batch arm is high-variance (uniform clearing makes
the HFT's P&L lumpy), so its CIs are wide and the "X% of rent eliminated" number
is noisy. The MM-welfare-recovery curve is monotone and tight, so it is the
statistically sound version of the policy punchline.

Run: bcs_research/.venv/bin/python bcs_research/analysis/exp4_analysis.py
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_ROOT = _HERE.parent
sys.path.insert(0, str(_HERE))

CONTINUOUS = "continuous"
THRESHOLDS = (0.50, 0.90)


def _finite_cells(report: dict) -> list:
    cells = [c for c in report["cells"] if c["batch_interval"] != CONTINUOUS]
    return sorted(cells, key=lambda c: c["batch_interval"])


def _continuous(report: dict) -> dict:
    return next(c for c in report["cells"] if c["batch_interval"] == CONTINUOUS)


def _min_interval_for(curve: list, threshold: float):
    """Smallest interval (curve is [(interval, fraction)] sorted) reaching threshold."""
    for interval, frac in curve:
        if frac >= threshold:
            return interval
    return None


def analyze(report: dict) -> dict:
    cont = _continuous(report)
    cont_rent = cont["treatment_ci"]["hft_rent"]["mean"]
    cont_mm_loss = -cont["delta_ci"]["mm_pnl"]["mean"]

    rent_curve = []
    mm_curve = []
    per_interval = []
    for c in _finite_cells(report):
        iv = c["batch_interval"]
        rent = c["treatment_ci"]["hft_rent"]["mean"]
        mm_loss = -c["delta_ci"]["mm_pnl"]["mean"]
        rent_elim = (1.0 - rent / cont_rent) if cont_rent else 0.0
        mm_recovery = (1.0 - mm_loss / cont_mm_loss) if cont_mm_loss else 0.0
        rent_curve.append((iv, rent_elim))
        mm_curve.append((iv, mm_recovery))
        per_interval.append({
            "interval": iv,
            "hft_rent": rent,
            "hft_rent_ci": [c["treatment_ci"]["hft_rent"]["lower"],
                            c["treatment_ci"]["hft_rent"]["upper"]],
            "rent_elimination": rent_elim,
            "mm_loss": mm_loss,
            "mm_recovery": mm_recovery,
            "liquidity_gaps": c["treatment_ci"]["liquidity_gaps"]["mean"],
        })

    return {
        "continuous_rent": cont_rent,
        "continuous_mm_loss": cont_mm_loss,
        "continuous_gaps": cont["treatment_ci"]["liquidity_gaps"]["mean"],
        "per_interval": per_interval,
        "min_interval_rent_elim": {
            f"{int(t*100)}pct": _min_interval_for(rent_curve, t) for t in THRESHOLDS
        },
        "min_interval_mm_recovery": {
            f"{int(t*100)}pct": _min_interval_for(mm_curve, t) for t in THRESHOLDS
        },
    }


def main() -> dict:
    in_path = _ROOT / "results" / "experiments" / "exp4_batch_auction.json"
    report = json.loads(in_path.read_text())
    out = analyze(report)
    out_path = _ROOT / "results" / "experiments" / "exp4_thresholds.json"
    out_path.write_text(json.dumps(out, indent=2, sort_keys=True))

    print("Experiment 4 — batch interval thresholds (vs continuous LOB)")
    print(f"  continuous: HFT rent ${out['continuous_rent']:.1f}, "
          f"MM loss ${out['continuous_mm_loss']:.1f}, gaps {out['continuous_gaps']:.1f}")
    print(f"  {'interval':>9}{'rent':>9}{'rent_elim':>11}{'mm_recovery':>13}{'gaps':>8}")
    for r in out["per_interval"]:
        print(f"  {r['interval']:>9}{r['hft_rent']:>9.1f}"
              f"{r['rent_elimination']*100:>10.1f}%{r['mm_recovery']*100:>12.1f}%"
              f"{r['liquidity_gaps']:>8.1f}")
    print(f"  min interval for >=50% rent eliminated : {out['min_interval_rent_elim']['50pct']}")
    print(f"  min interval for >=50% MM loss recovered: {out['min_interval_mm_recovery']['50pct']}")
    print(f"  min interval for >=90% MM loss recovered: {out['min_interval_mm_recovery']['90pct']}")
    return out


if __name__ == "__main__":
    main()
