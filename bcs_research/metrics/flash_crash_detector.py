"""Flash-crash detector (Phase 3, BCS Step 9).

A crash is an ENDOGENOUS price dislocation. Over a window of T_crash the mid
moves by more than k * sigma * sqrt(window_ticks) while V moves by less than
half that, and within T_recovery the mid returns near its pre-crash level. The
'V barely moved' condition is what makes the move endogenous rather than price
following value. Pure function over snapshots; no engine dependency.
"""
from __future__ import annotations

import math


def detect_crashes(snapshots, dt_us, sigma_per_step, base_spread_ticks,
                   k=3.0, t_crash_us=100_000, t_recovery_us=500_000):
    """Return a list of crash-event dicts."""
    w = max(1, int(t_crash_us // dt_us))
    r = max(1, int(t_recovery_us // dt_us))
    move_threshold = k * sigma_per_step * math.sqrt(w)
    fund_threshold = 0.5 * move_threshold
    epsilon = 0.5 * base_spread_ticks

    mids = [s["mid"] for s in snapshots]
    vs = [s["v"] for s in snapshots]
    n = len(snapshots)
    crashes = []
    i = w
    while i < n:
        m_now, m_prev = mids[i], mids[i - w]
        if m_now <= 0 or m_prev <= 0:
            i += 1
            continue
        price_move = m_now - m_prev
        if abs(price_move) > move_threshold:
            v_move = vs[i] - vs[i - w]
            if abs(v_move) < fund_threshold:
                j = min(i + r, n - 1)
                if mids[j] > 0 and abs(mids[j] - m_prev) < epsilon:
                    crashes.append({
                        "start_t": snapshots[i - w]["t"],
                        "trough_t": snapshots[i]["t"],
                        "depth_ticks": float(price_move),
                        "v_move_ticks": float(v_move),
                        "recovery_t": snapshots[j]["t"],
                    })
                    i += w  # advance past this window to avoid double counting
                    continue
        i += 1
    return crashes


def count_crashes(snapshots, dt_us, sigma_per_step, base_spread_ticks, **kw):
    return len(detect_crashes(snapshots, dt_us, sigma_per_step,
                              base_spread_ticks, **kw))
