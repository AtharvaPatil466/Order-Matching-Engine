#!/usr/bin/env python3
"""Extract BCS calibration target moments from AlphaForge BTC order-flow data.

Reads the AlphaForge Phase 0 collector's parquet (20-level book snapshots +
aggTrade stream, BTCUSDT perp). The collector drops a few hours most days
(host sleep; see its COLLECTOR_NOTES.md), so demanding 24/24 hourly files
admits only two days ever recorded. Days are instead admitted at
MIN_HOURS_PER_DAY coverage and every moment is made gap-safe:

- trade arrival rate accumulates duration PER FILE, so missing hours leave
  the denominator rather than inflating it (a whole-day max-min span over a
  21/24 day biases lambda down ~12%);
- realized vol keeps only returns between ADJACENT one-second buckets, so no
  return straddles a gap.

Both were the real reason for the 24/24 gate; with them fixed the gate buys
nothing but sample size. Writes the target moment vector that fit.py inverts.

Time-scale convention: TAU_S = 0.1, i.e. 1 sim tick = 100 ms of BTC
wall-clock. This keeps the BCS fast/slow asymmetry at realistic crypto
scales — the market maker's 3000 us latency maps to 300 ms of quote
staleness (slow but plausible), while HFTs react within a tick. At
1 tick = 1 s the MM would be ~3 s stale, implausible even for a slow desk.

Run: bcs_research/.venv/bin/python bcs_research/calibration/targets.py
"""
from __future__ import annotations

import json
import math
import statistics
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

TAU_S = 0.1                 # seconds of real time per sim tick
EXCHANGE_TICK = 0.1         # BTCUSDT perp price increment, USD
MIN_HOURS_PER_DAY = 18      # coverage floor per stream; see module docstring
AF_DATA = Path.home() / "Quant Projects" / "Quant Alpha" / \
    "alphaforge-microstructure" / "data"
OUT = Path(__file__).resolve().parents[1] / "results" / "calibration"


def usable_days(data_dir: Path, min_hours: int = MIN_HOURS_PER_DAY) -> list[str]:
    """UTC days with at least `min_hours` hourly files in BOTH streams.

    The current UTC day is excluded: the collector is mid-write on its newest
    file, which has no parquet footer yet and fails to read.
    """
    today = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    days = []
    for d in sorted(p.name for p in (data_dir / "trades").iterdir() if p.is_dir()):
        if d >= today:
            continue
        t = len(list((data_dir / "trades" / d).glob("*.parquet")))
        b = len(list((data_dir / "book_snapshots" / d).glob("*.parquet")))
        if t >= min_hours and b >= min_hours:
            days.append(d)
    return days


def trade_moments(per_file) -> dict:
    """per_file: iterable of (ts_ns, sizes, is_buyer_maker), one per HOURLY file.

    Arrival rate sums each file's own observed span, so a day missing hours
    contributes only the time it actually covered (a per-day max-min span
    would charge the missing hours to the denominator). Size quantiles pool
    everything. is_buyer_maker=True means the AGGRESSOR was the seller, so
    the aggressive-buy fraction counts False.

    Consumed lazily and reduced to numpy per file: the full sample is tens of
    millions of trades, and only the sizes need to survive the loop.
    """
    total_n = 0
    total_dur = 0.0
    size_chunks: list[np.ndarray] = []
    n_buy_aggr = 0
    for ts_ns, file_sizes, ibm in per_file:
        ts = np.asarray(ts_ns, dtype=np.int64)
        if ts.size == 0:
            continue
        sz = np.asarray(file_sizes, dtype=float)
        total_n += sz.size
        total_dur += float(ts.max() - ts.min()) / 1e9
        size_chunks.append(sz)
        n_buy_aggr += len(ibm) - int(np.count_nonzero(ibm))
    sizes = np.concatenate(size_chunks)
    p50, p90, p99 = (float(x) for x in np.percentile(sizes, [50, 90, 99]))
    return {
        "lambda_per_s": total_n / total_dur,
        "size_p50": p50, "size_p90": p90, "size_p99": p99,
        "size_mean": float(sizes.mean()),
        "buyer_fraction": n_buy_aggr / total_n,
        "n_trades": total_n,
        "duration_s": total_dur,
    }


def book_moments(ts_ns: list[int], mid: list[float], spread: list[float],
                 bid_sz1: list[float], ask_sz1: list[float]) -> dict:
    """Spread/depth distributions + 1 s realized vol from snapshot rows."""
    spread_bp = sorted(s / m * 1e4 for s, m in zip(spread, mid) if m > 0)
    spread_ticks = sorted(s / EXCHANGE_TICK for s in spread)
    depth = sorted((b + a) / 2 for b, a in zip(bid_sz1, ask_sz1))

    # Last mid per 1 s bucket -> log returns -> stdev, in bp per sqrt(s).
    # Only ADJACENT buckets contribute: a return spanning a missing hour (or
    # any snapshot gap) is not a one-second return and would inflate the vol.
    by_sec: dict[int, float] = {}
    for t, m in zip(ts_ns, mid):
        if m > 0:
            by_sec[t // 1_000_000_000] = m
    secs = sorted(by_sec)
    lr = [math.log(by_sec[secs[i + 1]] / by_sec[secs[i]])
          for i in range(len(secs) - 1) if secs[i + 1] - secs[i] == 1]
    n = len(spread_bp)
    return {
        "spread_p50_bp": spread_bp[n // 2],
        "spread_p90_bp": spread_bp[int(0.9 * n)],
        "spread_p50_ticks": spread_ticks[len(spread_ticks) // 2],
        "top_depth_p50": depth[len(depth) // 2],
        "rv_1s_bp": statistics.stdev(lr) * 1e4,
        "n_snapshots": len(mid),
    }


def _read_parquet(path: Path, columns: list[str]):
    """Read one hourly file, or None if it is truncated.

    The collector occasionally dies mid-write and leaves a file with no
    parquet footer. Those hours are dropped rather than failing the run;
    per-file duration accounting means a dropped hour costs only its own
    sample, not the day.
    """
    import pyarrow.parquet as pq
    from pyarrow.lib import ArrowInvalid

    try:
        return pq.read_table(path, columns=columns)
    except ArrowInvalid:
        return None


def main() -> dict:
    days = usable_days(AF_DATA)
    if not days:
        raise SystemExit(f"no days with >={MIN_HOURS_PER_DAY} hours in both "
                         f"streams under {AF_DATA}")

    skipped: list[str] = []

    def trade_files():
        """Yield one (ts_ns, sizes, is_buyer_maker) per readable hourly file."""
        cols = ["exchange_ts_ns", "size", "is_buyer_maker"]
        for d in days:
            for f in sorted((AF_DATA / "trades" / d).glob("*.parquet")):
                t = _read_parquet(f, cols)
                if t is None:
                    skipped.append(f"trades/{d}/{f.name}")
                    continue
                yield (t.column("exchange_ts_ns").to_numpy(),
                       t.column("size").to_numpy(),
                       t.column("is_buyer_maker").to_numpy(zero_copy_only=False))

    tm = trade_moments(trade_files())

    b_ts, b_mid, b_spr, b_bid, b_ask = [], [], [], [], []
    cols = ["exchange_ts_ns", "mid", "spread", "bid_sz_1", "ask_sz_1"]
    for d in days:
        for f in sorted((AF_DATA / "book_snapshots" / d).glob("*.parquet")):
            t = _read_parquet(f, cols)
            if t is None:
                skipped.append(f"book_snapshots/{d}/{f.name}")
                continue
            b_ts.extend(t.column("exchange_ts_ns").to_pylist())
            b_mid.extend(t.column("mid").to_pylist())
            b_spr.extend(t.column("spread").to_pylist())
            b_bid.extend(t.column("bid_sz_1").to_pylist())
            b_ask.extend(t.column("ask_sz_1").to_pylist())

    targets = {
        "symbol": "BTCUSDT-perp", "days": days, "n_days": len(days),
        "min_hours_per_day": MIN_HOURS_PER_DAY,
        "skipped_files": skipped,
        "tau_s": TAU_S, "exchange_tick": EXCHANGE_TICK,
        **tm,
        **book_moments(b_ts, b_mid, b_spr, b_bid, b_ask),
    }
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "targets_btcusdt.json").write_text(json.dumps(targets, indent=2, sort_keys=True))
    for k, v in sorted(targets.items()):
        if isinstance(v, float):
            print(f"{k:>18}: {v:.6g}")
        else:
            print(f"{k:>18}: {v}")
    return targets


if __name__ == "__main__":
    main()
