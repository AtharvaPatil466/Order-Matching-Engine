"""Unit tests for welfare_decomposition over synthetic SimResults.

Covers mark_price selection, the welfare decomposition dict, per-second rates,
missing-agent handling, the baseline-vs-treatment delta, and JSON safety.
"""
import sys, pathlib
_R = pathlib.Path(__file__).resolve().parents[1]
for _d in ("build", "simulation", "metrics"):
    sys.path.insert(0, str(_R / _d))

import json

import pytest

from scheduler import SimResult
from welfare_decomposition import decompose_welfare, mark_price, welfare_delta


class StubAgent:
    """Minimal agent: fixed pnl regardless of mark."""

    def __init__(self, participant_id, pnl_value):
        self.participant_id = participant_id
        self._pnl_value = pnl_value

    def pnl(self, mark):
        return self._pnl_value


MM_ID = 1
NT_IDS = [2, 3]
HFT_IDS = [200, 201, 202]
DT_US = 1000


def _two_sided_snapshot(t, mid, v):
    return {
        "t": t,
        "best_bid": mid - 50,
        "best_ask": mid + 50,
        "mid": mid,
        "v": v,
        "tick_volume": 0,
        "signed_volume": 0,
    }


def _one_sided_snapshot(t, v):
    return {
        "t": t,
        "best_bid": v - 50,
        "best_ask": 0,
        "mid": 0,
        "v": v,
        "tick_volume": 0,
        "signed_volume": 0,
    }


def _make_result(agents, duration_us=3_000_000, snapshots=None):
    if snapshots is None:
        snapshots = [_two_sided_snapshot(0, 1_000_000, 1_000_000)]
    return SimResult(snapshots, [], agents, duration_us, DT_US)


# --- 1. mark_price -----------------------------------------------------------

def test_mark_price_returns_last_two_sided_mid():
    snaps = [
        _two_sided_snapshot(0, 1_000_000, 1_000_000),
        _one_sided_snapshot(DT_US, 1_010_000),
    ]
    result = _make_result([], snapshots=snaps)
    # Must pick the last TWO-SIDED mid, not the trailing one-sided mid (0).
    assert mark_price(result) == 1_000_000


def test_mark_price_falls_back_to_last_v_when_all_one_sided():
    snaps = [
        _one_sided_snapshot(0, 1_000_000),
        _one_sided_snapshot(DT_US, 1_020_000),
    ]
    result = _make_result([], snapshots=snaps)
    assert mark_price(result) == 1_020_000


# --- 2. decompose basic ------------------------------------------------------

def test_decompose_basic():
    agents = [
        StubAgent(MM_ID, 40.0),
        StubAgent(2, -60.0),
        StubAgent(3, -52.0),
        StubAgent(200, 24.0),
        StubAgent(201, 25.0),
        StubAgent(202, 23.0),
    ]
    d = decompose_welfare(_make_result(agents), MM_ID, NT_IDS, HFT_IDS)
    assert d["mm_pnl"] == 40.0
    assert d["noise_trader_pnl"] == -112.0
    assert d["hft_pnl"] == 72.0
    assert d["hft_rent"] == 72.0
    assert d["hft_pnl_per_agent"] == pytest.approx(24.0)
    assert d["noise_trader_loss"] == 112.0
    assert d["zero_sum_residual"] == pytest.approx(0.0)


# --- 3. per-sec rates --------------------------------------------------------

def test_per_sec_rates():
    agents = [
        StubAgent(MM_ID, 40.0),
        StubAgent(200, 24.0),
        StubAgent(201, 25.0),
        StubAgent(202, 23.0),
    ]
    # duration_us == 3_000_000 -> duration_s == 3.0
    d = decompose_welfare(_make_result(agents, duration_us=3_000_000),
                          MM_ID, NT_IDS, HFT_IDS)
    assert d["mm_pnl_per_sec"] == pytest.approx(40.0 / 3.0)
    assert d["hft_rent_per_sec"] == pytest.approx(72.0 / 3.0)


# --- 4. missing agents -------------------------------------------------------

def test_missing_hft_contributes_zero_and_excluded_from_per_agent():
    agents = [
        StubAgent(MM_ID, 40.0),
        StubAgent(200, 24.0),
        StubAgent(201, 25.0),
    ]
    hft_ids = [200, 201, 999]  # 999 not present
    d = decompose_welfare(_make_result(agents), MM_ID, NT_IDS, hft_ids)
    assert d["hft_pnl"] == 49.0           # 24 + 25, missing contributes 0
    assert d["hft_pnl_per_agent"] == pytest.approx(49.0 / 2)  # only 2 present


# --- 5. welfare_delta --------------------------------------------------------

def test_welfare_delta_headline_transfer():
    baseline = {
        "mm_pnl": 100.0, "noise_trader_pnl": -50.0, "hft_pnl": 0.0,
        "hft_rent": 0.0, "noise_trader_loss": 50.0,
        "mm_pnl_per_sec": 0.0, "hft_rent_per_sec": 0.0,
    }
    treatment = {
        "mm_pnl": 40.0, "noise_trader_pnl": -112.0, "hft_pnl": 72.0,
        "hft_rent": 72.0, "noise_trader_loss": 112.0,
        "mm_pnl_per_sec": 0.0, "hft_rent_per_sec": 0.0,
    }
    delta = welfare_delta(baseline, treatment)
    assert delta["hft_rent"] == 72.0
    assert delta["mm_pnl"] == -60.0


# --- 6. json-serializable ----------------------------------------------------

def test_decompose_is_json_serializable():
    agents = [
        StubAgent(MM_ID, 40.0),
        StubAgent(2, -60.0),
        StubAgent(200, 24.0),
    ]
    d = decompose_welfare(_make_result(agents), MM_ID, NT_IDS, HFT_IDS)
    json.dumps(d)  # must not raise
