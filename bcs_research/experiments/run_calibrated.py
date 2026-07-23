#!/usr/bin/env python3
"""Re-run Experiments 1-4 at the BTCUSDT-perp calibrated operating point.

The pre-registered operating point (exp1_primary.CFG) was chosen for mechanism
clarity, not realism. This driver replays the same four experiments with the
market parameters inverted from real order flow in calibration/fit.py, writing
every result to a `*_calibrated.json` sibling so the operating-point artifacts
that the paper's headline numbers cite stay byte-identical.

Two parameters are NOT read off the fit, because the data does not pin them.
Both are stated here rather than chosen silently (paper S3.7):

  hft_qty      Set to quote_qty, preserving the operating point's ratio of 1.0
               so that one snipe still clears the top quote — the mechanism the
               arms race runs on. Leaving it at 50 against a 2061-unit quote
               makes a snipe take 2.4% of the book and changes the experiment
               rather than rescaling it. Reported as a robustness arm.

  min_quote_qty  Left at 1. At calibrated depth it is inert: the thinning floor
               is set by spread_cap_mult (the MM bottoms out at ~226 units in
               every calibrated cell regardless of k), so this knob never binds.

adverse_sensitivity is re-calibrated, not inherited. Widening is applied per
adverse fill EVENT, so it is not scale-free: calibrated flow raises the MM's
pickoff rate from 0.20 to 0.86 per tick and the operating-point value of 0.1
drives the no-HFT spread to 5.36x base — with no HFT in the market at all.
calibrate_adverse_selection.py re-run under calibrated flow selects 0.015 on
the same criterion the published grid used (no-HFT spread ~1.1x base, positive
with-HFT divergence), holding spread_decay at its pre-registered 0.1.

Run: bcs_research/.venv/bin/python bcs_research/experiments/run_calibrated.py
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_ROOT = _HERE.parent
for _d in ("build", "agents", "simulation", "metrics", "experiments"):
    _p = str(_ROOT / _d)
    if _p not in sys.path:
        sys.path.insert(0, _p)

import exp1_primary                # noqa: E402
import exp2_latency_sweep          # noqa: E402
import exp3_hft_count              # noqa: E402
import exp4_batch_auction          # noqa: E402

FIT_PATH = _ROOT / "results" / "calibration" / "fit_btcusdt.json"

# Re-calibrated under calibrated flow; see module docstring and
# results/experiments/adverse_selection_calibration_calibrated.json.
CALIBRATED_ADVERSE_SENSITIVITY = 0.015


def calibrated_cfg(hft_qty: int | None = None) -> dict:
    """Market parameters inverted from BTCUSDT-perp order flow."""
    fit = json.loads(FIT_PATH.read_text())
    size, fitted = fit["size_model"], fit["fitted"]
    quote_qty = int(size["quote_qty_units"])
    return {
        "lambda_per_tick": float(fitted["lambda_per_tick"]),
        "sigma": float(fitted["sigma"]),
        "size_sigma_ln": float(size["sigma_ln"]),
        "noise_qty": int(size["median_units"]),
        "quote_qty": quote_qty,
        "hft_qty": quote_qty if hft_qty is None else int(hft_qty),
        "adverse_sensitivity": CALIBRATED_ADVERSE_SENSITIVITY,
    }


def main(n_seeds: int = 100) -> None:
    cfg = calibrated_cfg()
    print("Calibrated operating point:")
    for k in sorted(cfg):
        print(f"  {k:<24}{cfg[k]}")
    print(f"  n_seeds                 {n_seeds}\n")

    for label, mod, out_name in (
        ("Experiment 1 (primary)", exp1_primary, "exp1_primary_calibrated.json"),
        ("Experiment 2 (latency sweep)", exp2_latency_sweep,
         "exp2_latency_sweep_calibrated.json"),
        ("Experiment 3 (HFT count)", exp3_hft_count, "exp3_hft_count_calibrated.json"),
        ("Experiment 4 (batch auction)", exp4_batch_auction,
         "exp4_batch_auction_calibrated.json"),
    ):
        print(f"\n{'=' * 72}\n{label} — calibrated\n{'=' * 72}", flush=True)
        mod.main(n_seeds=n_seeds, cfg=cfg, out_name=out_name)


if __name__ == "__main__":
    main(n_seeds=int(sys.argv[1]) if len(sys.argv) > 1 else 100)
