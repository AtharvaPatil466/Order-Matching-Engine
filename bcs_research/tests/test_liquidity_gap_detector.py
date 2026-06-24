import sys, pathlib
_R = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(_R / "metrics"))

from liquidity_gap_detector import detect_liquidity_gaps, count_liquidity_gaps

DT_US = 1000
SIGMA = 10
KW = dict(dt_us=DT_US, sigma_per_step=SIGMA, k_fund=3.0, max_gap_us=50_000)

BID = 999_950
ASK = 1_000_050
V = 1_000_000


def snap(i, bid, ask, v):
    return {"t": i * 1000, "best_bid": bid, "best_ask": ask, "v": v}


def two_sided(i, v=V):
    return snap(i, BID, ASK, v)


def test_endogenous_one_tick_gap_detected():
    # Arrange: 10 two-sided ticks, one one-sided tick (ask=0), then two-sided.
    snaps = [two_sided(i) for i in range(10)]
    snaps.append(snap(10, BID, 0, V))          # one-sided gap, V flat
    snaps += [two_sided(i) for i in range(11, 21)]

    # Act
    events = detect_liquidity_gaps(snaps, **KW)

    # Assert
    assert len(events) == 1
    ev = events[0]
    assert ev["side_missing"] == "ask"
    assert ev["gap_ticks"] == 1
    assert ev["v_move"] == 0.0


def test_value_driven_gap_not_counted():
    # Arrange: same one-sided tick but V jumps by 200 (> ~52 threshold).
    snaps = [two_sided(i) for i in range(10)]
    snaps.append(snap(10, BID, 0, V + 200))    # value-driven move
    snaps += [two_sided(i, v=V + 200) for i in range(11, 21)]

    # Act / Assert
    assert count_liquidity_gaps(snaps, **KW) == 0


def test_no_recovery_not_counted():
    # Arrange: ten two-sided, then one-sided to the end (never recovers).
    snaps = [two_sided(i) for i in range(10)]
    snaps += [snap(i, BID, 0, V) for i in range(10, 30)]

    # Act / Assert
    assert count_liquidity_gaps(snaps, **KW) == 0


def test_gap_too_long_not_counted():
    # Arrange: 100-tick one-sided run (100_000us > 50_000us max), V flat, recovers.
    snaps = [two_sided(i) for i in range(10)]
    snaps += [snap(i, BID, 0, V) for i in range(10, 110)]
    snaps += [two_sided(i) for i in range(110, 120)]

    # Act / Assert
    assert count_liquidity_gaps(snaps, **KW) == 0


def test_quiet_two_sided_market():
    # Arrange: all two-sided.
    snaps = [two_sided(i) for i in range(50)]

    # Act / Assert
    assert count_liquidity_gaps(snaps, **KW) == 0


def test_multiple_gaps_counted():
    # Arrange: two separate endogenous 1-tick gaps (ask=0), V flat.
    snaps = [two_sided(i) for i in range(5)]
    snaps.append(snap(5, BID, 0, V))           # gap 1
    snaps += [two_sided(i) for i in range(6, 15)]
    snaps.append(snap(15, BID, 0, V))          # gap 2
    snaps += [two_sided(i) for i in range(16, 25)]

    # Act / Assert
    assert count_liquidity_gaps(snaps, **KW) == 2


def test_bid_side_gap():
    # Arrange: a tick with best_bid=0 (ask present), V flat, recovery.
    snaps = [two_sided(i) for i in range(10)]
    snaps.append(snap(10, 0, ASK, V))          # bid side missing
    snaps += [two_sided(i) for i in range(11, 21)]

    # Act
    events = detect_liquidity_gaps(snaps, **KW)

    # Assert
    assert len(events) == 1
    assert events[0]["side_missing"] == "bid"
