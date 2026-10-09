#!/usr/bin/env python3
"""Arrival, size and volatility at 10 ms beside 100 ms and 1 s — and what the
100 ms tick hides.

The paper's model runs on a 100 ms tick (targets.TAU_S), so everything that
happens inside one tick — several trades, a price move and the trades that
follow it — is collapsed into one simultaneous step. This module re-estimates
the calibration moments at 10 ms from the trade tape and measures how much
the 100 ms grid throws away.

WHAT THE DATA CAN RESOLVE (measured by `resolution_audit`, not assumed):

- Trade `exchange_ts_ns` is Binance's `T`, a MILLISECOND stamp stored in ns
  units: every value is a multiple of 1e6. Nothing in the trade tape is
  finer than 1 ms.
- `local_ts_ns` is when the collector's REST poller received the record:
  whole batches share one stamp and the lag runs from ~0.1 s to ~30 s. It
  carries no timing information about the market and is never used.
- `agg_trade_id` is the matching engine's sequence: no duplicates and no id
  gaps inside any file of the sample, so within an hourly file the tape is
  hole-free and the ORDER of records inside one millisecond is exact.
- On USD-M futures an aggTrade is NOT one taker order. Binance aggregates
  "market trades that fill in 100ms with the same price and the same taking
  side" (fapi/v1/aggTrades docs); on the tape no two consecutive same-side,
  same-price records are ever less than 101 ms apart. A record's T is its
  FIRST fill; its quantity may include later fills, by other takers, up to
  100 ms afterwards. So: price CHANGES are stamped to the millisecond (a new
  price opens a new record), but volume inside a same-price run is blurred by
  up to 100 ms, and record counts are not taker-order counts at any window.

The price used at 10 ms is the trade-implied mid: a buy lifted the ask, so
mid = price - tick/2; a sell hit the bid, so mid = price + tick/2. With the
spread at one tick almost always, this removes bid-ask bounce exactly; at
book-snapshot times it sits on the book mid at the median and within 3 ticks
at p99. Every moment is computed per hourly file (hole-free) and pooled, so
no return or bucket straddles a collector gap.

Run: bcs_research/.venv/bin/python bcs_research/calibration/fine_grain.py
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

from targets import AF_DATA, EXCHANGE_TICK, OUT, _read_parquet  # noqa: E402

INTERVALS_MS = (10, 100, 1000)
TICK_MS = 100                  # the paper's model tick (targets.TAU_S)
COUNT_CAP = 1000               # per-bucket record counts are histogrammed up to this
# The paper's sample, so every v2 number compares 1:1 with §3.6.
SAMPLE_DAYS = json.loads((OUT / "targets_btcusdt.json").read_text())["days"]


# ---------------------------------------------------------------- loaders

def load_trades(path: Path) -> dict | None:
    """One hourly trade file in matching-engine order, or None if unreadable.

    Sorted (and de-duplicated, as the collector notes require) by
    agg_trade_id, which is the exchange's own sequence.
    """
    t = _read_parquet(path, ["exchange_ts_ns", "local_ts_ns", "agg_trade_id",
                             "price", "size", "is_buyer_maker"])
    if t is None or t.num_rows < 2:
        return None
    ids, k = np.unique(t["agg_trade_id"].to_numpy(), return_index=True)
    ts_ns = t["exchange_ts_ns"].to_numpy()[k]
    return {
        "ts_ms": ts_ns // 1_000_000,
        "price": np.asarray(t["price"].to_numpy(), float)[k],
        "size": np.asarray(t["size"].to_numpy(), float)[k],
        "buy": ~t["is_buyer_maker"].to_numpy(zero_copy_only=False)[k],
        "sub_ms_stamps": int(np.count_nonzero(ts_ns % 1_000_000)),
        "id_gaps": int(np.count_nonzero(np.diff(ids) != 1)),
        "local_lag_ms": (t["local_ts_ns"].to_numpy()[k] - ts_ns) / 1e6,
    }


def load_book(day: str, hour: str) -> dict | None:
    """Top of book for one hour, time-sorted, or None if absent/unreadable."""
    path = AF_DATA / "book_snapshots" / day / f"{hour}.parquet"
    if not path.exists():
        return None
    t = _read_parquet(path, ["exchange_ts_ns", "bid_px_1", "ask_px_1",
                             "bid_sz_1", "ask_sz_1"])
    if t is None or t.num_rows < 2:
        return None
    ts = t["exchange_ts_ns"].to_numpy() // 1_000_000
    o = np.argsort(ts, kind="stable")
    return {"ts_ms": ts[o]} | {
        k: np.asarray(t[c].to_numpy(), float)[o]
        for k, c in (("bid_px", "bid_px_1"), ("ask_px", "ask_px_1"),
                     ("bid_sz", "bid_sz_1"), ("ask_sz", "ask_sz_1"))}


def trade_hours(days=SAMPLE_DAYS, skipped: list | None = None):
    """Yield (day, hour, trades) for every readable hourly trade file."""
    for d in days:
        for f in sorted((AF_DATA / "trades" / d).glob("*.parquet")):
            tr = load_trades(f)
            if tr is None:
                if skipped is not None:
                    skipped.append(f"trades/{d}/{f.name}")
                continue
            yield d, f.stem, tr


# ---------------------------------------------------------------- moments

def implied_mid(price: np.ndarray, buy: np.ndarray) -> np.ndarray:
    """Mid implied by each trade: a buy paid the ask, a sell hit the bid."""
    return price - np.where(buy, EXCHANGE_TICK / 2, -EXCHANGE_TICK / 2)


def on_grid(ts_ms: np.ndarray, values: np.ndarray, dt_ms: int) -> np.ndarray:
    """Previous-tick sample of `values` every dt_ms across [ts[0], ts[-1]]."""
    g = np.arange(ts_ms[0], ts_ms[-1] + 1, dt_ms)
    return values[np.searchsorted(ts_ms, g, side="right") - 1]


def moved(mid: np.ndarray) -> np.ndarray:
    """True where a record's implied mid differs from the previous record's."""
    out = np.zeros(len(mid), dtype=bool)
    out[1:] = np.abs(np.diff(mid)) > EXCHANGE_TICK / 2
    return out


def tick_losses(ts_ms: np.ndarray, size: np.ndarray, mid: np.ndarray,
                dt_ms: int) -> dict:
    """Raw sums for what a dt_ms tick collapses, for one hole-free file.

    - records/volume in buckets that hold >= 2 records (their order is lost);
    - records/volume in buckets that contain a price move (trades that react
      to the move happen "simultaneously" with it on the grid);
    - path length: |record-to-record mid changes| against |net change per
      bucket|; the difference is price variation invisible on the grid.
    """
    b = (ts_ms - ts_ms[0]) // dt_ms
    counts = np.bincount(b)
    vol = np.bincount(b, weights=size)
    mv = moved(mid)
    move_bucket = np.bincount(b, weights=mv) > 0
    multi = counts >= 2
    step = np.abs(np.diff(mid))
    # net change per occupied bucket: its last mid against the previous
    # bucket's last (the file's first record anchors the first bucket)
    last = np.flatnonzero(np.r_[b[1:] != b[:-1], True])
    net = np.abs(np.diff(mid[np.r_[0, last]]))
    return {
        "n": int(counts.sum()), "vol": float(vol.sum()),
        "n_multi": int(counts[multi].sum()), "vol_multi": float(vol[multi].sum()),
        "n_move": int(counts[move_bucket].sum()), "vol_move": float(vol[move_bucket].sum()),
        "path": float(step.sum()), "net": float(net.sum()),
    }


class IntervalAccumulator:
    """Pools per-file bucket and return statistics at one interval."""

    def __init__(self, dt_ms: int):
        self.dt_ms = dt_ms
        self.count_hist = np.zeros(COUNT_CAP + 1, dtype=np.int64)
        self.bucket_vol: list[np.ndarray] = []
        self.r2 = self.r4 = 0.0
        self.n_ret = self.n_zero = 0
        self.dur_s = 0.0
        self.loss = {}

    def add(self, ts_ms, size, mid):
        b = (ts_ms - ts_ms[0]) // self.dt_ms
        counts = np.bincount(b)
        self.count_hist += np.bincount(np.minimum(counts, COUNT_CAP),
                                       minlength=COUNT_CAP + 1)
        vol = np.bincount(b, weights=size)
        self.bucket_vol.append(vol[counts > 0].astype(np.float32))
        r = np.diff(np.log(on_grid(ts_ms, mid, self.dt_ms))) * 1e4
        self.r2 += float(r @ r)
        self.r4 += float(np.sum(r ** 4))
        self.n_ret += r.size
        self.n_zero += int(np.count_nonzero(r == 0))
        self.dur_s += r.size * self.dt_ms / 1e3
        for k, v in tick_losses(ts_ms, size, mid, self.dt_ms).items():
            self.loss[k] = self.loss.get(k, 0) + v

    def summary(self) -> dict:
        k = np.arange(COUNT_CAP + 1)
        n_b = self.count_hist.sum()
        mean = float((k * self.count_hist).sum() / n_b)
        var = float((k * k * self.count_hist).sum() / n_b - mean ** 2)
        vol = np.concatenate(self.bucket_vol)
        var_r = self.r2 / self.n_ret
        L = self.loss
        return {
            "buckets": int(n_b),
            "records_per_bucket_mean": mean,
            "records_per_bucket_fano": var / mean,
            "p_empty": float(self.count_hist[0] / n_b),
            "p_empty_if_poisson": float(np.exp(-mean)),
            "p_two_plus": float(self.count_hist[2:].sum() / n_b),
            "p_two_plus_if_poisson": float(1 - np.exp(-mean) * (1 + mean)),
            "bucket_volume_btc_nonempty": {
                f"p{q}": float(np.percentile(vol, q)) for q in (50, 90, 99)},
            "sigma_bp_per_sqrt_s": float(np.sqrt(self.r2 / self.dur_s)),
            "zero_return_share": self.n_zero / self.n_ret,
            "return_kurtosis": (float(self.r4 / self.n_ret / var_r ** 2)
                                if var_r > 0 else float("nan")),
            "share_records_in_multi_record_buckets": L["n_multi"] / L["n"],
            "share_volume_in_multi_record_buckets": L["vol_multi"] / L["vol"],
            "share_records_in_buckets_with_a_price_move": L["n_move"] / L["n"],
            "share_volume_in_buckets_with_a_price_move": L["vol_move"] / L["vol"],
            "share_of_mid_path_invisible_on_grid": (1 - L["net"] / L["path"]
                                                    if L["path"] > 0 else 0.0),
        }


def resolution_audit(files: list[dict], same_price_gaps_ms: np.ndarray) -> dict:
    lag = np.concatenate([f["local_lag_ms"] for f in files])
    return {
        "trade_stamps_finer_than_1ms": int(sum(f["sub_ms_stamps"] for f in files)),
        "agg_trade_id_gaps_within_files": int(sum(f["id_gaps"] for f in files)),
        "local_minus_exchange_ms": {f"p{q}": float(np.percentile(lag, q))
                                    for q in (1, 50, 90, 99)},
        "min_gap_ms_between_consecutive_same_side_same_price_records":
            int(same_price_gaps_ms.min()),
        "aggregation_rule": ("USD-M futures aggTrade merges fills within 100 ms "
                             "at the same price and taking side, across taker orders"),
    }


def same_price_gaps(tr: dict) -> np.ndarray:
    """Time between consecutive records that share side and price."""
    same = (tr["buy"][1:] == tr["buy"][:-1]) & (tr["price"][1:] == tr["price"][:-1])
    return np.diff(tr["ts_ms"])[same]


def main() -> dict:
    skipped: list[str] = []
    acc = {dt: IntervalAccumulator(dt) for dt in INTERVALS_MS}
    audit_files, gaps, sizes = [], [], []
    n_records, dur_s = 0, 0.0
    for _, _, tr in trade_hours(skipped=skipped):
        mid = implied_mid(tr["price"], tr["buy"])
        for a in acc.values():
            a.add(tr["ts_ms"], tr["size"], mid)
        audit_files.append({k: tr[k] for k in ("sub_ms_stamps", "id_gaps")}
                           | {"local_lag_ms": tr["local_lag_ms"][::50]})
        gaps.append(same_price_gaps(tr))
        sizes.append(tr["size"].astype(np.float32))
        n_records += tr["size"].size
        dur_s += (tr["ts_ms"][-1] - tr["ts_ms"][0]) / 1e3

    size = np.concatenate(sizes)
    out = {
        "sample": {"days": SAMPLE_DAYS, "n_days": len(SAMPLE_DAYS),
                   "hour_files": len(audit_files), "skipped_files": skipped,
                   "n_records": n_records, "duration_s": dur_s,
                   "record_rate_per_s": n_records / dur_s},
        "resolution": resolution_audit(audit_files, np.concatenate(gaps)),
        "record_size_btc": {f"p{q}": float(np.percentile(size, q))
                            for q in (50, 90, 99)} | {"mean": float(size.mean())},
        "by_interval_ms": {str(dt): a.summary() for dt, a in acc.items()},
        "notes": [
            "Counts are aggTrade RECORDS, not taker orders: on USD-M futures one "
            "record merges all same-price same-side fills within 100 ms.",
            "sigma uses the trade-implied mid sampled previous-tick on the grid; "
            "returns never cross an hourly file (each file is hole-free).",
            "The 100 ms row is what the tick model's grid sees; the 10 ms row "
            "shows what it collapses.",
        ],
    }
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "fine_grain_v2.json").write_text(json.dumps(out, indent=2, sort_keys=True))
    print(json.dumps({k: out[k] for k in ("resolution", "record_size_btc")}, indent=2))
    for dt, s in out["by_interval_ms"].items():
        print(f"\n{dt} ms")
        for k, v in s.items():
            print(f"  {k:<46} {v}")
    print("\nwrote results/calibration/fine_grain_v2.json")
    return out


if __name__ == "__main__":
    main()
