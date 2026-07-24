"""Step-4 harness checks: single-maker byte-parity and multi-maker conservation.

The whole risk of generalizing `_run` from one maker to N is (a) silently
changing the pre-registered single-maker path, and (b) dropping a competing
maker's PnL out of the zero-sum identity. These two tests pin both.
"""
import sys
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
for _d in ("build", "agents", "simulation", "metrics", "experiments"):
    _p = str(_ROOT / _d)
    if _p not in sys.path:
        sys.path.insert(0, _p)

from exp1_primary import CFG, _run, _split_qty          # noqa: E402
from exp4_competing_makers import latency_set           # noqa: E402


def test_single_maker_path_is_byte_identical():
    """No `mm_latencies_us` and an explicit one-element list must agree exactly."""
    implicit = _run(1, CFG, with_hft=True)
    explicit = _run(1, {**CFG, "mm_latencies_us": [CFG["mm_latency_us"]]}, with_hft=True)
    assert implicit == explicit


def test_conservation_holds_with_competing_makers():
    """Adding makers must not leak PnL from the zero-sum identity."""
    for n in (2, 3, 5):
        r = _run(1, {**CFG, "mm_latencies_us": latency_set(n)},
                 with_hft=True, per_maker=True)
        assert r["n_makers"] == n
        assert len(r["maker_pnls"]) == n
        assert abs(r["zero_sum_residual"]) < 1e-6   # machine zero, not a leak


def test_quote_is_split_not_duplicated():
    """Aggregate quote across N makers equals the single-maker quote."""
    for n in (1, 2, 3, 5):
        assert sum(_split_qty(CFG["quote_qty"], n)) == CFG["quote_qty"]
