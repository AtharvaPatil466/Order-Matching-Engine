#!/usr/bin/env python3
"""A BCS-style race detector on the BTC tape, and what it can and cannot see.

Replaces the paper's single snipe-to-depth point estimate (§3.7, built on a
100 ms book trigger) with a distribution, using the trade tape's own price
changes as the trigger.

WHAT IS REAL (see fine_grain.py's audit): trade stamps are whole
milliseconds, the record ORDER within a millisecond is exact (agg_trade_id),
and every price change is stamped to the millisecond. What is NOT there:
any sub-millisecond time, and any count of takers — Binance USD-M futures
aggregates all fills at one price and taking side within 100 ms into a single
record, across taker orders. A BCS race in which several snipers hit the same
stale level therefore prints as ONE record; and one market order sweeping
three levels prints exactly like three snipers taking one level each.
Participants, winners and losers are unobservable; Aquilina, Budish and
O'Neill (2022) could count them only because regulator message data includes
the FAILED orders.

So the detector measures the visible footprint of a race — an aggressive,
same-side burst immediately after a price move:

  trigger   a record whose implied mid moved in its OWN direction (a buy that
            lifted the price, a sell that pushed it down): the visible start
            of a sweep of the stale side;
  race      every same-side record from the START of the trigger's
            millisecond to W ms after it, when that is >= 2 records;
            overlapping clusters merge into one race. Starting at the ms,
            not at the trigger, matters: a sweep's level-1 fill leaves the
            implied mid unchanged (it trades at the old ask), so the move
            first shows on level 2 — yet the level-1 fill IS the stale quote
            being taken;
  placebo   the same cluster test started from a record that did NOT move
            the mid, as a control for ordinary clustering of order flow.

W runs from 0 (the same millisecond, the finest the stamps allow; ABO's
information horizon averages ~200 us) to 100 ms (the aggregation window).
Depth is the hit side's top-of-book size from the last book snapshot strictly
before the trigger, kept only when at most DEPTH_MAX_AGE_MS old; book
coverage is far patchier than trade coverage, so the ratio uses fewer races.

Run: bcs_research/.venv/bin/python bcs_research/calibration/race_proxy.py
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

from fine_grain import (OUT, SAMPLE_DAYS, implied_mid, load_book,  # noqa: E402
                        trade_hours)
from targets import EXCHANGE_TICK  # noqa: E402

WINDOWS_MS = (0, 1, 2, 5, 10, 20, 50, 100)
MIN_RECORDS = 2
DEPTH_MAX_AGE_MS = 200         # two book cadences

# Aquilina, Budish & O'Neill (2022), QJE 137(1):493-564, verified against the
# published PDF. LSE message data, 43 trading days Aug-Oct 2015, FTSE 350.
ABO_2022 = {
    "race_definition": ("2+ participants, 1+ success and 1+ fail (failed IOC or "
                        "cancel), same symbol, price and side, within the "
                        "information horizon (~200 us average, capped at 500 us)"),
    "ftse100_races_per_symbol_day": 537.24,
    "ftse100_races_per_symbol_second_8p5h_day": 537.24 / (8.5 * 3600),
    "ftse100_share_of_volume_in_races": 0.22,
    "full_sample_share_of_volume_in_races": 0.2146,
    "race_duration_us": {"mean": 79, "median": 46, "mode": "5-10", "p90": "~200"},
    "participants_per_race_at_500us": 3.3,
    "latency_arbitrage_tax_bp_all_volume": 0.42,
    "sensitivity_2plus_within_1ms": {"races_per_day": 768.02, "share_of_volume": 0.4003},
    "sensitivity_2plus_within_3ms": {"races_per_day": 799.91, "share_of_volume": 0.4372},
}

COMPARABILITY = {
    "comparable": [
        "same instrument and side, a short window after a price change",
        "share of volume in races: aggressive volume in races over all volume",
    ],
    "not_comparable": [
        "ABO count participants including FAILED messages; the tape shows only "
        "fills, and one record can hold many takers (100 ms same-price "
        "aggregation), so a multi-sniper race at one level is a single record",
        "a single taker sweeping several levels looks exactly like a race here, "
        "so these counts are an upper bound on races and give no participant count",
        "finest window here is one 1 ms stamp (W=0), ~5x ABO's average horizon; "
        "W>=1 ms compares to ABO's 1-3 ms sensitivity rows, not their baseline",
        "a 24/7 crypto perp against FTSE equities in 8.5 h sessions: compare "
        "per-second rates, not per-day counts",
        "no latency-arbitrage tax: without failed messages there is no "
        "winner/loser split and no profit-per-race measure",
    ],
}


def triggers(mid: np.ndarray, buy: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """(move, placebo) masks: a record that pushed the mid its own way, or left it unchanged."""
    dm = np.r_[np.nan, np.diff(mid)]
    half = EXCHANGE_TICK / 2
    move = (buy & (dm > half)) | (~buy & (dm < -half))
    placebo = np.abs(dm) <= half            # NaN for the first record compares False
    return move, placebo


def cluster_end(ts_ms: np.ndarray, window_ms: int) -> np.ndarray:
    """For ONE side's records in sequence order: one past the last record within W of each."""
    return np.searchsorted(ts_ms, ts_ms + window_ms, side="right")


def _at(positions: np.ndarray, n: int) -> np.ndarray:
    mask = np.zeros(n, dtype=bool)
    mask[positions] = True
    return mask


def merge_clusters(starts: np.ndarray, end: np.ndarray) -> np.ndarray:
    """Race id per record (0 = none) for clusters [s, end[s]) from each start.

    Overlapping clusters merge into one race; clusters that merely touch stay
    separate, so W=0 never joins two different milliseconds.
    """
    s = np.flatnonzero(starts)
    cover = np.zeros(end.size + 1, dtype=np.int64)
    np.add.at(cover, s, 1)
    np.add.at(cover, end[s], -1)
    depth = np.cumsum(cover[:-1])
    new = np.zeros(end.size, dtype=bool)
    new[s] = depth[s] == 1                  # no earlier cluster covers this start
    return np.where(depth > 0, np.cumsum(new), 0)


def book_at(book: dict | None, t_ms: np.ndarray, first_px: np.ndarray,
            buy_side: bool) -> dict:
    """Hit-side book each race met, from the last snapshot strictly before it.

    `top` is the level-1 size (the paper's denominator); `at_first_px` the
    displayed size at the race's first price and `level` its ladder index.
    Both matter because the top moves inside the 100 ms cadence: races often
    start above a stale snapshot's best ask, where the ladder holds only dust.
    NaN / -1 where the snapshot is older than DEPTH_MAX_AGE_MS, absent, or
    the price is off its 20-level ladder.
    """
    n = t_ms.size
    if book is None:
        return {"top": np.full(n, np.nan), "at_first_px": np.full(n, np.nan),
                "level": np.full(n, -1), "age_ms": np.full(n, np.nan)}
    j = np.searchsorted(book["ts_ms"], t_ms, side="left") - 1
    jj = np.maximum(j, 0)
    px = (book["ask_px"] if buy_side else book["bid_px"])[jj]
    sz = (book["ask_sz"] if buy_side else book["bid_sz"])[jj]
    age = (t_ms - book["ts_ms"][jj]).astype(float)
    fresh = (j >= 0) & (age <= DEPTH_MAX_AGE_MS)
    hit = np.isclose(px, first_px[:, None], rtol=0, atol=EXCHANGE_TICK / 4)
    on_ladder = fresh & hit.any(axis=1)
    pos = lambda x: np.where(x > 0, x, np.nan)          # noqa: E731
    return {"top": np.where(fresh, pos(sz[:, 0]), np.nan),
            "at_first_px": np.where(on_ladder, pos((sz * hit).sum(axis=1)), np.nan),
            "level": np.where(on_ladder, hit.argmax(axis=1), -1),
            "age_ms": np.where(fresh, age, np.nan)}


class WindowAccumulator:
    def __init__(self, window_ms: int):
        self.w = window_ms
        self.n_races = 0
        self.qty: list[np.ndarray] = []
        self.ratio_top: list[np.ndarray] = []
        self.ratio_hit: list[np.ndarray] = []
        self.age: list[np.ndarray] = []
        self.records: list[np.ndarray] = []
        self.duration_ms: list[np.ndarray] = []
        self.levels: list[np.ndarray] = []
        self.level: list[np.ndarray] = []
        self.vol_in = self.vol_all = 0.0
        self.rec_in = self.rec_all = 0
        self.n_lb, self.vol_lb = 0, 0.0
        self.move_n = self.move_hit = self.placebo_n = self.placebo_hit = 0

    def add(self, tr: dict, mid: np.ndarray, book: dict | None) -> None:
        move, placebo = triggers(mid, tr["buy"])
        self.vol_all += float(tr["size"].sum())
        self.rec_all += tr["size"].size
        for side in (True, False):
            idx = np.flatnonzero(tr["buy"] == side)
            if idx.size == 0:
                continue
            ts, sz, px = tr["ts_ms"][idx], tr["size"][idx], tr["price"][idx]
            lo = np.searchsorted(ts, ts, side="left")     # start of each record's ms
            end = cluster_end(ts, self.w)
            big = (end - lo) >= MIN_RECORDS
            trig = move[idx] & big
            rid = merge_clusters(_at(lo[trig], ts.size), end)
            # One taker order's fills share one stamp, so a cluster spanning
            # >= 2 stamps certainly holds >= 2 takers: the LOWER bound.
            certain = trig & (ts[end - 1] > ts)
            rid_lb = merge_clusters(_at(lo[certain], ts.size), end)
            self.n_lb += int(rid_lb.max(initial=0))
            self.vol_lb += float(sz[rid_lb > 0].sum())
            self.move_n += int(move[idx].sum())
            self.move_hit += int(trig.sum())
            self.placebo_n += int(placebo[idx].sum())
            self.placebo_hit += int((placebo[idx] & big).sum())
            self.vol_in += float(sz[rid > 0].sum())
            self.rec_in += int((rid > 0).sum())
            if not rid.any():
                continue
            a = np.flatnonzero((rid > 0) & (np.r_[0, rid[:-1]] != rid))
            b = a + np.bincount(rid)[1:]
            cum_sz = np.r_[0.0, np.cumsum(sz)]
            cum_chg = np.r_[0, 0, np.cumsum(px[1:] != px[:-1])]   # changes up to each index
            q = cum_sz[b] - cum_sz[a]
            self.n_races += a.size
            self.qty.append(q)
            self.records.append(b - a)
            self.duration_ms.append(ts[b - 1] - ts[a])
            self.levels.append(1 + cum_chg[b] - cum_chg[a + 1])
            bk = book_at(book, ts[a], px[a], side)
            self.ratio_top.append(q / bk["top"])
            self.ratio_hit.append(q / bk["at_first_px"])
            self.level.append(bk["level"])
            self.age.append(bk["age_ms"])

    def summary(self, covered_s: float) -> dict:
        def q(x, qs=(10, 25, 50, 75, 90, 99)):
            x = x[np.isfinite(x)]
            return ({f"p{p}": float(np.percentile(x, p)) for p in qs} | {"n": int(x.size)}
                    if x.size else {"n": 0})
        cat = (lambda xs: np.concatenate(xs).astype(float) if xs else np.array([]))
        top, age, level = cat(self.ratio_top), cat(self.age), cat(self.level)
        has = np.isfinite(top)
        qty = cat(self.qty)
        return {
            "window_ms": self.w,
            "n_races": self.n_races,
            "races_per_s": self.n_races / covered_s,
            "races_per_hour": self.n_races / covered_s * 3600,
            "share_of_volume_in_races": self.vol_in / self.vol_all,
            "share_of_records_in_races": self.rec_in / self.rec_all,
            # n_races is the upper bound (a lone sweep counts); the lower bound
            # keeps clusters spanning >= 2 stamps, so it misses same-ms races,
            # ABO's typical case, and is 0 at W=0 by construction.
            "lower_bound_races_per_s": self.n_lb / covered_s,
            "lower_bound_share_of_volume": self.vol_lb / self.vol_all,
            "p_race_after_move_trigger": self.move_hit / max(self.move_n, 1),
            "p_cluster_after_placebo_trigger": self.placebo_hit / max(self.placebo_n, 1),
            "race_qty_btc": q(qty),
            "share_of_races_with_book": float(has.mean()) if has.size else 0.0,
            "race_qty_over_top_depth": q(top),
            "race_share_exceeding_top_depth": float(np.mean(top[has] > 1)) if has.any() else None,
            "race_qty_over_top_depth_snapshot_le_20ms": q(top[age <= 20]),
            "race_qty_over_depth_at_first_price": q(cat(self.ratio_hit)),
            "share_starting_at_snapshot_top": float(np.mean(level[has] == 0)) if has.any() else None,
            "share_starting_at_snapshot_top_le_20ms": (
                float(np.mean(level[age <= 20] == 0)) if (age <= 20).any() else None),
            "records_per_race": q(cat(self.records), (50, 90, 99)),
            "price_levels_per_race": q(cat(self.levels), (50, 90, 99)),
            "race_duration_ms": q(cat(self.duration_ms), (50, 90, 99)),
        }


def main() -> dict:
    acc = {w: WindowAccumulator(w) for w in WINDOWS_MS}
    skipped: list[str] = []
    covered_s = 0.0
    hours_with_book = 0
    for day, hour, tr in trade_hours(skipped=skipped):
        book = load_book(day, hour, levels=20)
        hours_with_book += book is not None
        mid = implied_mid(tr["price"], tr["buy"])
        for a in acc.values():
            a.add(tr, mid, book)
        covered_s += (tr["ts_ms"][-1] - tr["ts_ms"][0]) / 1e3

    by = {str(w): a.summary(covered_s) for w, a in acc.items()}
    out = {
        "sample": {"days": SAMPLE_DAYS, "covered_s": covered_s,
                   "hours_with_book": hours_with_book, "skipped_files": skipped},
        "definition": {"trigger": "record that moved the implied mid in its own direction",
                       "race": (f">= {MIN_RECORDS} same-side records from the start of the "
                                "trigger's ms to W ms after it"),
                       "depth": (f"hit-side book from the last snapshot <= {DEPTH_MAX_AGE_MS} ms "
                                 "before the race: level-1 size, and size at the race's "
                                 "first price on the 20-level ladder")},
        "by_window_ms": by,
        "abo_2022": ABO_2022,
        "comparability": COMPARABILITY,
        "paper_section_3_7_point_estimate": {
            "race_proxy_median_taker_over_depth": 0.0283,
            "note": "100 ms book trigger, 200 ms window, records regrouped per ms+side",
        },
    }
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "race_proxy_v2.json").write_text(json.dumps(out, indent=2, sort_keys=True))
    hdr = f"{'W ms':>5} {'races/s':>9} {'vol%':>6} {'lb/s':>7} {'lb vol%':>7} {'P(r|mv)':>8} {'P(c|pl)':>8} {'qty p50':>8} {'q/top p50':>11} {'p90':>8} {'>depth':>7}"
    print(hdr)
    for w, s in by.items():
        r = s["race_qty_over_top_depth"]
        print(f"{w:>5} {s['races_per_s']:>9.4f} {100*s['share_of_volume_in_races']:>6.1f} "
              f"{s['lower_bound_races_per_s']:>7.4f} {100*s['lower_bound_share_of_volume']:>7.1f} "
              f"{s['p_race_after_move_trigger']:>8.3f} {s['p_cluster_after_placebo_trigger']:>8.3f} "
              f"{s['race_qty_btc'].get('p50', 0):>8.3f} {r.get('p50', float('nan')):>11.4f} "
              f"{r.get('p90', float('nan')):>8.3f} {s['race_share_exceeding_top_depth'] or 0:>7.3f}")
    print(f"\nABO FTSE 100: {ABO_2022['ftse100_races_per_symbol_second_8p5h_day']:.4f} races/s/symbol, "
          f"{100*ABO_2022['ftse100_share_of_volume_in_races']:.0f}% of volume")
    print("wrote results/calibration/race_proxy_v2.json")
    return out


if __name__ == "__main__":
    main()
