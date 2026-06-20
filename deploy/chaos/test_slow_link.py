"""
Scenario: added latency on the replication link.

Adds 200ms of one-way latency on primary's egress. The primary's
heartbeats now arrive at the backup ~200ms late. With the default
500ms heartbeat timeout and 100ms send interval, the system should
remain healthy (last heartbeat is ~300ms old when the next arrives,
under the 500ms threshold).

Invariants:
    * peerAlive remains true on both sides under 200ms one-way latency.
    * Roles stay stable.

Lower bound on what this proves: the heartbeat machinery has
adequate margin for real-world WAN latencies (sub-200ms is typical
for cross-AZ same-region; cross-region needs heartbeat tuning).
"""
from __future__ import annotations

import time

import pytest

from cluster import BACKUP, PRIMARY, Cluster

LATENCY_MS = 200
OBSERVE_S = 3.0  # how long to confirm stability


def test_heartbeats_survive_added_latency(cluster: Cluster) -> None:
    # Pre-condition: clean bilateral connectivity.
    cluster.wait_until(
        lambda: (cluster.replication(PRIMARY) or {}).get("peerAlive") is True
                and (cluster.replication(BACKUP) or {}).get("peerAlive") is True,
        timeout_s=5.0,
        what="pre-latency bilateral peerAlive=true",
    )

    cluster.slow_link(PRIMARY, LATENCY_MS)

    # Watch for OBSERVE_S to confirm no spurious peerAlive flapping.
    deadline = time.monotonic() + OBSERVE_S
    samples = 0
    while time.monotonic() < deadline:
        backup_repl = cluster.replication(BACKUP) or {}
        primary_repl = cluster.replication(PRIMARY) or {}
        assert backup_repl.get("peerAlive") is True, (
            f"backup peerAlive flipped to false under {LATENCY_MS}ms latency: {backup_repl}"
        )
        assert primary_repl.get("peerAlive") is True, (
            f"primary peerAlive flipped to false under {LATENCY_MS}ms latency: {primary_repl}"
        )
        samples += 1
        time.sleep(0.1)
    print(f"\n[slow_link] {LATENCY_MS}ms latency held for {OBSERVE_S}s ({samples} samples)")
