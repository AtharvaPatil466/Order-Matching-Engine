"""Unit tests for flash_crash_detector over synthetic snapshot lists.

A crash here is ENDOGENOUS: the mid dislocates while the fundamental value V
stays put, then the mid recovers near its pre-crash level. These tests build
small synthetic per-tick snapshot lists and assert the detector fires only on
genuine endogenous-and-recovering dislocations.

Detector kwargs are held fixed across every test so the expected thresholds
are stable:
    dt_us=1000, sigma_per_step=10, base_spread_ticks=200,
    k=3.0, t_crash_us=5000, t_recovery_us=20000
which gives:
    w = 5, r = 20
    move_threshold = 3 * 10 * sqrt(5)  ~= 67.08
    fund_threshold = 0.5 * move_threshold ~= 33.54
    epsilon        = 0.5 * 200 = 100
"""
import sys, pathlib
_R = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(_R / "metrics"))

from flash_crash_detector import detect_crashes, count_crashes

# Shared detector kwargs so expected thresholds match across tests.
KW = dict(
    dt_us=1000,
    sigma_per_step=10,
    base_spread_ticks=200,
    k=3.0,
    t_crash_us=5000,
    t_recovery_us=20000,
)

FLAT_MID = 1_000_000
FLAT_V = 1_000_000.0
MOVE_THRESHOLD = 67.08
FUND_THRESHOLD = 33.54


def snap(t, mid, v):
    return {"t": t, "mid": mid, "v": v}


def build(rows):
    """rows: list of (mid, v); t = index * 1000."""
    return [snap(i * 1000, mid, v) for i, (mid, v) in enumerate(rows)]


def test_endogenous_crash_detected():
    """Mid drops 200 (>67) while V stays flat (~0 <33.5), then recovers.

    Use a sharp one-tick drop so no analysis window straddles a gradual ramp
    (every window's pre-crash mid is unambiguously FLAT_MID).
    """
    rows = []
    # Hold flat for 10 ticks.
    for _ in range(10):
        rows.append((FLAT_MID, FLAT_V))
    # Drop mid by 200 in a single tick, V flat (move 200 > 67, v_move 0 < 33.5).
    rows.append((FLAT_MID - 200, FLAT_V))
    # Hold the trough briefly, V flat.
    for _ in range(4):
        rows.append((FLAT_MID - 200, FLAT_V))
    # Recover mid back near flat (within epsilon=100) within the next 20 ticks.
    for _ in range(25):
        rows.append((FLAT_MID - 20, FLAT_V))
    snapshots = build(rows)

    events = detect_crashes(snapshots, **KW)

    assert count_crashes(snapshots, **KW) >= 1
    assert len(events) >= 1
    ev = events[0]
    assert ev["depth_ticks"] < 0  # endogenous DROP
    assert abs(ev["v_move_ticks"]) < FUND_THRESHOLD


def test_exogenous_move_not_a_crash():
    """Mid AND V both drop 200 together and stay -> not endogenous, no recovery."""
    rows = []
    for _ in range(10):
        rows.append((FLAT_MID, FLAT_V))
    # Sharp one-tick drop of 200 in BOTH mid and V (price followed value).
    rows.append((FLAT_MID - 200, FLAT_V - 200))
    # Stay down: V followed price, and price never recovers. Fails both the
    # fundamental-flat test (v_move 200 > 33.5) and the recovery test.
    for _ in range(29):
        rows.append((FLAT_MID - 200, FLAT_V - 200))
    snapshots = build(rows)

    assert count_crashes(snapshots, **KW) == 0


def test_quiet_market_no_crash():
    """Perfectly flat mid == v for 40 ticks -> nothing fires."""
    rows = [(FLAT_MID, FLAT_V) for _ in range(40)]
    snapshots = build(rows)

    assert count_crashes(snapshots, **KW) == 0


def test_one_sided_snapshots_skipped():
    """mid==0 snapshots interleaved in a quiet series are skipped, no raise."""
    rows = []
    for i in range(40):
        if i % 7 == 0:
            rows.append((0, FLAT_V))  # one-sided / empty book
        else:
            rows.append((FLAT_MID, FLAT_V))
    snapshots = build(rows)

    # Must not raise and must report no crashes on an otherwise quiet book.
    assert count_crashes(snapshots, **KW) == 0


def test_recovery_required():
    """Endogenous dip (V flat) that never recovers -> no crash."""
    rows = []
    for _ in range(10):
        rows.append((FLAT_MID, FLAT_V))
    # Sharp one-tick drop of 200, V flat.
    rows.append((FLAT_MID - 200, FLAT_V))
    # Mid stays down (200 > epsilon=100 away from pre-crash level) and never
    # comes back, so the recovery condition is never satisfied.
    for _ in range(29):
        rows.append((FLAT_MID - 200, FLAT_V))
    snapshots = build(rows)

    assert count_crashes(snapshots, **KW) == 0
