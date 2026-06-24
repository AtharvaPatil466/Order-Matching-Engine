"""Liquidity-gap detector (Phase 3, BCS endogenous-instability event).

The thinning feedback does not move the mid; it makes the book go ONE-SIDED for
a moment (a thin level is emptied and the maker has not yet requoted). The
endogenous instability event for this mechanism is a LIQUIDITY GAP: a bounded
one-sided / empty-book episode with the fundamental V flat across it (so it is
not value-driven), followed by recovery to a two-sided book. Pure function over
snapshots; no engine dependency.
"""
from __future__ import annotations

import math


def _two_sided(s):
    return s["best_bid"] > 0 and s["best_ask"] > 0


def detect_liquidity_gaps(snapshots, dt_us, sigma_per_step,
                          k_fund=3.0, max_gap_us=50_000):
    """Return a list of endogenous liquidity-gap events."""
    n = len(snapshots)
    events = []
    i = 0
    while i < n:
        if _two_sided(snapshots[i]):
            i += 1
            continue
        start = i                      # first non-two-sided tick of the episode
        while i < n and not _two_sided(snapshots[i]):
            i += 1
        end = i                        # first two-sided tick after the gap (or n)
        recovered = end < n
        gap_ticks = end - start
        duration_us = gap_ticks * dt_us
        if not recovered or duration_us > max_gap_us:
            continue                   # structural outage, not a flash gap
        # Endogenous test: V flat across the episode (one tick of margin each side).
        lo = max(0, start - 1)
        hi = min(n - 1, end)
        v_move = abs(snapshots[hi]["v"] - snapshots[lo]["v"])
        fund_threshold = k_fund * sigma_per_step * math.sqrt(max(1, gap_ticks + 2))
        if v_move >= fund_threshold:
            continue                   # value-driven, not endogenous
        bid_present = snapshots[start]["best_bid"] > 0
        events.append({
            "start_t": snapshots[start]["t"],
            "recovery_t": snapshots[end]["t"],
            "duration_us": duration_us,
            "gap_ticks": gap_ticks,
            "side_missing": "ask" if bid_present else "bid",
            "v_move": float(v_move),
        })
    return events


def count_liquidity_gaps(snapshots, dt_us, sigma_per_step, **kw):
    return len(detect_liquidity_gaps(snapshots, dt_us, sigma_per_step, **kw))
