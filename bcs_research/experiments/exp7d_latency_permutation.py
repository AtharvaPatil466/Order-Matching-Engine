"""Experiment 7d — does the within-class winner follow speed, slot, or tick value?

exp7b found a winner that is not ordered by speed under calibrated latencies:
at M=2 the order reverses at k >= 13, and at M=3 the middle (3-tick) maker
gains in all eighteen tape-band cells while the fastest pays for k <= 7. Two
artifacts could produce that with no economics behind it:

  slot  — makers are built and act in list order, and _split_qty gives slot 0
          the odd leftover unit, so "the maker in slot i" is not symmetric.
  value — latency resolves on a 1-tick grid and every maker requotes once per
          tick, so one particular tick value could interact with that grid.

Group "perm2"/"perm3" re-run the calibrated latency set with the latencies
assigned to the maker slots in every order (2 orders at M=2, 6 at M=3). The
identity order is exp7b's own arm, so it doubles as a reproduction check.
Group "values" keeps slots in ascending latency and changes the values:
{2, 6} and {1, 4} ticks at M=2, {1, 4, 7} and {2, 3, 4} at M=3.

Same cells as exp7b: tape band (ratios 0.0049, 0.0097), the full k grid,
calibrated environment, n=100.

Run: bcs_research/.venv/bin/python bcs_research/experiments/exp7d_latency_permutation.py <group> [n_seeds]
     group in {perm2, perm3, values}
"""
from __future__ import annotations

import itertools
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

from exp1_primary import CFG                          # noqa: E402
from exp7_3d_surface import RESIDUAL_ABORT_ABS        # noqa: E402
from exp7b_per_maker_pnl import run_arm               # noqa: E402
from run_calibrated import calibrated_cfg             # noqa: E402

TICK_US = 1000
CALIBRATED = {2: (1, 5), 3: (1, 3, 5)}               # ticks, as exp7_3d's scheme

GROUPS = {
    "perm2": [(2, "perm", list(p)) for p in itertools.permutations(CALIBRATED[2])],
    "perm3": [(3, "perm", list(p)) for p in itertools.permutations(CALIBRATED[3])],
    "values": [(2, "values", [2, 6]), (2, "values", [1, 4]),
               (3, "values", [1, 4, 7]), (3, "values", [2, 3, 4])],
}


def main(group: str, n_seeds: int = 100) -> dict:
    cfg = {**CFG, **calibrated_cfg()}
    t_start = time.time()
    arms = []
    for n_makers, label, ticks in GROUPS[group]:
        lats = [t * TICK_US for t in ticks]
        arms.append(run_arm(n_makers, f"{label}:{ticks}", cfg, n_seeds, lats=lats))

    resid_max = max(a["max_abs_residual"] for a in arms)
    report = {
        "group": group,
        "config": cfg,
        "n_seeds": n_seeds,
        "arms": arms,
        "max_abs_residual": resid_max,
        "conservation_ok": resid_max < RESIDUAL_ABORT_ABS,
        "elapsed_sec": time.time() - t_start,
    }
    out = _ROOT / "results" / "experiments" / f"exp7d_latency_permutation_{group}.json"
    out.write_text(json.dumps(report, indent=2, sort_keys=True))
    print(f"\nwrote {out.name} — max |residual| {resid_max:.2e}, "
          f"{report['elapsed_sec'] / 60:.1f} min", flush=True)
    return report


if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in GROUPS:
        sys.exit(f"usage: exp7d_latency_permutation.py {{{','.join(GROUPS)}}} [n_seeds]")
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 100)
