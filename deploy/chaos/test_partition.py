"""
Scenario: bidirectional network partition between primary and backup.

Drops traffic in both directions via `iptables -A OUTPUT -d <peer> -j DROP`
on both nodes. Heartbeats stop flowing without either process dying.

Invariants:
    * Backup observes peerAlive=false within heartbeat timeout.
    * Primary observes peerAlive=false within heartbeat timeout.
    * Primary stays in the primary role (no false demotion).
    * Healing the partition restores bilateral peerAlive=true within
      a few heartbeat intervals.

This is the canonical "network is gone but no one crashed" test —
distinguishes detection latency from process-death detection.
"""
from __future__ import annotations

import time

import pytest

from cluster import BACKUP, PRIMARY, Cluster

HEARTBEAT_TIMEOUT_MS = 500
SLACK_MS = 1500
DETECT_BUDGET_MS = HEARTBEAT_TIMEOUT_MS + SLACK_MS
RECOVERY_BUDGET_S = 3.0


def test_bidirectional_partition_detected_then_healed(cluster: Cluster) -> None:
    # Wait for steady-state connectivity.
    cluster.wait_until(
        lambda: (cluster.replication(PRIMARY) or {}).get("peerAlive") is True
                and (cluster.replication(BACKUP) or {}).get("peerAlive") is True,
        timeout_s=5.0,
        what="pre-partition bilateral peerAlive=true",
    )

    # Drop both directions. Use full container names (resolvable via
    # /etc/hosts inside each container) rather than compose aliases,
    # which aren't installed for both nodes in this topology.
    cluster.partition(PRIMARY, BACKUP)
    cluster.partition(BACKUP, PRIMARY)

    # Both sides should notice within heartbeat timeout.
    t0 = time.monotonic()
    elapsed_backup = cluster.wait_until(
        lambda: (cluster.replication(BACKUP) or {}).get("peerAlive") is False,
        timeout_s=DETECT_BUDGET_MS / 1000.0,
        what="backup detects partition",
    )
    elapsed_primary = cluster.wait_until(
        lambda: (cluster.replication(PRIMARY) or {}).get("peerAlive") is False,
        timeout_s=DETECT_BUDGET_MS / 1000.0,
        what="primary detects partition",
    )
    print(
        f"\n[partition] backup detected in {elapsed_backup*1000:.0f}ms, "
        f"primary detected in {elapsed_primary*1000:.0f}ms "
        f"(budget {DETECT_BUDGET_MS}ms)"
    )

    # Primary must NOT have demoted itself. With the lease-renew fix
    # in startAsPrimary, the primary keeps its lease independently of
    # peer reachability. Without that fix this assert would flake.
    primary_role = cluster.role(PRIMARY)
    assert primary_role is not None
    assert primary_role["role"] == "primary"
    assert primary_role["isLeader"] is True, (
        f"primary falsely demoted under partition: {primary_role}"
    )

    # Heal and verify bilateral recovery. ReplicationTransport now
    # auto-reconnects from its receive loop using the saved
    # connectHost/connectPort, so once iptables stops dropping
    # packets the backup re-establishes within a few hundred ms.
    cluster.heal()
    elapsed = cluster.wait_until(
        lambda: (cluster.replication(PRIMARY) or {}).get("peerAlive") is True
                and (cluster.replication(BACKUP) or {}).get("peerAlive") is True,
        timeout_s=RECOVERY_BUDGET_S,
        what="post-heal bilateral peerAlive=true",
    )
    print(f"[partition] recovered to bilateral peerAlive in {elapsed*1000:.0f}ms after heal")
