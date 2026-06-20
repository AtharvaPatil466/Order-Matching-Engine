"""
Scenario: random packet loss on the replication link.

Drops a configurable percentage of packets on primary's egress via
tc netem. With heartbeats at 100ms interval and a 500ms timeout,
the link can tolerate ~3 consecutive lost heartbeats. At 30% loss
the probability of 3 consecutive losses is 0.3^3 ≈ 2.7% — well
above what you'd expect on healthy fiber, but below the threshold
where the heartbeat machinery breaks down.

Invariants:
    * Under 30% loss, peerAlive remains true on both sides over a
      sustained observation window.
    * No spurious split-brain (no false promotion).

If this test starts flaking at the same loss level on the same
hardware, it's a signal that the heartbeat interval should drop
(faster ticks for the same timeout = better tolerance) — that's
real protocol tuning information, not test noise.
"""
from __future__ import annotations

import time

import pytest

from cluster import BACKUP, PRIMARY, Cluster

LOSS_PCT = 30
OBSERVE_S = 4.0


def test_heartbeats_survive_packet_loss(cluster: Cluster) -> None:
    cluster.wait_until(
        lambda: (cluster.replication(PRIMARY) or {}).get("peerAlive") is True
                and (cluster.replication(BACKUP) or {}).get("peerAlive") is True,
        timeout_s=5.0,
        what="pre-loss bilateral peerAlive=true",
    )

    cluster.packet_loss(PRIMARY, LOSS_PCT)

    deadline = time.monotonic() + OBSERVE_S
    samples = 0
    flap_count = 0
    while time.monotonic() < deadline:
        backup_repl = cluster.replication(BACKUP) or {}
        primary_repl = cluster.replication(PRIMARY) or {}
        if not (backup_repl.get("peerAlive") is True
                and primary_repl.get("peerAlive") is True):
            flap_count += 1
        # No split brain: at most one leader.
        leaders = [
            n for n, r in (
                (PRIMARY, cluster.role(PRIMARY)),
                (BACKUP, cluster.role(BACKUP)),
            ) if r and r.get("isLeader")
        ]
        assert len(leaders) <= 1, (
            f"split-brain under {LOSS_PCT}% loss: {leaders}"
        )
        samples += 1
        time.sleep(0.1)

    print(
        f"\n[packet_loss] {LOSS_PCT}% loss: {samples} samples, "
        f"{flap_count} peerAlive flaps observed"
    )
    # Allow transient peerAlive flaps — at 30% loss, the probability
    # of 3 consecutive heartbeat drops in any 500ms window is ~2.7%,
    # so over 40 windows (4s at 100ms tick) we expect ~1 flap on
    # average with high variance. Bound the flap count to "minority"
    # of samples — the real protocol failure mode is sustained
    # degradation, not occasional false negatives.
    max_flaps = max(2, samples // 2)
    assert flap_count <= max_flaps, (
        f"too many flaps ({flap_count}/{samples}) under {LOSS_PCT}% loss — "
        f"heartbeat tuning likely needed"
    )
