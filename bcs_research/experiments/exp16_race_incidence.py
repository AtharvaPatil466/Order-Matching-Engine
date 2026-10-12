#!/usr/bin/env python3
"""Experiment 16 — cancel-vs-snipe race incidence on the event clock (PREREGISTRATION P2).

Why this exists. The tick model put the maker's reaction and the snipers' in
the same 100 ms tick, so "who eats a jump" was a jitter draw inside a fixed
staleness window. On a nanosecond clock the maker's cancel and the snipers'
IOCs race to the engine, and P2 says the maker's sniping loss is then set by
its race-win probability. Four predictions, each implemented as §6 states it,
at the signed-off operating point (operating_point.py: 100 us snipers, race
jitter eps = 80 us on every racing firm, VIP 9 fees, 30-minute runs, n = 100):

  H2a (primary)  The share of snipe-able jumps on which the maker's cancel
                 reaches the engine before every sniper IOC equals
                 race_win_probability(lag, eps, k) within +-0.02, for
                 k in {1, 3, 8, 21} x lag/eps in {0, 0.25, 0.5, 1.5}. Wilson
                 intervals. A jump is snipe-able when the snipers fired at it
                 (their threshold is the taker fee, so a jump below it races
                 nothing); read off the scheduler's race log.
  H2b            Fixed-spread maker that cancels on every signal, snipe size =
                 quote size: total sniper rent at lag 0 grows as k/(k+1),
                 rent(k)/rent(21) = 0.52 at k = 1 and 0.79 at k = 3, each within
                 +-10%; at lag > eps every rent(k)/rent(21) is in [0.9, 1.1]
                 (TOST). Miss: the k = 1 lag-0 ratio above 0.65.
  H2c            Liquidity gaps (§3.3's detector, unchanged) do not grow
                 linearly in k: gaps(21)/gaps(3) < 2 at both lags. Miss: >= 3.
  H2d            The paper's adverse-selection maker on the event clock
                 (_confirm_common.paper_policy): tape-band maker-gains cells
                 <= 2 of 18 at every maker latency <= 1 ms, a count
                 non-decreasing over {0.01, 0.1, 1, 10, 100, 300} ms, >= 12 of
                 18 at 300 ms. Miss: >= 6 at any latency <= 1 ms, or < 6 at
                 300 ms. Verdict on the paper band; the corrected tape band
                 (§4) is reported beside it under the same rule.

One market serves H2a-H2c: the operating point's default maker (fixed
BCS half-spread, cancel-and-requote on every observed update), snipe size
equal to the 6,180-lot quote, maker latency = sniper latency + lag. Its runs
at lag 0 and lag 1.5 eps are H2b's and H2c's. build_market takes one-way
latencies, so the lag is split evenly over the maker's two legs; only the
total reaches the race. Rent is the snipers' P&L net of fees, marked at V at
execution ("HFT PnL", P2's metric); its k-ratio is the same gross or net.

Gaps. The §3.3 detector (metrics/liquidity_gap_detector.py) on 1-ms book
snapshots with its own constants (k_fund 3, max gap 50 ms), as the event pilot
of H2c (gap_channel.py) read it; sigma per step is the path's diffusive sigma
over 1 ms. 1.8 M snapshot dicts per run would not fit eight workers, so
`grid_gaps` runs the same detector on the grid implied by the exact book log
(tests pin it to detect_liquidity_gaps). Exact one-sided episodes and time,
the clock-free measures, are reported beside it, not judged.

Run (from bcs_research/):
    python experiments/exp16_race_incidence.py --confirmatory
    python experiments/exp16_race_incidence.py --seeds 2 --out /tmp/exp16.json   # smoke
"""
from __future__ import annotations

import math
import sys
import time
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import _confirm_common as cc  # noqa: E402
import operating_point as op  # noqa: E402

from bcs_theory import race_win_probability  # noqa: E402
from gap_channel import TracedEventScheduler, one_sided_episodes  # noqa: E402

NAME = "exp16_race_incidence"
K_RACE = (1, 3, 8, 21)
LAG_FRACS = (0.0, 0.25, 0.5, 1.5)          # lag / eps
SLOW_LAG = 1.5                             # H2b/H2c's "lag > eps"
RACE_RATIO = 1.0                           # snipe size = quote size (H2b)
SHARE_BAND = 0.02                          # H2a, absolute
RENT_BAND = 0.10                           # H2b, relative to (k/(k+1))/(21/22)
RENT_JUDGED_K = (1, 3)                     # the two values H2b states
RENT_K1_MISS = 0.65
FLAT = (0.9, 1.1)                          # H2b at lag > eps, TOST
GAP_HIT, GAP_MISS = 2.0, 3.0               # H2c
H2D_LATENCIES_MS = (0.01, 0.1, 1, 10, 100, 300)
FAST_MS, FAST_HIT_MAX, FAST_MISS_MIN = 1, 2, 6
SLOW_MS, SLOW_HIT_MIN, SLOW_MISS_BELOW = 300, 12, 6
GAP_DT_NS = 1_000_000                      # §3.3's snapshot step (the tick model's dt_us)
GAP_MAX_NS = 50_000_000                    # liquidity_gap_detector's max_gap_us
K_FUND = 3.0
SIGMA_PER_GAP_STEP = (op.PATH["sigma_bp_per_sqrt_s"] * op.BP * math.sqrt(GAP_DT_NS / 1e9))


# --- the gap metric ------------------------------------------------------------

def grid_gaps(states, end_ns: int, dt_ns: int, value_at, sigma_per_step: float,
              k_fund: float = K_FUND, max_gap_ns: int = GAP_MAX_NS) -> int:
    """§3.3's detector on snapshots every dt_ns, from the exact book log.

    states: (t, bid, ask) after each book event, ascending. A snapshot at s shows
    the book after every event at or before s (EventScheduler). Equals
    count_liquidity_gaps over the grid snapshots from the first two-sided one
    on (before it nothing had been quoted: gap_channel's convention).
    """
    first = next((t for t, b, a in states if b > 0 and a > 0), None)
    if first is None:
        return 0
    n = -(-end_ns // dt_ns)
    spans = []
    for a, b, _ in one_sided_episodes(states, end_ns):
        i0, i1 = -(-a // dt_ns), -(-b // dt_ns)        # grid points with a <= i*dt < b
        if i1 <= i0:
            continue                                   # between two snapshots: invisible
        if spans and i0 == spans[-1][1]:
            spans[-1][1] = i1                          # no two-sided snapshot between
        else:
            spans.append([i0, i1])
    if spans and spans[0][0] == -(-first // dt_ns):
        spans.pop(0)                                   # before the first two-sided snapshot
    count = 0
    for i0, i1 in spans:
        gap = i1 - i0
        if i1 >= n or gap * dt_ns > max_gap_ns:
            continue                                   # unrecovered, or a structural outage
        move = abs(value_at(min(n - 1, i1) * dt_ns) - value_at(max(0, i0 - 1) * dt_ns))
        count += move < k_fund * sigma_per_step * math.sqrt(max(1, gap + 2))
    return count


# --- one run -------------------------------------------------------------------

def race_run(k: int, lag_frac: float, seed: int, horizon_s: float = op.HORIZON_S) -> dict:
    """H2a-H2c's market: default maker lagging the snipers by lag_frac * eps."""
    lag = int(round(lag_frac * op.JITTER_NS))
    if lag % 2:
        raise ValueError(f"lag {lag} ns must split evenly over two legs")
    mk = op.build_market(seed, maker_latencies_ns=(op.MAKER_ONE_WAY_NS + lag // 2,), k=k,
                         snipe_ratio=RACE_RATIO, horizon_s=horizon_s)
    res, sched = cc.simulate(mk, seed, TracedEventScheduler)
    makers = {m.participant_id for m in mk.makers}
    snipers = {s.participant_id for s in mk.snipers}
    won = [cc.maker_won(r, makers, snipers) for r in res.races]
    one_sided = [(a, b) for a, b, ok in one_sided_episodes(sched.book_log, res.duration_ns) if ok]
    return {**cc.legs(res),
            "races": sum(w is not None for w in won), "maker_wins": sum(w is True for w in won),
            "gaps": grid_gaps(sched.book_log, res.duration_ns, GAP_DT_NS, res.path.value_at,
                              SIGMA_PER_GAP_STEP),
            "episodes_exact": len(one_sided),
            "one_sided_share_exact": sum(b - a for a, b in one_sided) / res.duration_ns}


def paper_run(lat_ms: float, k: int, ratio: float, seed: int,
              horizon_s: float = op.HORIZON_S) -> dict:
    """H2d: the paper's maker at one-way latency lat_ms; k = 0 is the baseline."""
    return cc.maker_run(seed, cc.paper_policy(), round(lat_ms * 1e6), k, ratio,
                        horizon_s=horizon_s)


def tasks(seeds, horizon_s: float = op.HORIZON_S) -> dict:
    """Every run, heaviest (largest k) first so the pool load-balances."""
    t = {}
    for k in sorted(K_RACE, reverse=True):
        for lf in LAG_FRACS:
            for s in seeds:
                t[cc.key("race", k, lf, s)] = (race_run, k, lf, s, horizon_s)
    for band, ratio, k in sorted(cc.band_cells(), key=lambda c: -c[2]):
        for lat in H2D_LATENCIES_MS:
            for s in seeds:
                t[cc.key("h2d", lat, band, ratio, k, s)] = (paper_run, lat, k, ratio, s, horizon_s)
    for lat in H2D_LATENCIES_MS:
        for s in seeds:
            t[cc.key("h2d", lat, "base", s)] = (paper_run, lat, 0, op.PAPER_BAND[0], s, horizon_s)
    return t


# --- verdicts (pure) --------------------------------------------------------------

def h2a(cells: dict) -> dict:
    """cells: {(k, lag_frac): (maker_wins, races)} pooled over seeds."""
    out = {}
    for (k, lf), (wins, n) in sorted(cells.items()):
        th = race_win_probability(lf * op.JITTER_NS, op.JITTER_NS, k)
        out[f"k={k},lag/eps={lf}"] = cc.judged(op.wilson(wins, n), th, th - SHARE_BAND,
                                               th + SHARE_BAND)
    return {"cells": out, "verdict": cc.combine(c["verdict"] for c in out.values())}


def h2b(rent: dict) -> dict:
    """rent: {(k, lag_frac): per-seed sniper rent}, seeds aligned (CRN)."""
    lag0, flat = {}, {}
    for k in (1, 3, 8):
        th = (k / (k + 1)) / (21 / 22)
        lag0[k] = cc.judged(cc.ratio_ci(rent[(k, 0.0)], rent[(21, 0.0)]), th,
                            th * (1 - RENT_BAND), th * (1 + RENT_BAND))
        r95 = cc.ratio_ci(rent[(k, SLOW_LAG)], rent[(21, SLOW_LAG)])
        r90 = cc.ratio_ci(rent[(k, SLOW_LAG)], rent[(21, SLOW_LAG)], ci=0.90)
        flat[k] = {**r95, "ci90": [r90["lower"], r90["upper"]], "margin": list(FLAT),
                   "verdict": cc.tost(r90, r95, *FLAT)}
    if lag0[1]["lower"] > RENT_K1_MISS:
        verdict = "MISS"
    elif (all(lag0[k]["verdict"] == "HIT" for k in RENT_JUDGED_K)
          and all(f["verdict"] == "HIT" for f in flat.values())):
        verdict = "HIT"
    else:
        verdict = "INCONCLUSIVE"
    return {"lag0_ratio_to_k21": lag0, "slow_ratio_to_k21": flat, "verdict": verdict,
            "judged_k_at_lag0": list(RENT_JUDGED_K)}


def gap_verdict(r: dict) -> str:
    """H2c on one ratio interval: HIT below 2, MISS at or above 3."""
    if r["upper"] < GAP_HIT:
        return "HIT"
    return "MISS" if r["lower"] >= GAP_MISS else "INCONCLUSIVE"


def h2c(gaps: dict, extra: dict | None = None) -> dict:
    """gaps: {lag_frac: (per-seed gaps at k=21, at k=3)}."""
    lags = {}
    for lf, (g21, g3) in gaps.items():
        r = cc.ratio_ci(g21, g3)
        lags[str(lf)] = {**r, "verdict": gap_verdict(r)}
    return {"by_lag": lags, "verdict": cc.combine(v["verdict"] for v in lags.values()),
            "unjudged_exact": extra or {}}


def h2d_verdict(counts: dict) -> dict:
    """counts: {latency_ms: maker-gains cells of 18}, H2d's rule."""
    lats = sorted(counts)
    fast = [counts[x] for x in lats if x <= FAST_MS]
    slow = counts[SLOW_MS]
    monotone = all(counts[a] <= counts[b] for a, b in zip(lats, lats[1:]))
    if any(c >= FAST_MISS_MIN for c in fast) or slow < SLOW_MISS_BELOW:
        verdict = "MISS"
    elif all(c <= FAST_HIT_MAX for c in fast) and slow >= SLOW_HIT_MIN and monotone:
        verdict = "HIT"
    else:
        verdict = "INCONCLUSIVE"
    return {"counts": {str(x): counts[x] for x in lats}, "non_decreasing": monotone,
            "verdict": verdict}


# --- summary ------------------------------------------------------------------------

def summarize(rows: dict, seeds) -> dict:
    def race_col(k, lf, f):
        return [f(rows[cc.key("race", k, lf, s)]) for s in seeds]

    races = {(k, lf): (sum(race_col(k, lf, lambda r: r["maker_wins"])),
                       sum(race_col(k, lf, lambda r: r["races"])))
             for k in K_RACE for lf in LAG_FRACS}
    rent = {(k, lf): race_col(k, lf, lambda r: cc.net(r, "sniper"))
            for k in K_RACE for lf in (0.0, SLOW_LAG)}
    gaps = {lf: (race_col(21, lf, lambda r: r["gaps"]), race_col(3, lf, lambda r: r["gaps"]))
            for lf in (0.0, SLOW_LAG)}
    exact = {f"{key}@lag/eps={lf}": cc.ratio_ci(race_col(21, lf, lambda r: r[key]),
                                                race_col(3, lf, lambda r: r[key]))
             for key in ("episodes_exact", "one_sided_share_exact") for lf in (0.0, SLOW_LAG)}
    h2d_bands = {}
    for lat in H2D_LATENCIES_MS:
        base = [rows[cc.key("h2d", lat, "base", s)] for s in seeds]
        cells = {(b, r, k): [rows[cc.key("h2d", lat, b, r, k, s)] for s in seeds]
                 for b, r, k in cc.band_cells()}
        for band, v in cc.band_counts(cells, base).items():
            h2d_bands.setdefault(band, {})[lat] = v
    h2d = {band: {**h2d_verdict({lat: v["count"] for lat, v in by.items()}),
                  "by_latency_ms": {str(lat): v for lat, v in by.items()}}
           for band, by in h2d_bands.items()}
    return {
        "H2a": {**h2a(races), "primary": True},
        "H2b": h2b(rent),
        "H2c": h2c(gaps, exact),
        "H2d": {"verdict": h2d["paper"]["verdict"], "verdict_band": "paper", "by_band": h2d},
        "race_cells_mean_rent_usd": {f"k={k},lag/eps={lf}": cc.boot(v) for (k, lf), v in rent.items()},
    }


def main(argv=None) -> dict:
    a = cc.parse_args(NAME, __doc__, argv)
    t0 = time.time()
    rows = cc.run_pending(tasks(a.seed_list), cc.call, a.checkpoint, a.workers, NAME)
    report = {"experiment": NAME, "prediction": "P2", "status": a.status,
              "seeds": a.seed_list, "horizon_s": op.HORIZON_S, "n_runs": len(rows),
              "config": {"k_race": K_RACE, "lag_over_eps": LAG_FRACS, "eps_ns": op.JITTER_NS,
                         "race_snipe_ratio": RACE_RATIO, "h2d_latencies_ms_one_way":
                         H2D_LATENCIES_MS, "paper_policy": repr(cc.paper_policy()),
                         "gap_detector": {"dt_ns": GAP_DT_NS, "max_gap_ns": GAP_MAX_NS,
                                          "k_fund": K_FUND, "sigma_per_step_ticks":
                                          SIGMA_PER_GAP_STEP},
                         "bands": op.BANDS, "k_grid": op.K_GRID},
              "units": "rent and maker P&L in USD per run, net of the operating point's fees",
              **summarize(rows, a.seed_list), "wall_s": round(time.time() - t0, 1)}
    cc.write_report(a.out, report)
    print({h: report[h]["verdict"] for h in ("H2a", "H2b", "H2c", "H2d")}, flush=True)
    return report


if __name__ == "__main__":
    main()
