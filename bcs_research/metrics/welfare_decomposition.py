"""Welfare decomposition (Phase 3, BCS Step 8).

The BCS arms race is a TRANSFER: HFTs extract rent from market makers and, via
wider effective trading costs, from noise traders. This is the paper's PRIMARY
treatment effect -- it moves strongly even when quoted spreads do not. Pure
functions over a SimResult; no engine dependency.
"""
from __future__ import annotations


def mark_price(result):
    """Last two-sided mid; fall back to the last fundamental value."""
    for s in reversed(result.snapshots):
        if s["best_bid"] > 0 and s["best_ask"] > 0:
            return s["mid"]
    return result.snapshots[-1]["v"] if result.snapshots else 0.0


def _as_ids(x):
    """Accept a single participant id or a collection of them.

    Step 4 adds competing makers, so `mm_id` may now be several ids. A scalar
    takes the identical code path, which keeps every pre-existing result — and
    the zero-sum residual that audits them — bit-identical.
    """
    if x is None:
        return ()
    if isinstance(x, (list, tuple, set, frozenset)):
        return tuple(x)
    return (x,)


def decompose_welfare(result, mm_id, nt_ids, hft_ids, mark=None):
    if mark is None:
        mark = mark_price(result)
    agents = {a.participant_id: a for a in result.agents}
    mm_ids = _as_ids(mm_id)

    def pnl(pid):
        a = agents.get(pid)
        return float(a.pnl(mark)) if a is not None else 0.0

    def realized(pid):
        """Round-trip P&L where the agent tracks it; falls back to total."""
        a = agents.get(pid)
        fn = getattr(a, "realized_pnl", None)
        return float(fn()) if fn is not None else pnl(pid)

    mm_pnl = float(sum(pnl(i) for i in mm_ids))
    n_makers = sum(1 for i in mm_ids if i in agents)
    nt_pnl = float(sum(pnl(i) for i in nt_ids))
    hft_pnl = float(sum(pnl(j) for j in hft_ids))
    hft_realized = float(sum(realized(j) for j in hft_ids))
    n_hft = sum(1 for j in hft_ids if j in agents)
    duration_s = result.duration_us / 1_000_000.0

    return {
        "mark_price": mark,
        "mm_pnl": mm_pnl,
        "mm_pnl_per_agent": mm_pnl / n_makers if n_makers else 0.0,
        "n_makers": n_makers,
        "noise_trader_pnl": nt_pnl,
        "hft_pnl": hft_pnl,
        "hft_rent": hft_pnl,                       # rent extracted by the arms race
        # Inventory-controlled rent. `hft_rent` includes mark-to-market on the
        # HFT's open lot, whose variance grows with run length and swamps the
        # latency signal over long simulations (§4.4). These two terms split it.
        "hft_rent_realized": hft_realized,
        "hft_rent_inventory": hft_pnl - hft_realized,
        "hft_pnl_per_agent": hft_pnl / n_hft if n_hft else 0.0,
        "noise_trader_loss": -nt_pnl,
        "mm_pnl_per_sec": mm_pnl / duration_s if duration_s else 0.0,
        "hft_rent_per_sec": hft_pnl / duration_s if duration_s else 0.0,
        "zero_sum_residual": mm_pnl + nt_pnl + hft_pnl,  # ~0 in a closed market
    }


def welfare_delta(baseline, treatment):
    """Treatment-minus-baseline for the key welfare terms."""
    keys = ("mm_pnl", "noise_trader_pnl", "hft_pnl", "hft_rent",
            "noise_trader_loss", "mm_pnl_per_sec", "hft_rent_per_sec")
    return {k: treatment[k] - baseline[k] for k in keys}
