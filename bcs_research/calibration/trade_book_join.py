#!/usr/bin/env python3
"""Join depletion episodes to the trade tape: consumption vs cancellation.

Research plan step 3b. Step 3a measured how fast top-of-book depth recovers,
but at ~1,000 episodes/hour that statistic conflates two different phenomena:
depth CONSUMED by an aggressive trade, and depth WITHDRAWN by cancellation.
Only the former is race-relevant, so 3a's quantiles are an upper bound on true
post-trade replenishment until the two are separated. This module separates
them, and in the same join measures the snipe-to-depth ratio the (ratio, k)
surface needs.

Two data corrections are applied here rather than assumed away.

1. Sweep de-fragmentation. Binance's aggregated-trade stream aggregates fills
   per taker order PER PRICE LEVEL, so one order sweeping three levels appears
   as three records. Taken raw, the size distribution systematically fragments
   exactly the multi-level sweeps that constitute race flow, biasing the
   numerator of the ratio downward. Consecutive `agg_trade_id`s sharing an
   exchange timestamp and aggressor side are regrouped into one taker order.

2. Aggressor attribution. `is_buyer_maker=True` means the resting buyer was the
   maker, so the aggressor SOLD into the bid; `False` means the aggressor
   lifted the ask. Depth consumed is therefore attributed to the bid and ask
   side respectively.

RESOLUTION LIMIT, stated rather than disclaimed. Book snapshots are 100 ms and
trade timestamps are exchange-side milliseconds, while latency arbitrage
resolves in microseconds. Any "aggressive trade shortly after a price move"
filter is therefore diluted by roughly three orders of magnitude, and the
conditional ratio reported here is a bound on the race ratio, not a measurement
of it. A placebo conditioned on NON-move windows is computed alongside so the
dilution is quantified rather than merely acknowledged.

Run: bcs_research/.venv/bin/python bcs_research/calibration/trade_book_join.py
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
import pyarrow.parquet as pq

sys.path.insert(0, str(Path(__file__).resolve().parent))

from depth_replenishment import (                          # noqa: E402
    AF_DATA, BASELINE_WINDOW, CADENCE_S, DEPLETION_FRACTION,
    MAX_RECOVERY_S, MIN_BASELINE_BTC, RECOVERY_FRACTION,
    _trailing_median, complete_hours,
)

TRADES_DIR = AF_DATA.parent / "trades"
OUT = Path(__file__).resolve().parents[1] / "results" / "calibration"

# A drop is called consumption when aggressive volume on that side over the
# snapshot interval covers at least this share of the observed depth decline.
CONSUMPTION_COVERAGE = 0.5
RACE_WINDOW_S = 0.2          # "shortly after" a top-of-book move; 2 snapshots
MOVE_TICKS = 0.1             # BTCUSDT-perp price increment, USD


def taker_orders(ts_ns, size, is_buyer_maker, agg_id):
    """Regroup fragmented aggTrade records back into taker orders.

    A sweep across N price levels arrives as N records sharing one exchange
    timestamp and aggressor side, with consecutive aggregate-trade ids.
    """
    order = np.argsort(agg_id)
    ts, sz, bm = ts_ns[order], size[order], is_buyer_maker[order]
    if len(ts) == 0:
        return ts, sz, bm
    new = np.empty(len(ts), dtype=bool)
    new[0] = True
    new[1:] = (ts[1:] != ts[:-1]) | (bm[1:] != bm[:-1])
    grp = np.cumsum(new) - 1
    counts = np.bincount(grp)
    return ((np.bincount(grp, weights=ts) / counts).astype(np.int64),
            np.bincount(grp, weights=sz),
            np.bincount(grp, weights=bm.astype(float)) > 0)


def _episode_starts(depth, base):
    usable = ~np.isnan(base) & (base >= MIN_BASELINE_BTC)
    return np.flatnonzero(usable & (depth < DEPLETION_FRACTION * base))


def _recovery_s(depth, ts_ns, start, base):
    """Seconds until depth regains RECOVERY_FRACTION of baseline; None if censored."""
    stop = min(start + int(MAX_RECOVERY_S / CADENCE_S), len(depth))
    rec = np.flatnonzero(depth[start:stop] >= RECOVERY_FRACTION * base[start])
    if not rec.size:
        return None
    return float((ts_ns[start + int(rec[0])] - ts_ns[start]) / 1e9)


def classify_hour(book, trades) -> list:
    """Attribute each depletion episode on each side to consumption or cancellation."""
    ts, bid_sz, ask_sz = book["ts"], book["bid_sz"], book["ask_sz"]
    t_ts, t_sz, t_bm = trades["ts"], trades["size"], trades["buyer_maker"]

    rows = []
    for side, depth, hits_side in (("bid", bid_sz, t_bm), ("ask", ask_sz, ~t_bm)):
        base = _trailing_median(depth, BASELINE_WINDOW)
        starts = _episode_starts(depth, base)
        s_ts, s_sz = t_ts[hits_side], t_sz[hits_side]
        last = -1
        for st in starts:
            if st <= last or st == 0:
                continue
            drop = depth[st - 1] - depth[st]
            if drop <= 0:
                continue
            lo, hi = np.searchsorted(s_ts, [ts[st - 1], ts[st]])
            consumed = float(s_sz[lo:hi].sum())
            rec = _recovery_s(depth, ts, st, base)
            rows.append({
                "side": side,
                "consumption": bool(consumed >= CONSUMPTION_COVERAGE * drop),
                "consumed_btc": consumed,
                "drop_btc": float(drop),
                "recovery_s": rec,
            })
            last = st
    return rows


def ratio_sample(book, trades) -> dict:
    """Taker size over contemporaneous top depth, unconditional and race-conditional."""
    ts, bid_px, ask_px = book["ts"], book["bid_px"], book["ask_px"]
    depth = {"bid": book["bid_sz"], "ask": book["ask_sz"]}
    t_ts, t_sz, t_bm = trades["ts"], trades["size"], trades["buyer_maker"]
    if len(t_ts) == 0:
        return {"uncond": [], "race": [], "placebo": []}

    # Snapshot immediately preceding each trade: the depth it actually met.
    j = np.clip(np.searchsorted(ts, t_ts, side="right") - 1, 0, len(ts) - 1)
    side_depth = np.where(t_bm, depth["bid"][j], depth["ask"][j])
    ok = side_depth > 0
    ratio = np.divide(t_sz, side_depth, out=np.zeros_like(t_sz), where=ok)

    # Race proxy: the top of book moved on the snapshot preceding the trade.
    moved = np.zeros(len(ts), dtype=bool)
    moved[1:] = (np.abs(np.diff(bid_px)) >= MOVE_TICKS) | \
                (np.abs(np.diff(ask_px)) >= MOVE_TICKS)
    recent_move = moved[j]
    within = (t_ts - ts[j]) / 1e9 <= RACE_WINDOW_S
    race = ok & recent_move & within
    placebo = ok & ~recent_move & within      # same window, no move: dilution control
    return {
        "uncond": ratio[ok].tolist(),
        "race": ratio[race].tolist(),
        "placebo": ratio[placebo].tolist(),
    }


def _load_hour(book_path: Path):
    day, hr = book_path.parent.name, book_path.stem
    t_path = TRADES_DIR / day / f"{hr}.parquet"
    if not t_path.exists():
        return None, None
    b = pq.read_table(book_path, columns=["exchange_ts_ns", "bid_sz_1", "ask_sz_1",
                                          "bid_px_1", "ask_px_1"])
    bts = b["exchange_ts_ns"].to_numpy()
    o = np.argsort(bts)
    book = {"ts": bts[o],
            "bid_sz": np.asarray(b["bid_sz_1"].to_numpy(), float)[o],
            "ask_sz": np.asarray(b["ask_sz_1"].to_numpy(), float)[o],
            "bid_px": np.asarray(b["bid_px_1"].to_numpy(), float)[o],
            "ask_px": np.asarray(b["ask_px_1"].to_numpy(), float)[o]}
    t = pq.read_table(t_path, columns=["exchange_ts_ns", "size",
                                       "is_buyer_maker", "agg_trade_id"])
    ts, sz, bm = taker_orders(t["exchange_ts_ns"].to_numpy(),
                              np.asarray(t["size"].to_numpy(), float),
                              t["is_buyer_maker"].to_numpy(),
                              t["agg_trade_id"].to_numpy())
    k = np.argsort(ts)
    return book, {"ts": ts[k], "size": sz[k], "buyer_maker": bm[k]}


def _q(x, qs=(10, 50, 90, 99)):
    a = np.asarray(x, dtype=float)
    if a.size == 0:
        return {f"p{q}": 0.0 for q in qs} | {"n": 0}
    return {f"p{q}": float(np.percentile(a, q)) for q in qs} | {"n": int(a.size)}


def main() -> dict:
    files = complete_hours(AF_DATA)
    print(f"joining {len(files)} complete book hours to the trade tape")

    rows, uncond, race, placebo = [], [], [], []
    used = 0
    for i, bf in enumerate(files, 1):
        try:
            book, trades = _load_hour(bf)
            if book is None or trades is None:
                continue
            rows.extend(classify_hour(book, trades))
            r = ratio_sample(book, trades)
            uncond.extend(r["uncond"]); race.extend(r["race"]); placebo.extend(r["placebo"])
            used += 1
        except Exception as exc:
            print(f"  skipped {bf.parent.name}/{bf.stem}: {type(exc).__name__}")
        if i % 40 == 0:
            print(f"  {i}/{len(files)} hours, {len(rows)} episodes", flush=True)

    cons = np.array([r["consumption"] for r in rows])
    rec_all = [r["recovery_s"] for r in rows if r["recovery_s"] is not None]
    rec_con = [r["recovery_s"] for r in rows
               if r["consumption"] and r["recovery_s"] is not None]
    rec_can = [r["recovery_s"] for r in rows
               if not r["consumption"] and r["recovery_s"] is not None]

    out = {
        "hours_used": used,
        "n_episodes": len(rows),
        "consumption_fraction": float(cons.mean()) if len(cons) else 0.0,
        "recovery_s_all": _q(rec_all, (25, 50, 75, 90)),
        "recovery_s_consumption": _q(rec_con, (25, 50, 75, 90)),
        "recovery_s_cancellation": _q(rec_can, (25, 50, 75, 90)),
        "taker_size_over_top_depth": {
            "unconditional": _q(uncond),
            "race_proxy": _q(race),
            "placebo_no_move": _q(placebo),
            "race_window_s": RACE_WINDOW_S,
            "note": ("100 ms book / 1 ms trade resolution against a microsecond "
                     "phenomenon: race_proxy is a diluted bound, not a measurement; "
                     "placebo_no_move quantifies the dilution"),
        },
        "sweep_defragmentation": "consecutive agg_trade_ids sharing timestamp+side regrouped",
        "consumption_coverage_threshold": CONSUMPTION_COVERAGE,
    }
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "trade_book_join.json").write_text(json.dumps(out, indent=2, sort_keys=True))

    print(f"\nepisodes                {out['n_episodes']:,} over {used} hours")
    print(f"consumption-driven      {100*out['consumption_fraction']:.1f}%")
    for k, lab in (("recovery_s_all", "all"), ("recovery_s_consumption", "consumption"),
                   ("recovery_s_cancellation", "cancellation")):
        q = out[k]
        print(f"  recovery {lab:<12} p25 {q['p25']:.2f}s  median {q['p50']:.2f}s  "
              f"p75 {q['p75']:.2f}s  p90 {q['p90']:.2f}s  (n={q['n']:,})")
    r = out["taker_size_over_top_depth"]
    for k, lab in (("unconditional", "unconditional"), ("race_proxy", "race proxy"),
                   ("placebo_no_move", "placebo")):
        q = r[k]
        print(f"  ratio {lab:<14} p50 {q['p50']:.5f}  p90 {q['p90']:.5f}  "
              f"p99 {q['p99']:.5f}  (n={q['n']:,})")
    print("\nwrote results/calibration/trade_book_join.json")
    return out


if __name__ == "__main__":
    main()
