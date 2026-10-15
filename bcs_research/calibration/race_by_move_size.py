#!/usr/bin/env python3
"""Race rate on the BTC tape as a function of the size of the move behind it.

EXPLORATORY. Not a test of any pre-registered prediction.

WHY THIS EXISTS. The rebuilt BCS event model, at its signed-off operating
point (Binance VIP-9 taker fee 1.7 bp, Laplace jumps fitted to the tape:
0.200/s at scale b = 0.705 bp on the 10 ms grid), snipes about 0.02 times a
second. Only jumps that clear the fee are worth taking, and those arrive at
lambda_j * exp(-h/b) = 0.003-0.018/s across the jump fit's horizons. The
tape's race proxy (race_proxy.py) counts 0.29-1.39 races a second, a 15-70x
gap. But that proxy's trigger is ANY own-direction move of the implied mid,
and most such moves are one exchange tick (~0.016 bp), about a hundredth of
the fee. This module asks whether the gap survives once the race has to
follow a move that a fee-paying sniper would chase.

TWO RATES PER MOVE-SIZE THRESHOLD:

  races     the proxy's own races (its triggers, windows, merging and both
            bounds, rebuilt here from race_proxy's functions; the 'any' row
            reproduces its WindowAccumulator exactly, which a test checks),
            kept when the move behind them is at least the threshold. The
            move runs from the PRE-MOVE MID (the implied mid of the last
            record before the race's first millisecond) to the FURTHEST
            implied mid the race's own records reach in its direction. It is
            not the trigger record's own step: a sweep prints one record per
            price level, so a 1.7 bp jump through a dense book shows up as
            ~100 one-tick steps, and thresholding single steps would find no
            fee-clearing race at all.
  moves     price moves at least the threshold, whether or not a race
            followed, measured two ways:
            - episodes: the same clusters with no minimum record count, so a
              lone sweep or a single-record jump counts too. Races divided
              by episodes is the share of such moves that drew a race;
            - grid: nonzero |log return| of the implied mid on the jump fit's
              own 10 ms / 100 ms / 1 s grids, per hourly file (so no return
              crosses a gap). lambda_j * exp(-h/b) at each horizon is the
              model's version of this number.

"fee + half-spread" adds half an exchange tick, in bp at the move's own
pre-move mid (~0.008 bp), to the 1.7 bp fee: a taker of the stale quote pays
that on top of the fee.

INTERVALS. A block bootstrap over DAYS (27 blocks): each draw re-forms the
ratio of summed counts to summed covered seconds (or volume). Days rather than
hours, because intraday seasonality and volatility clustering make adjacent
hours dependent. The model rates are treated as fixed numbers.

Run: bcs_research/.venv/bin/python bcs_research/calibration/race_by_move_size.py
"""
from __future__ import annotations

import json
import math
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

from fine_grain import (INTERVALS_MS, OUT, SAMPLE_DAYS, implied_mid,  # noqa: E402
                        on_grid, trade_hours)
from race_proxy import (MIN_RECORDS, WINDOWS_MS, _at, cluster_end,  # noqa: E402
                        merge_clusters, triggers)
from targets import EXCHANGE_TICK  # noqa: E402

THRESHOLDS_BP = (0.1, 0.25, 0.5, 1.0, 1.7, 2.5, 5.0)
FEE_BP = 1.7                    # Binance USD-M VIP 9 taker, the operating point's fee
LABELS = ("any",) + tuple(str(t) for t in THRESHOLDS_BP) + ("fee+half_spread",)
GRID_MS = INTERVALS_MS          # the jump fit's horizons: 10, 100, 1000 ms
MODEL_SNIPES_PER_S = 0.02       # operating-point runs of the rebuilt event model, as reported
BOOT_DRAWS = 2000
SEED = 20261016
OP_DRAWS = 400_000              # MC draws per jump count: ~1% error at 1.7 bp, ~5% at 5 bp
OP_MAX_JUMPS = 6                # P(> 6 jumps in 1 s at 0.2/s) ~ 1e-10
CLUSTER_KINDS = ("races", "lb_races", "episodes")


def exceed(move_bp: np.ndarray, hs_bp: np.ndarray,
           weights: np.ndarray | None = None) -> np.ndarray:
    """Count (or sum `weights`) at or above each row of LABELS.

    'any' keeps every entry, even one whose move is unknown (NaN), so it stays
    equal to the unthresholded proxy; every other row needs a known move.
    """
    w = np.ones(move_bp.size) if weights is None else weights
    rows = ([np.ones(move_bp.size, dtype=bool)]
            + [move_bp >= t for t in THRESHOLDS_BP]
            + [move_bp - hs_bp >= FEE_BP])
    return np.array([w[r].sum() for r in rows])


def side_clusters(ts: np.ndarray, trig: np.ndarray, window_ms: int) -> dict:
    """Race ids per record for ONE side, built exactly as race_proxy builds them.

    races     >= MIN_RECORDS same-side records from the trigger's ms to W after;
    lb_races  those spanning >= 2 stamps, so >= 2 takers for certain;
    episodes  every trigger's cluster, with no minimum: every visible move.
    """
    lo = np.searchsorted(ts, ts, side="left")
    end = cluster_end(ts, window_ms)
    race = trig & ((end - lo) >= MIN_RECORDS)
    certain = race & (ts[end - 1] > ts)
    return {k: merge_clusters(_at(lo[m], ts.size), end)
            for k, m in zip(CLUSTER_KINDS, (race, certain, trig))}


def spans(rid: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """[a, b) record range of each race, in race order."""
    a = np.flatnonzero((rid > 0) & (np.r_[0, rid[:-1]] != rid))
    return a, a + np.bincount(rid)[1:]


def burst_moves(a: np.ndarray, b: np.ndarray, y: np.ndarray, sign: float,
                pre_mid: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Each burst's move (bp) and half a tick (bp) at its pre-move mid.

    `y` is sign * log(implied mid) of the side's records (sign +1 for buys,
    -1 for sells), so the furthest level reached is always a maximum;
    `pre_mid` is each side record's pre-move mid, NaN where none exists.
    """
    if a.size == 0:
        return np.empty(0), np.empty(0)
    ext = np.maximum.reduceat(np.r_[y, 0.0], np.c_[a, b].ravel())[::2]
    pre = pre_mid[a]
    return (ext - sign * np.log(pre)) * 1e4, 0.5 * EXCHANGE_TICK / pre * 1e4


def grid_moves(ts: np.ndarray, mid: np.ndarray) -> np.ndarray:
    """(len(GRID_MS), len(LABELS)) counts of nonzero grid moves by size."""
    rows = []
    for dt in GRID_MS:
        g = on_grid(ts, mid, dt)
        r = np.abs(np.diff(np.log(g))) * 1e4
        nz = r > 0
        rows.append(exceed(r[nz], 0.5 * EXCHANGE_TICK / g[:-1][nz] * 1e4))
    return np.array(rows)


def hour_counts(tr: dict) -> dict:
    """Per-threshold sums for one hole-free hourly file.

    Cluster counts and volumes are (len(WINDOWS_MS), len(LABELS)) arrays.
    """
    ts_all = tr["ts_ms"]
    mid = implied_mid(tr["price"], tr["buy"])
    move, _ = triggers(mid, tr["buy"])
    j = np.searchsorted(ts_all, ts_all, side="left") - 1   # last record before each ms
    pre_mid = np.where(j >= 0, mid[np.maximum(j, 0)], np.nan)
    shape = (len(WINDOWS_MS), len(LABELS))
    out = {k: np.zeros(shape) for k in CLUSTER_KINDS + ("races_vol", "lb_vol")}
    for side in (True, False):
        idx = np.flatnonzero(tr["buy"] == side)
        if idx.size == 0:
            continue
        ts = ts_all[idx]
        sign = 1.0 if side else -1.0
        y = sign * np.log(mid[idx])
        cum = np.r_[0.0, np.cumsum(tr["size"][idx])]
        for k, w in enumerate(WINDOWS_MS):
            for kind, rid in side_clusters(ts, move[idx], w).items():
                a, b = spans(rid)
                m, hs = burst_moves(a, b, y, sign, pre_mid[idx])
                out[kind][k] += exceed(m, hs)
                if kind != "episodes":
                    vol_key = "races_vol" if kind == "races" else "lb_vol"
                    out[vol_key][k] += exceed(m, hs, cum[b] - cum[a])
    return out | {"grid": grid_moves(ts_all, mid),
                  "covered_s": (ts_all[-1] - ts_all[0]) / 1e3,
                  "vol_all": float(tr["size"].sum()),
                  "mid_sum": float(mid.sum()), "n_records": float(mid.size)}


def day_intervals(num: np.ndarray, den: np.ndarray, seed: int = SEED,
                  draws: int = BOOT_DRAWS) -> tuple[list, list, list]:
    """Pooled ratio sum(num)/sum(den) and its 95% day-block bootstrap interval.

    `num` is (n_days, ...); `den` is (n_days,) or num's shape. Each draw
    resamples whole days with replacement, so a draw's weights are day
    multiplicities and the ratio is re-formed from re-summed totals.
    """
    n = num.shape[0]
    flat = num.reshape(n, -1)
    d = np.broadcast_to(den.reshape(n, -1), flat.shape)
    pick = np.random.default_rng(seed).integers(0, n, size=(draws, n))
    mult = np.zeros((draws, n))
    np.add.at(mult, (np.arange(draws)[:, None], pick), 1.0)
    with np.errstate(invalid="ignore", divide="ignore"):
        boot = (mult @ flat) / (mult @ d)
        est = flat.sum(0) / d.sum(0)
    lo, hi = np.nanpercentile(boot, [2.5, 97.5], axis=0)
    shape = num.shape[1:]
    return tuple(x.reshape(shape).tolist() for x in (est, lo, hi))


def _levels(half_spread_bp: float) -> np.ndarray:
    return np.r_[0.0, THRESHOLDS_BP, FEE_BP + half_spread_bp]


def model_snipeable(fit: dict, half_spread_bp: float) -> np.ndarray:
    """The model's rate of jumps at least each threshold: lambda_j * exp(-h/b)."""
    lam, b = fit["jump_intensity_per_s"], fit["jump_scale_bp_laplace_mle"]
    return lam * np.exp(-_levels(half_spread_bp) / b)


def operating_point_moves(op: dict, dt_ms: int, half_spread_bp: float,
                          draws: int = OP_DRAWS, seed: int = SEED) -> np.ndarray:
    """Rate (/s) at which the OPERATING-POINT model's price moves >= each level over dt_ms.

    lambda_j * exp(-h/b) at 100 ms and 1 s uses those horizons' own fits, but
    the model runs only the 10 ms fit. Its move over dt is a Poisson(lambda dt)
    sum of Laplace(b) jumps plus N(0, sigma^2 dt) diffusion; the jump-count
    mixture is summed exactly and each count's tail is a seeded Monte Carlo.
    The 'any' row is the jump rate P(a jump) / dt. Under diffusion every
    model interval moves, so no 'any' row compares with the tape's; nor do the
    sub-0.5 bp rows at 100 ms and 1 s, where diffusion alone clears them.
    """
    lam, b, sig = op["jump_intensity_per_s"], op["jump_scale_bp"], op["sigma_bp_per_sqrt_s"]
    dt = dt_ms / 1e3
    m = lam * dt
    h = _levels(half_spread_bp)[1:]
    rng = np.random.default_rng(seed)
    p = np.zeros(h.size)
    for n in range(OP_MAX_JUMPS + 1):
        x = (rng.laplace(0.0, b, size=(draws, n)).sum(1)
             + rng.normal(0.0, sig * np.sqrt(dt), size=draws))
        p += math.exp(-m) * m ** n / math.factorial(n) * (np.abs(x)[:, None] >= h).mean(0)
    return np.r_[-math.expm1(-m), p] / dt


# ---------------------------------------------------------------- the run

def collect(days=SAMPLE_DAYS) -> tuple[list[str], dict, list[str], int]:
    """Per-day sums of every hour_counts field, day order sorted."""
    skipped: list[str] = []
    by_day: dict[str, dict] = {}
    n_hours = 0
    for day, _, tr in trade_hours(days, skipped=skipped):
        c = hour_counts(tr)
        acc = by_day.setdefault(day, {})
        for k, v in c.items():
            acc[k] = acc.get(k, 0) + v
        n_hours += 1
    order = sorted(by_day)
    stacked = {k: np.stack([np.asarray(by_day[d][k], float) for d in order])
               for k in by_day[order[0]]}
    return order, stacked, skipped, n_hours


def _iv(est, lo, hi) -> dict:
    return {"est": est, "lo": lo, "hi": hi}


def race_table(s: dict) -> dict:
    """{window: {label: rates and volume shares with intervals}}."""
    per_s = {k: day_intervals(s[k], s["covered_s"]) for k in CLUSTER_KINDS}
    vol = {k: day_intervals(s[k], s["vol_all"]) for k in ("races_vol", "lb_vol")}
    hit = day_intervals(s["races"], s["episodes"])
    out = {}
    for i, w in enumerate(WINDOWS_MS):
        out[str(w)] = {lab: {
            "races_per_s": _iv(*(x[i][j] for x in per_s["races"])),
            "share_of_volume_in_races": _iv(*(x[i][j] for x in vol["races_vol"])),
            "lower_bound_races_per_s": _iv(*(x[i][j] for x in per_s["lb_races"])),
            "lower_bound_share_of_volume": _iv(*(x[i][j] for x in vol["lb_vol"])),
            "move_episodes_per_s": _iv(*(x[i][j] for x in per_s["episodes"])),
            "share_of_move_episodes_that_drew_a_race": _iv(*(x[i][j] for x in hit)),
        } for j, lab in enumerate(LABELS)}
    return out


def grid_table(s: dict, fit: dict, hs_bp: float) -> dict:
    """{horizon: {label: tape move rate against both model rates}}."""
    est, lo, hi = day_intervals(s["grid"], s["covered_s"])
    fits = fit["diagnostics"]["by_interval_ms"]
    out = {}
    for i, dt in enumerate(GRID_MS):
        model = model_snipeable(fits[str(dt)], hs_bp)
        op = operating_point_moves(fit, dt, hs_bp)
        out[str(dt)] = {lab: {
            "moves_per_s": _iv(est[i][j], lo[i][j], hi[i][j]),
            "model_snipeable_jumps_per_s": float(model[j]),
            "tape_over_model": _iv(*(x[i][j] / model[j] for x in (est, lo, hi))),
            "operating_point_moves_per_s": float(op[j]),
            "tape_over_operating_point": _iv(*(x[i][j] / op[j] for x in (est, lo, hi))),
        } for j, lab in enumerate(LABELS)}
    return out


def comparison(races: dict, grid: dict) -> dict:
    """The fee rows the verdict rests on, at race_proxy_v2.json's two bounds."""
    fee = "fee+half_spread"
    pick = {"upper_bound_W0": races["0"][fee]["races_per_s"],
            "lower_bound_W1": races["1"][fee]["lower_bound_races_per_s"]}
    return {
        "model_snipes_per_s": MODEL_SNIPES_PER_S,
        "tape_races_after_fee_clearing_moves": {
            k: v | {"over_model": {q: v[q] / MODEL_SNIPES_PER_S for q in ("est", "lo", "hi")}}
            for k, v in pick.items()},
        "tape_races_any_move": {"upper_bound_W0": races["0"]["any"]["races_per_s"],
                                "lower_bound_W1": races["1"]["any"]["lower_bound_races_per_s"]},
        "tape_fee_clearing_moves_vs_model": {dt: grid[dt][fee] for dt in grid},
    }


def main() -> dict:
    days, s, skipped, n_hours = collect()
    fit = json.loads((OUT / "jump_fit_v2.json").read_text())
    hs_bp = float(0.5 * EXCHANGE_TICK / (s["mid_sum"].sum() / s["n_records"].sum()) * 1e4)
    races, grid = race_table(s), grid_table(s, fit, hs_bp)
    out = {
        "exploratory": ("Not pre-registered; not a test of any pre-registered "
                        "prediction. Written to qualify the race-gap limitation."),
        "sample": {"days": days, "n_days": len(days), "hour_files": n_hours,
                   "covered_s": float(s["covered_s"].sum()), "skipped_files": skipped,
                   "mean_half_spread_bp": hs_bp},
        "thresholds": list(LABELS),
        "definition": {
            "race": "race_proxy.py's race (upper bound) and >=2-stamp race (lower bound)",
            "move": ("log move, in the race's direction, from the implied mid of the "
                     "last record before the race's first ms to the furthest implied "
                     "mid the race's own records reach"),
            "fee+half_spread": f"move - half a tick (bp at the pre-move mid) >= {FEE_BP} bp",
            "move_episodes": "the same clusters with no minimum record count",
            "grid_moves": "nonzero |log return| of the implied mid, per hourly file",
            "model_snipeable": "lambda_j * exp(-h/b) from jump_fit_v2.json at each horizon",
            "operating_point_moves": ("rate of moves >= h over each grid horizon in the "
                                      "model as run: the 10 ms fit's jumps plus diffusion"),
        },
        "bootstrap": {"blocks": "UTC day", "n_blocks": len(days), "draws": BOOT_DRAWS,
                      "seed": SEED, "level": 0.95, "interval": "percentile"},
        "races_by_window_ms": races,
        "grid_moves_by_interval_ms": grid,
        "comparison": comparison(races, grid),
    }
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "race_by_move_size_v2.json").write_text(json.dumps(out, indent=2, sort_keys=True))
    _print(races, grid)
    return out


def _fmt(iv: dict, scale: float = 1.0) -> str:
    return f"{iv['est'] * scale:.4f} [{iv['lo'] * scale:.4f}, {iv['hi'] * scale:.4f}]"


def _print(races: dict, grid: dict) -> None:
    print(f"{'thr bp':>16} {'UB W0 races/s':>28} {'LB W1 races/s':>28} "
          f"{'UB W0 vol%':>26} {'episodes W0 /s':>28}")
    for lab in LABELS:
        r0, r1 = races["0"][lab], races["1"][lab]
        print(f"{lab:>16} {_fmt(r0['races_per_s']):>28} "
              f"{_fmt(r1['lower_bound_races_per_s']):>28} "
              f"{_fmt(r0['share_of_volume_in_races'], 100):>26} "
              f"{_fmt(r0['move_episodes_per_s']):>28}")
    for dt, rows in grid.items():
        print(f"\ngrid {dt} ms: tape moves/s  vs  lambda*exp(-h/b)  vs  operating point")
        for lab in LABELS:
            print(f"{lab:>16} {_fmt(rows[lab]['moves_per_s']):>28} "
                  f"{rows[lab]['model_snipeable_jumps_per_s']:.4f} "
                  f"{rows[lab]['operating_point_moves_per_s']:.4f}")
    print("\nwrote results/calibration/race_by_move_size_v2.json")


if __name__ == "__main__":
    main()
