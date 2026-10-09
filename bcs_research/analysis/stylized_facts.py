#!/usr/bin/env python3
"""Stylized facts from plain arrays — the same yardstick for the tape and any model.

The reviewer's choice for the paper is binary: show the model reproduces the
market's stylized facts, or say plainly that it is a mechanism model and not
a market replica. That needs one measurement applied to both, so everything
here is computed from plain arrays (trade times, prices, sizes, aggressor
side; a quote series) with no knowledge of where they came from, and in
scale-free units: spreads and returns in bp, volume in mean-trade units,
impact in bp per standard deviation of signed flow.

Data arrive as contiguous SEGMENTS. A segment never spans a data gap, so no
return, interarrival or flow bucket crosses one; `segment_stats` reduces one
segment to sums and samples, `combine` pools them into the facts. A simulator
run is one segment; the tape is every stretch where the 10 Hz book is
continuous (the trade tape alone is hole-free per hour, but the book is far
patchier, and the quote-based facts need it).

Run: bcs_research/.venv/bin/python bcs_research/analysis/stylized_facts.py
(writes the tape facts, then runs the calibrated tick model once and writes
its facts beside them.)
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np

_ROOT = Path(__file__).resolve().parents[1]
for _d in ("build", "agents", "simulation", "metrics", "experiments", "calibration"):
    _p = str(_ROOT / _d)
    if _p not in sys.path:
        sys.path.insert(0, _p)

RETURN_HORIZONS_S = (0.1, 1.0, 10.0)
SIGN_LAGS = (1, 2, 5, 10, 20, 50, 100)
FLOW_BUCKET_S = 1.0
IMPACT_HORIZONS = (0, 1, 5, 10, 30)      # flow buckets after the flow bucket ends
IA_CAP_MS = 60_000
QUANTILES = (10, 50, 90, 99)
MAX_QUOTE_GAP_S = 1.0                    # tape: a longer book gap ends a segment
MIN_SEGMENT_S = 60.0


def _grid(t: np.ndarray, x: np.ndarray, t0: float, t1: float, step: float) -> np.ndarray:
    """Previous-tick sample of x on [t0, t1] every `step` seconds."""
    g = t0 + step * np.arange(int((t1 - t0) / step) + 1)
    return x[np.maximum(np.searchsorted(t, g, side="right") - 1, 0)]


def segment_stats(t, px, qty, buy, qt, bid, ask) -> dict:
    """Sums and samples for one gap-free segment.

    t, px, qty, buy: trades in execution order (t in seconds, buy = aggressor
    bought). qt, bid, ask: two-sided quotes, time-sorted. Trades outside
    [qt[0], qt[-1]] are ignored so trades and quotes cover the same span.
    """
    t0, t1 = float(qt[0]), float(qt[-1])
    keep = (t >= t0) & (t <= t1)
    t, px, qty, buy = t[keep], px[keep], qty[keep], buy[keep]
    mid = (bid + ask) / 2
    log_mid = np.log(mid) * 1e4
    sign = np.where(buy, 1.0, -1.0)
    out = {"duration_s": t1 - t0, "n_trades": int(t.size), "qty_sum": float(qty.sum()),
           "spread_bp": ((ask - bid) / mid * 1e4).astype(np.float32)}

    # per-second activity
    n_b = int((t1 - t0) // FLOW_BUCKET_S)
    b = ((t - t0) // FLOW_BUCKET_S).astype(np.int64)
    inb = b < n_b
    out["count_per_s"] = np.bincount(b[inb], minlength=n_b).astype(np.float32)
    out["vol_per_s"] = np.bincount(b[inb], weights=qty[inb], minlength=n_b).astype(np.float32)

    # returns: lag-1 autocorrelation and variance at each horizon
    for h in RETURN_HORIZONS_S:
        r = np.diff(_grid(qt, log_mid, t0, t1, h))
        out[f"ret_{h}"] = (r.size, float(r.sum()), float(r @ r),
                           r.size - 1, float(r[1:] @ r[:-1]) if r.size > 1 else 0.0)

    # trade-sign autocorrelation, in trade-event time
    out["sign"] = (sign.size, float(sign.sum()),
                   {k: (max(sign.size - k, 0), float(sign[k:] @ sign[:-k]) if sign.size > k else 0.0)
                    for k in SIGN_LAGS})

    # interarrival, 1 ms bins
    ia_ms = np.minimum(np.round(np.diff(t) * 1e3).astype(np.int64), IA_CAP_MS)
    out["ia_hist"] = np.bincount(ia_ms, minlength=IA_CAP_MS + 1)
    out["ia_sums"] = (float(np.diff(t).sum()), float(np.diff(t) @ np.diff(t)))

    # signed flow per bucket against the mid move from bucket start to h buckets after its end
    flow = np.bincount(b[inb], weights=(sign * qty)[inb], minlength=n_b)
    m = _grid(qt, log_mid, t0, t1, FLOW_BUCKET_S)        # m[i] = mid at t0 + i s
    out["flow"] = flow.astype(np.float32)
    out["impact"] = {h: (m[np.minimum(np.arange(n_b) + 1 + h, m.size - 1)] - m[:n_b])
                     .astype(np.float32)[: max(n_b - h, 0)] for h in IMPACT_HORIZONS}
    return out


def _acf(n, s1, s2, m, sxy) -> float:
    mean, var = s1 / n, s2 / n - (s1 / n) ** 2
    return float((sxy / m - mean ** 2) / var) if var > 0 and m > 0 else float("nan")


def _q(x: np.ndarray) -> dict:
    return {f"p{q}": float(np.percentile(x, q)) for q in QUANTILES} if x.size else {}


def combine(parts: list[dict]) -> dict:
    dur = sum(p["duration_s"] for p in parts)
    n_tr = sum(p["n_trades"] for p in parts)
    mean_size = sum(p["qty_sum"] for p in parts) / n_tr
    cnt = np.concatenate([p["count_per_s"] for p in parts])
    vol = np.concatenate([p["vol_per_s"] for p in parts]) / mean_size

    returns = {}
    for h in RETURN_HORIZONS_S:
        n, s1, s2, m, sxy = (sum(p[f"ret_{h}"][i] for p in parts) for i in range(5))
        returns[str(h)] = {"sigma_bp_per_sqrt_s": float(np.sqrt(s2 / n / h)),
                           "lag1_autocorrelation": _acf(n, s1, s2, m, sxy)}

    n = sum(p["sign"][0] for p in parts)
    s1 = sum(p["sign"][1] for p in parts)
    sign_acf = {}
    for k in SIGN_LAGS:
        m = sum(p["sign"][2][k][0] for p in parts)
        sxy = sum(p["sign"][2][k][1] for p in parts)
        sign_acf[str(k)] = _acf(n, s1, n, m, sxy)          # sign^2 == 1

    hist = sum(p["ia_hist"] for p in parts)
    cdf = np.cumsum(hist) / hist.sum()
    ia_sum = sum(p["ia_sums"][0] for p in parts)
    ia_sq = sum(p["ia_sums"][1] for p in parts)
    n_ia = hist.sum()
    ia_mean = ia_sum / n_ia

    flow = np.concatenate([p["flow"] for p in parts]).astype(float)
    sd_flow = float(flow.std())
    impact = {}
    for h in IMPACT_HORIZONS:
        r = np.concatenate([p["impact"][h] for p in parts]).astype(float)
        f = np.concatenate([p["flow"][: p["impact"][h].size] for p in parts]).astype(float)
        cov = float(np.mean((f - f.mean()) * (r - r.mean())))
        impact[str(h)] = cov / f.var() * sd_flow if f.var() > 0 else float("nan")
    r0 = np.concatenate([p["impact"][0] for p in parts]).astype(float)
    f0 = np.concatenate([p["flow"][: p["impact"][0].size] for p in parts]).astype(float)
    edges = np.percentile(f0, np.linspace(0, 100, 11))
    dec = np.clip(np.searchsorted(edges, f0, side="right") - 1, 0, 9)
    by_decile = [float(r0[dec == d].mean()) if (dec == d).any() else float("nan")
                 for d in range(10)]

    spread = np.concatenate([p["spread_bp"] for p in parts]).astype(float)
    return {
        "coverage": {"segments": len(parts), "duration_s": dur, "n_trades": n_tr},
        "spread_bp": _q(spread) | {"mean": float(spread.mean())},
        "trades_per_s": _q(cnt) | {"mean": float(cnt.mean()),
                                   "fano": float(cnt.var() / cnt.mean()),
                                   "share_of_seconds_without_trades": float(np.mean(cnt == 0))},
        "volume_per_s_in_mean_trades": _q(vol),
        "returns": returns,
        "trade_sign_autocorrelation": sign_acf,
        "interarrival_ms": {f"p{q}": int(np.searchsorted(cdf, q / 100)) for q in QUANTILES}
                           | {"share_simultaneous": float(hist[0] / n_ia),
                              "cv": float(np.sqrt(max(ia_sq / n_ia - ia_mean ** 2, 0.0)) / ia_mean)},
        "impact_bp_per_sd_of_signed_flow": impact,
        "impact_bp_by_flow_decile_same_second": by_decile,
    }


# ---------------------------------------------------------------- the tape

def tape_segments():
    """Yield segment_stats for each continuous stretch of book with its hour's trades."""
    from fine_grain import load_book, trade_hours

    for day, hour, tr in trade_hours():
        book = load_book(day, hour)
        if book is None:
            continue
        bid, ask = book["bid_px"][:, 0], book["ask_px"][:, 0]
        ok = (bid > 0) & (ask > bid)
        qt = book["ts_ms"][ok] / 1e3
        bid, ask = bid[ok], ask[ok]
        t = tr["ts_ms"] / 1e3
        cut = np.flatnonzero(np.diff(qt) > MAX_QUOTE_GAP_S) + 1
        for lo, hi in zip(np.r_[0, cut], np.r_[cut, qt.size]):
            s0, s1 = max(qt[lo], t[0]), min(qt[hi - 1], t[-1])
            if s1 - s0 < MIN_SEGMENT_S:
                continue
            q = (qt >= s0) & (qt <= s1)
            yield segment_stats(t, tr["price"], tr["size"], tr["buy"], qt[q], bid[q], ask[q])


# ---------------------------------------------------------------- the tick model

def tick_model_segment(seed: int = 1, n_ticks: int = 36_000) -> dict:
    """One calibrated tick-model run (Exp 1 treatment arm), as a single segment.

    ponytail: captures the SimResult by swapping exp1_primary's scheduler for
    a recording subclass, so the run is exactly the published configuration
    with no copy of its agent wiring.
    """
    from unittest import mock

    import exp1_primary
    from run_calibrated import calibrated_cfg
    from scheduler import LatencyScheduler
    from targets import TAU_S

    captured = []

    class Recording(LatencyScheduler):
        def run(self, *a, **k):
            captured.append(super().run(*a, **k))
            return captured[-1]

    cfg = {**exp1_primary.CFG, **calibrated_cfg(),
           "duration_us": n_ticks * exp1_primary.CFG["dt_us"]}
    with mock.patch.object(exp1_primary, "LatencyScheduler", Recording):
        exp1_primary._run(seed, cfg, with_hft=True)
    res = captured[0]
    to_s = TAU_S / res.dt_us
    tr = res.trades
    snaps = [s for s in res.snapshots if s["best_bid"] > 0 and s["best_ask"] > s["best_bid"]]
    return segment_stats(
        np.array([x["t"] for x in tr], float) * to_s,
        np.array([x["price"] for x in tr], float),
        np.array([x["quantity"] for x in tr], float),
        np.array([x["aggressor_buy"] for x in tr], bool),
        np.array([s["t"] for s in snaps], float) * to_s,
        np.array([s["best_bid"] for s in snaps], float),
        np.array([s["best_ask"] for s in snaps], float))


def main() -> dict:
    out_dir = _ROOT / "results" / "calibration"
    tape = combine(list(tape_segments()))
    (out_dir / "stylized_facts_tape.json").write_text(json.dumps(tape, indent=2, sort_keys=True))
    model = combine([tick_model_segment()])
    model["source"] = ("calibrated tick model (run_calibrated.calibrated_cfg), Exp 1 "
                       "treatment arm, seed 1, 36,000 ticks = 1 h at 100 ms/tick")
    (out_dir / "stylized_facts_tick_model_v2.json").write_text(
        json.dumps(model, indent=2, sort_keys=True))
    for k in tape:
        print(f"{k}\n  tape  {tape[k]}\n  model {model[k]}")
    print("\nwrote results/calibration/stylized_facts_tape.json, stylized_facts_tick_model_v2.json")
    return {"tape": tape, "tick_model": model}


if __name__ == "__main__":
    main()
