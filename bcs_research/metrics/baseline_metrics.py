"""Baseline market-quality + welfare metrics (Phase 1).

Pure functions over a SimResult. No HFT here — this is the control group the
BCS treatment (Phase 2+) is measured against. By construction the baseline has
no endogenous-crash mechanism, so `endogenous_crashes` is 0.
"""
from __future__ import annotations

import numpy as np

import bcs_engine as be

PP = be.PRICE_PRECISION


def _two_sided(s):
    return s["best_bid"] > 0 and s["best_ask"] > 0


def compute_metrics(result, mm_id, nt_ids, kyle_horizon_ticks=5):
    snaps = result.snapshots
    duration_s = result.duration_us / 1_000_000.0

    rel, absol = [], []
    for s in snaps:
        if _two_sided(s):
            sp = s["best_ask"] - s["best_bid"]
            absol.append(sp)
            rel.append(sp / s["mid"])
    tw_rel = float(np.mean(rel)) if rel else 0.0
    tw_abs = float(np.mean(absol)) if absol else 0.0

    num = den = 0.0
    for s in snaps:
        if _two_sided(s) and s["tick_volume"] > 0:
            num += (s["best_ask"] - s["best_bid"]) / s["mid"] * s["tick_volume"]
            den += s["tick_volume"]
    vw_rel = float(num / den) if den else 0.0

    mark = None
    for s in reversed(snaps):
        if _two_sided(s):
            mark = s["mid"]
            break
    if mark is None:
        mark = snaps[-1]["v"] if snaps else 0.0

    agents = {a.participant_id: a for a in result.agents}
    # `mm_id` may be one maker or several (step 4 adds competing makers). A
    # scalar takes the identical path, so existing results are unchanged.
    mm_ids = mm_id if isinstance(mm_id, (list, tuple, set, frozenset)) else (mm_id,)
    mm_pnl = float(sum(agents[i].pnl(mark) for i in mm_ids if i in agents))
    nt_pnl = float(sum(agents[i].pnl(mark) for i in nt_ids if i in agents))

    mids = [s["mid"] for s in snaps if s["mid"] > 0]
    rets = np.diff(mids) if len(mids) > 1 else np.array([])
    reversals = 0
    last = 0
    for r in rets:
        sgn = 1 if r > 0 else (-1 if r < 0 else 0)
        if sgn != 0 and last != 0 and sgn != last:
            reversals += 1
        if sgn != 0:
            last = sgn

    # Kyle's lambda — LAGGED price impact (Hasbrouck-style): regress the
    # forward-window mid change mid[t+k] - mid[t] on signed flow at t. The
    # forward window (not the contemporaneous tick) is what makes this
    # referee-defensible here: a latency-disadvantaged market maker quotes
    # stale, so a same-tick regression scores its delayed catch-up requote as
    # NEGATIVE impact. Measuring the move over the next k ticks instead
    # captures that catch-up as the POSITIVE permanent impact it actually is.
    k = max(1, int(kyle_horizon_ticks))
    ts = [(s["mid"], s["signed_volume"]) for s in snaps if s["mid"] > 0]
    xs, ys = [], []
    for i in range(len(ts) - k):
        xs.append(ts[i][1])
        ys.append(ts[i + k][0] - ts[i][0])
    lam = r2 = 0.0
    if len(xs) >= 3 and np.std(xs) > 0:
        x = np.asarray(xs, float)
        y = np.asarray(ys, float)
        lam = float(np.polyfit(x, y, 1)[0])
        corr = np.corrcoef(x, y)[0, 1]
        r2 = float(corr * corr) if not np.isnan(corr) else 0.0

    total_vol = int(sum(t["quantity"] for t in result.trades))

    return {
        "duration_seconds": duration_s,
        "dt_us": result.dt_us,
        "n_snapshots": len(snaps),
        "n_trades": len(result.trades),
        "total_volume": total_vol,
        "time_weighted_avg_spread_rel": tw_rel,
        "time_weighted_avg_spread_ticks": tw_abs,
        "time_weighted_avg_spread_dollars": tw_abs / PP,
        "volume_weighted_avg_spread_rel": vw_rel,
        "mm_pnl_dollars": mm_pnl,
        "mm_pnl_per_sec": mm_pnl / duration_s if duration_s else 0.0,
        "mm_inventory_final": int(sum(agents[i].inventory for i in mm_ids
                                      if i in agents)),
        "noise_trader_pnl_dollars": nt_pnl,
        "noise_trader_pnl_per_sec": nt_pnl / duration_s if duration_s else 0.0,
        "price_reversals": reversals,
        "reversals_per_sec": reversals / duration_s if duration_s else 0.0,
        "kyle_lambda": lam,
        "kyle_lambda_r2": r2,
        "kyle_lambda_horizon_ticks": k,
        "endogenous_crashes": 0,
    }
