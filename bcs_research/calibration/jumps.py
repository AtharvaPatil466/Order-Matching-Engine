#!/usr/bin/env python3
"""Jump intensity, jump sizes and jump-free volatility of the tape, 10 ms to 1 s.

The event model's fundamental is a jump diffusion (agents/fundamental_paths.py,
`JumpDiffusionPath.from_calibration`): Brownian steps every
`update_interval_ms` plus compound-Poisson Laplace jumps. Jumps are what make
quotes stale, so in BCS they are where sniping comes from; their rate and size
must come from the tape, not from a choice.

Method — a Lee–Mykland style threshold test, applied per hourly file (each is
hole-free, see fine_grain.py) at each sampling interval:

  1. Price: the trade-implied mid (fine_grain.implied_mid), sampled
     previous-tick on the grid. Price changes are stamped to the millisecond.
  2. Local diffusive scale: BIPOWER variation of the hour's 1-MINUTE returns.
     Not 1 s: 70% of 1 s returns are exactly zero (the perp's top of book is
     sticky and moves in bursts), so neighbour products — and iterative
     truncation — collapse to ~0 in quiet hours and flag single ticks as
     jumps. At 1 minute zeros are rare and bipower stays jump-robust.
  3. A return is a jump when |r| > z* · sigma_local · sqrt(dt), with z* the
     Lee–Mykland (2008) Gumbel critical value for a day's worth of tests at
     that interval (alpha = 1%), i.e. larger than the largest move a
     diffusion at the local scale would produce in a day, with 99% confidence.
  4. Diffusive sigma = realized variance of the non-jump returns (threshold-
     truncated RV, Mancini 2009); jump intensity = jumps / covered time;
     Laplace scale = MLE on the jump sizes (location 0, b = mean |J|).

By construction total RV = diffusive RV + sum of squared jumps, so the
decomposition the consumer relies on (total ~ sigma_d^2 + lambda E[J^2]) holds
exactly for the measured E[J^2]; how close the LAPLACE second moment 2 b^2
comes to it is reported as the fit check, with two KS distances (whole jump
law, and the exceedance tail, which is what a thresholded sample can test).

What a Lévy path cannot match, stated rather than hidden: the tape's
volatility RISES with horizon (positive autocorrelation below 1 s), while a
jump diffusion's is flat. Parameters fitted at one interval therefore
reproduce that interval's moments and miss the others; all three fits are
written so the choice of update interval is visible.

Run: bcs_research/.venv/bin/python bcs_research/calibration/jumps.py
"""
from __future__ import annotations

import json
import math
import sys
from pathlib import Path

import numpy as np
from scipy import stats

sys.path.insert(0, str(Path(__file__).resolve().parent))

from fine_grain import (INTERVALS_MS, OUT, SAMPLE_DAYS, implied_mid,  # noqa: E402
                        on_grid, trade_hours)

UPDATE_INTERVAL_MS = 10        # the reviewer's resolution; finest the tape stamps cleanly
SCALE_MS = 60_000              # local-scale returns (see module docstring, step 2)
MIN_SCALE_RETURNS = 20         # an hour needs >= 20 one-minute returns to be tested
ALPHA = 0.01
MS_PER_DAY = 86_400_000


def lm_critical_z(n: int, alpha: float = ALPHA) -> float:
    """Lee–Mykland threshold on |r| / (sigma sqrt(dt)) for n tests.

    The max of n |N(0,1)| draws is approximately Gumbel; a return beyond this
    exceeds what pure diffusion produces over n draws with prob. 1 - alpha.
    """
    a = math.sqrt(2 * math.log(n))
    centre = a - (math.log(math.pi) + math.log(math.log(n))) / (2 * a)
    return centre - math.log(-math.log(1 - alpha)) / a


def local_sigma(ts_ms: np.ndarray, log_mid_bp: np.ndarray) -> float | None:
    """Jump-robust diffusive scale of one hour, bp per sqrt(s), or None if too short.

    ponytail: one scale per hourly file; a rolling Lee–Mykland window would
    track volatility shifts inside the hour if they turn out to matter.
    """
    r = np.diff(on_grid(ts_ms, log_mid_bp, SCALE_MS))
    if r.size < MIN_SCALE_RETURNS:
        return None
    bv = math.pi / 2 * float(np.mean(np.abs(r[1:]) * np.abs(r[:-1])))
    return math.sqrt(bv / (SCALE_MS / 1e3))


class JumpAccumulator:
    """Pools jumps and variance at one sampling interval across hourly files."""

    def __init__(self, dt_ms: int):
        self.dt_ms = dt_ms
        self.z = lm_critical_z(MS_PER_DAY // dt_ms)
        self.jumps: list[np.ndarray] = []
        self.thresholds: list[np.ndarray] = []
        self.rv = self.rv_diffusive = 0.0
        self.dur_s = 0.0

    def add(self, ts_ms: np.ndarray, log_mid_bp: np.ndarray, sigma: float) -> None:
        dt_s = self.dt_ms / 1e3
        r = np.diff(on_grid(ts_ms, log_mid_bp, self.dt_ms))
        c = self.z * sigma * math.sqrt(dt_s)
        jump = np.abs(r) > c
        self.jumps.append(r[jump])
        self.thresholds.append(np.full(int(jump.sum()), c))
        self.rv += float(r @ r)
        self.rv_diffusive += float(r[~jump] @ r[~jump])
        self.dur_s += r.size * dt_s

    def summary(self) -> dict:
        J = np.concatenate(self.jumps)
        excess = np.abs(J) - np.concatenate(self.thresholds)
        lam = J.size / self.dur_s
        b = float(np.abs(J).mean())              # Laplace(0, b) MLE
        b_tail = float(excess.mean())            # exponential exceedances
        ej2 = float(J @ J) / J.size
        sig_d = math.sqrt(self.rv_diffusive / self.dur_s)
        return {
            "lm_critical_z": self.z,
            "covered_s": self.dur_s,
            "n_jumps": int(J.size),
            "jump_intensity_per_s": lam,
            "sigma_total_bp_per_sqrt_s": math.sqrt(self.rv / self.dur_s),
            "sigma_diffusive_bp_per_sqrt_s": sig_d,
            "jump_share_of_variance": float(J @ J) / self.rv,
            "jump_scale_bp_laplace_mle": b,
            "jump_abs_bp": {f"p{q}": float(np.percentile(np.abs(J), q))
                            for q in (10, 50, 90, 99)},
            "up_share": float(np.mean(J > 0)),
            "laplace_check": {
                "measured_E_J2": ej2,
                "laplace_2b2": 2 * b * b,
                "ks_abs_jump_vs_exponential_b": float(
                    stats.kstest(np.abs(J), "expon", args=(0, b)).statistic),
                "tail_scale_bp_from_exceedances": b_tail,
                "ks_exceedance_vs_exponential": float(
                    stats.kstest(excess, "expon", args=(0, b_tail)).statistic),
            },
            # A Lévy path's variance is flat in horizon: this is its sigma at ANY interval.
            "model_total_sigma_bp_per_sqrt_s": math.sqrt(sig_d ** 2 + lam * 2 * b * b),
        }


def main() -> dict:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "build"))
    import bcs_engine as be

    acc = {dt: JumpAccumulator(dt) for dt in INTERVALS_MS}
    skipped: list[str] = []
    too_short = 0
    for _, _, tr in trade_hours(skipped=skipped):
        lm = np.log(implied_mid(tr["price"], tr["buy"])) * 1e4
        sigma = local_sigma(tr["ts_ms"], lm)
        if sigma is None:
            too_short += 1
            continue
        for a in acc.values():
            a.add(tr["ts_ms"], lm, sigma)

    by = {str(dt): a.summary() for dt, a in acc.items()}
    main_fit = by[str(UPDATE_INTERVAL_MS)]
    out = {
        "v0": 100 * be.PRICE_PRECISION,
        "sigma_bp_per_sqrt_s": main_fit["sigma_diffusive_bp_per_sqrt_s"],
        "jump_intensity_per_s": main_fit["jump_intensity_per_s"],
        "jump_size_dist": "laplace",
        "jump_scale_bp": main_fit["jump_scale_bp_laplace_mle"],
        "update_interval_ms": UPDATE_INTERVAL_MS,
        "diagnostics": {
            "sigma_total_bp_per_sqrt_s": main_fit["sigma_total_bp_per_sqrt_s"],
            "by_interval_ms": by,
            "tape_total_sigma_by_interval": {
                k: v["sigma_total_bp_per_sqrt_s"] for k, v in by.items()},
            "method": ("Lee-Mykland threshold per hourly file; local scale = bipower "
                       "of 1-min trade-implied-mid returns; alpha 1%; diffusive "
                       "sigma = truncated RV; Laplace b = mean |J|"),
            "sample": {"days": SAMPLE_DAYS, "hours_too_short_for_scale": too_short,
                       "skipped_files": skipped},
            "units": ("v0 is engine fixed point (100 * PRICE_PRECISION), not a BTC "
                      "price; sigma, jump scale are bp of v0"),
        },
    }
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "jump_fit_v2.json").write_text(json.dumps(out, indent=2, sort_keys=True))
    for dt, s in by.items():
        print(f"\n{dt} ms")
        for k, v in s.items():
            print(f"  {k:<34} {v}")
    print("\nwrote results/calibration/jump_fit_v2.json")
    return out


if __name__ == "__main__":
    main()
