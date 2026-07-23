#!/usr/bin/env python3
"""Depth-depletion episodes and replenishment half-life on the BTC tape.

Research plan step 3. Two open items need this one measurement:

  * The multi-maker model (step 4) cannot be designed without knowing how fast
    real top-of-book depth recovers after being consumed. Replenishment — not
    maker headcount — is the mechanically load-bearing quantity, and headcount
    is not observable anyway: Binance's depth stream publishes quantity per
    level with no order counts.
  * The Hawkes real-data leg (paper §5.3) is blocked on a depth-depletion gap
    definition for books that never empty. `metrics/liquidity_gap_detector.py`
    defines a gap as a one-sided book, which never occurs on a real perp.

COVERAGE IS THE BINDING CONSTRAINT, not sample size. Snapshot cadence is a
clean 100 ms whenever the collector is up (median inter-snapshot gap 0.102 s),
but hourly coverage is bimodal: an hour file holds either ~35,290 rows (a
complete hour) or a few hundred (a badly punctured one; median hour is ~3,200).
Overall book coverage is ~34%. A depletion-and-recovery episode spans seconds
and cannot be measured across a hole, so this module uses COMPLETE HOURS ONLY —
172 of 841, about 7.2 days of contiguous 10 Hz book. Partial hours are counted
and reported, never silently mixed in.

Method. On each complete hour, per side, top-of-book depth is compared against
a trailing median baseline. A depletion event opens when depth falls below
`DEPLETION_FRACTION` of that baseline; recovery is the elapsed time until depth
first regains `RECOVERY_FRACTION` of it. Events that do not recover inside
`MAX_RECOVERY_S` are recorded as censored rather than dropped, so the half-life
is not biased down by discarding the slow tail.

Run: bcs_research/.venv/bin/python bcs_research/calibration/depth_replenishment.py
"""
from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pyarrow.parquet as pq

AF_DATA = Path.home() / "Quant Projects" / "Quant Alpha" / \
    "alphaforge-microstructure" / "data" / "book_snapshots"
OUT = Path(__file__).resolve().parents[1] / "results" / "calibration"

CADENCE_S = 0.102              # measured median inter-snapshot gap
FULL_HOUR_ROWS = 30_000        # >= this many rows counts as a complete hour
BASELINE_WINDOW = 300          # trailing snapshots (~30 s) for the depth baseline
DEPLETION_FRACTION = 0.25      # depth below 25% of baseline opens an episode
RECOVERY_FRACTION = 0.50       # regaining 50% of baseline closes it (half-life)
MAX_RECOVERY_S = 30.0          # beyond this an episode is censored, not dropped
MIN_BASELINE_BTC = 0.05        # ignore stretches where the book is trivially thin


def complete_hours(data_dir: Path) -> list[Path]:
    """Hour files with near-full 10 Hz coverage, cheapest test first.

    Row count comes from the parquet footer, so this does not read any data.
    """
    files = []
    for day in sorted(p for p in data_dir.iterdir() if p.is_dir()):
        for f in sorted(day.glob("*.parquet")):
            try:
                if pq.ParquetFile(f).metadata.num_rows >= FULL_HOUR_ROWS:
                    files.append(f)
            except Exception:
                continue
    return files


def _trailing_median(x: np.ndarray, window: int) -> np.ndarray:
    """Median of the `window` samples ending just before each index."""
    n = len(x)
    out = np.full(n, np.nan)
    if n <= window:
        return out
    strided = np.lib.stride_tricks.sliding_window_view(x, window)
    out[window:] = np.median(strided[:-1], axis=1)
    return out


def episodes_for_side(depth: np.ndarray, ts_ns: np.ndarray) -> list[dict]:
    """Depletion episodes and their recovery times for one side of the book."""
    base = _trailing_median(depth, BASELINE_WINDOW)
    usable = ~np.isnan(base) & (base >= MIN_BASELINE_BTC)
    open_at = usable & (depth < DEPLETION_FRACTION * base)
    max_steps = int(MAX_RECOVERY_S / CADENCE_S)

    out, i, n = [], 0, len(depth)
    idx = np.flatnonzero(open_at)
    for start in idx:
        if start < i:            # still inside the previous episode
            continue
        target = RECOVERY_FRACTION * base[start]
        stop = min(start + max_steps, n)
        rec = np.flatnonzero(depth[start:stop] >= target)
        if rec.size:
            k = int(rec[0])
            out.append({
                "recovery_s": float((ts_ns[start + k] - ts_ns[start]) / 1e9),
                "censored": False,
                "baseline_btc": float(base[start]),
                "trough_btc": float(depth[start:start + k + 1].min()),
            })
            i = start + k + 1
        else:
            out.append({
                "recovery_s": MAX_RECOVERY_S,
                "censored": True,
                "baseline_btc": float(base[start]),
                "trough_btc": float(depth[start:stop].min()),
            })
            i = stop
    return out


def scan_file(path: Path) -> list[dict]:
    cols = ["exchange_ts_ns", "bid_sz_1", "ask_sz_1"]
    t = pq.read_table(path, columns=cols)
    ts = t["exchange_ts_ns"].to_numpy()
    order = np.argsort(ts)
    ts = ts[order]
    events = []
    for side in ("bid_sz_1", "ask_sz_1"):
        depth = np.asarray(t[side].to_numpy(), dtype=float)[order]
        for e in episodes_for_side(depth, ts):
            e["side"] = side[:3]
            events.append(e)
    return events


def summarize(events: list[dict], n_files: int, n_partial: int) -> dict:
    rec = np.array([e["recovery_s"] for e in events])
    cens = np.array([e["censored"] for e in events])
    depth_ratio = np.array([e["trough_btc"] / e["baseline_btc"] for e in events])
    hours = n_files  # one file == one hour
    return {
        "complete_hours_used": n_files,
        "partial_hours_skipped": n_partial,
        "coverage_note": "complete hours only; partial hours are never mixed in",
        "n_episodes": len(events),
        "episodes_per_hour": float(len(events) / hours) if hours else 0.0,
        "censored_fraction": float(cens.mean()) if len(cens) else 0.0,
        "recovery_s": {
            "p25": float(np.percentile(rec, 25)),
            "median": float(np.median(rec)),
            "p75": float(np.percentile(rec, 75)),
            "p90": float(np.percentile(rec, 90)),
            "mean": float(rec.mean()),
        },
        "trough_over_baseline": {
            "median": float(np.median(depth_ratio)),
            "p10": float(np.percentile(depth_ratio, 10)),
        },
        "params": {
            "cadence_s": CADENCE_S,
            "baseline_window_snapshots": BASELINE_WINDOW,
            "depletion_fraction": DEPLETION_FRACTION,
            "recovery_fraction": RECOVERY_FRACTION,
            "max_recovery_s": MAX_RECOVERY_S,
            "min_baseline_btc": MIN_BASELINE_BTC,
        },
    }


def main() -> dict:
    files = complete_hours(AF_DATA)
    total = sum(1 for d in AF_DATA.iterdir() if d.is_dir()
                for _ in d.glob("*.parquet"))
    print(f"complete hours: {len(files)} of {total} hour files "
          f"({100*len(files)/total:.1f}%) — partial hours excluded, not mixed in")

    events = []
    for i, f in enumerate(files, 1):
        try:
            events.extend(scan_file(f))
        except Exception as exc:
            print(f"  skipped {f.parent.name}/{f.name}: {type(exc).__name__}")
        if i % 40 == 0:
            print(f"  {i}/{len(files)} hours, {len(events)} episodes", flush=True)

    summary = summarize(events, len(files), total - len(files))
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "depth_replenishment.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True))

    r = summary["recovery_s"]
    print(f"\nepisodes            {summary['n_episodes']:,} "
          f"({summary['episodes_per_hour']:.1f}/h)")
    print(f"censored            {100*summary['censored_fraction']:.1f}% "
          f"(no recovery within {MAX_RECOVERY_S:.0f} s)")
    print(f"recovery to {int(RECOVERY_FRACTION*100)}%     "
          f"p25 {r['p25']:.2f}s   median {r['median']:.2f}s   "
          f"p75 {r['p75']:.2f}s   p90 {r['p90']:.2f}s")
    print(f"trough/baseline     median {summary['trough_over_baseline']['median']:.3f}")
    print(f"\nwrote results/calibration/depth_replenishment.json")
    return summary


if __name__ == "__main__":
    main()
