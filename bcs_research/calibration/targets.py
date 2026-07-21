#!/usr/bin/env python3
"""Extract BCS calibration target moments from AlphaForge BTC order-flow data.

Reads the AlphaForge Phase 0 collector's parquet (20-level book snapshots +
aggTrade stream, BTCUSDT perp), restricted to COMPLETE UTC days — 24/24
hourly files in BOTH streams — because the pre-2026-07-19 window has
host-sleep gaps clustered overnight IST (see the collector's
COLLECTOR_NOTES.md); moment estimates over gap days would over-weight
daytime hours. Writes the target moment vector that fit.py inverts.

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
from pathlib import Path

TAU_S = 0.1                 # seconds of real time per sim tick
EXCHANGE_TICK = 0.1         # BTCUSDT perp price increment, USD
AF_DATA = Path.home() / "Quant Projects" / "Quant Alpha" / \
    "alphaforge-microstructure" / "data"
OUT = Path(__file__).resolve().parents[1] / "results" / "calibration"


def complete_days(data_dir: Path) -> list[str]:
    """UTC days with 24 hourly files in both streams."""
    days = []
    for d in sorted(p.name for p in (data_dir / "trades").iterdir() if p.is_dir()):
        t = len(list((data_dir / "trades" / d).glob("*.parquet")))
        b = len(list((data_dir / "book_snapshots" / d).glob("*.parquet")))
        if t == 24 and b == 24:
            days.append(d)
    return days


def trade_moments(per_day: list[tuple[list[int], list[float], list[bool]]]) -> dict:
    """per_day: [(ts_ns, sizes, is_buyer_maker), ...] one tuple per UTC day.

    Arrival rate uses within-day durations summed (days need not be
    contiguous); size quantiles pool all days. is_buyer_maker=True means the
    AGGRESSOR was the seller, so aggressive-buy fraction counts False.
    """
    total_n = 0
    total_dur = 0.0
    sizes: list[float] = []
    n_buy_aggr = 0
    for ts_ns, day_sizes, ibm in per_day:
        total_n += len(day_sizes)
        total_dur += (max(ts_ns) - min(ts_ns)) / 1e9
        sizes.extend(day_sizes)
        n_buy_aggr += sum(1 for b in ibm if not b)
    q = statistics.quantiles(sizes, n=100)
    return {
        "lambda_per_s": total_n / total_dur,
        "size_p50": q[49], "size_p90": q[89], "size_p99": q[98],
        "size_mean": statistics.fmean(sizes),
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
    # Buckets are per-day in practice (snapshots arrive densely), and any
    # cross-gap return is a single outlier among ~86k/day; acceptable here.
    by_sec: dict[int, float] = {}
    for t, m in zip(ts_ns, mid):
        if m > 0:
            by_sec[t // 1_000_000_000] = m
    mids = [by_sec[s] for s in sorted(by_sec)]
    lr = [math.log(mids[i + 1] / mids[i]) for i in range(len(mids) - 1)]
    n = len(spread_bp)
    return {
        "spread_p50_bp": spread_bp[n // 2],
        "spread_p90_bp": spread_bp[int(0.9 * n)],
        "spread_p50_ticks": spread_ticks[len(spread_ticks) // 2],
        "top_depth_p50": depth[len(depth) // 2],
        "rv_1s_bp": statistics.stdev(lr) * 1e4,
        "n_snapshots": len(mid),
    }


def main() -> dict:
    import pyarrow.parquet as pq

    days = complete_days(AF_DATA)
    if not days:
        raise SystemExit("no complete 24/24 days found under " + str(AF_DATA))

    per_day = []
    b_ts, b_mid, b_spr, b_bid, b_ask = [], [], [], [], []
    for d in days:
        ts, sz, ibm = [], [], []
        for f in sorted((AF_DATA / "trades" / d).glob("*.parquet")):
            t = pq.read_table(f, columns=["exchange_ts_ns", "size", "is_buyer_maker"])
            ts.extend(t.column("exchange_ts_ns").to_pylist())
            sz.extend(t.column("size").to_pylist())
            ibm.extend(t.column("is_buyer_maker").to_pylist())
        per_day.append((ts, sz, ibm))
        cols = ["exchange_ts_ns", "mid", "spread", "bid_sz_1", "ask_sz_1"]
        for f in sorted((AF_DATA / "book_snapshots" / d).glob("*.parquet")):
            t = pq.read_table(f, columns=cols)
            b_ts.extend(t.column("exchange_ts_ns").to_pylist())
            b_mid.extend(t.column("mid").to_pylist())
            b_spr.extend(t.column("spread").to_pylist())
            b_bid.extend(t.column("bid_sz_1").to_pylist())
            b_ask.extend(t.column("ask_sz_1").to_pylist())

    targets = {
        "symbol": "BTCUSDT-perp", "days": days, "tau_s": TAU_S,
        "exchange_tick": EXCHANGE_TICK,
        **trade_moments(per_day),
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
