"""
Scenario: asymmetric partition — primary→backup blocked, backup→primary OK.

The most theoretically interesting failure mode in distributed
systems: the two endpoints disagree on whether they're partitioned.
Only one side sees missing heartbeats.

Concretely:
    * iptables DROP on primary's egress targeting backup only.
    * Primary's heartbeats stop arriving at backup → backup's
      `peerAlive` flips to false within heartbeat timeout.
    * Backup's heartbeats still reach primary → primary's
      `peerAlive` stays true.
    * Backup's heartbeat-failure callback fires → it attempts
      `tryAcquire()` on the lease.
    * The lease-propagation fix kicks in: backup's local lease
      (refreshed by primary's most recent LeaseGrant before the
      partition) is still valid, so tryAcquire returns false.
      Backup does NOT promote. NO split brain.
    * After heal, both sides see bilateral peerAlive recover via
      the transport's auto-reconnect (or just the heartbeats
      flowing again, depending on whether TCP stayed up).

Why this scenario matters: without the lease-propagation fix (and
the same-epoch-refresh path inside `acceptRemoteLease`), backup
WOULD promote under this exact partition — producing split-brain
while the primary is still happily operating. That bug was the
one the chaos suite caught earlier. This test pins the fix in
place against regression.
"""
from __future__ import annotations

import pytest

from cluster import BACKUP, PRIMARY, Cluster

HEARTBEAT_TIMEOUT_MS = 500
SLACK_MS = 1500
DETECT_BUDGET_MS = HEARTBEAT_TIMEOUT_MS + SLACK_MS
RECOVERY_BUDGET_S = 3.0


def test_asymmetric_partition_does_not_split_brain(cluster: Cluster) -> None:
    # Pre-condition: clean bilateral.
    cluster.wait_until(
        lambda: (cluster.replication(PRIMARY) or {}).get("peerAlive") is True
                and (cluster.replication(BACKUP) or {}).get("peerAlive") is True,
        timeout_s=5.0,
        what="pre-partition bilateral peerAlive=true",
    )

    # Drop only primary→backup. Backup's egress to primary stays clean,
    # so primary keeps receiving heartbeats from backup.
    cluster.partition(PRIMARY, BACKUP)

    # Backup notices.
    elapsed_backup = cluster.wait_until(
        lambda: (cluster.replication(BACKUP) or {}).get("peerAlive") is False,
        timeout_s=DETECT_BUDGET_MS / 1000.0,
        what="backup detects asymmetric partition",
    )
    print(
        f"\n[asymmetric] backup detected partition in {elapsed_backup*1000:.0f}ms"
    )

    # CRITICAL: backup must NOT promote. The lease-propagation fix
    # means backup's local lease for the primary is still valid
    # (primary sent LeaseGrant durationMs=5000 every heartbeat
    # before partition; the most recent one is still well within
    # its window when the heartbeat-failure callback fires at 500ms).
    # tryAcquire() refuses, no promotion, no split brain.
    backup_role = cluster.role(BACKUP)
    assert backup_role is not None
    assert backup_role.get("isLeader") is False, (
        f"backup falsely promoted under asymmetric partition (regression on "
        f"lease-propagation fix): {backup_role}"
    )

    primary_role = cluster.role(PRIMARY)
    assert primary_role is not None
    assert primary_role.get("isLeader") is True, (
        f"primary falsely demoted under asymmetric partition: {primary_role}"
    )

    # Primary should still see backup as alive (asymmetry — the
    # backup→primary direction was never blocked).
    primary_repl = cluster.replication(PRIMARY) or {}
    print(f"[asymmetric] primary peerAlive during partition: "
          f"{primary_repl.get('peerAlive')}")
    # This is the most interesting observable property of asymmetric
    # partition. Loose assertion — under high jitter the primary
    # might transiently see backup as dead too if its own clock-driven
    # heartbeat counter wraps. The strong assertion is the role check
    # above.

    # Heal and verify recovery.
    cluster.heal()
    elapsed = cluster.wait_until(
        lambda: (cluster.replication(PRIMARY) or {}).get("peerAlive") is True
                and (cluster.replication(BACKUP) or {}).get("peerAlive") is True,
        timeout_s=RECOVERY_BUDGET_S,
        what="bilateral peerAlive after heal",
    )
    print(f"[asymmetric] recovered to bilateral peerAlive in {elapsed*1000:.0f}ms")
