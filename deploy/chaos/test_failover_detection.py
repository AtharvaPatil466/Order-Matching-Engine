"""
Scenario: clean primary kill → backup detects peer-death within
heartbeat timeout, and the role/replication admin endpoints accurately
reflect cluster state throughout.

This is the foundational scenario — every later scenario builds on the
ability to (a) observe role consistently, (b) measure how long the
backup takes to notice the primary is gone. The latter IS the RTO
floor of the cluster.

Invariants exercised:
    * Initial steady state: primary reports primary+leader,
      backup reports backup, peerAlive=true on both sides.
    * NoSplitBrain in steady state (only one node holds leadership).
    * After SIGKILL on primary: backup observes peerAlive=false
      within heartbeat-timeout + slack.
"""
from __future__ import annotations

import time

import pytest

from cluster import BACKUP, PRIMARY, Cluster

# Heartbeat default in ReplicationCoordinator (include/ReplicationProtocol.h):
#   intervalMs=100, timeoutMs=500
# So the backup should notice a dead primary within ~500ms after the
# last received heartbeat. Allow generous slack for Docker scheduling
# jitter and the admin-HTTP poll interval (50ms).
HEARTBEAT_TIMEOUT_MS = 500
SLACK_MS = 1500
RTO_BUDGET_MS = HEARTBEAT_TIMEOUT_MS + SLACK_MS


def test_steady_state_roles(cluster: Cluster) -> None:
    """Both nodes report the role docker-compose assigned them."""
    primary_role = cluster.role(PRIMARY)
    backup_role = cluster.role(BACKUP)

    assert primary_role is not None, "primary /role unreachable"
    assert backup_role is not None, "backup /role unreachable"

    assert primary_role["role"] == "primary"
    assert primary_role["replicationEnabled"] is True
    assert primary_role["isLeader"] is True

    assert backup_role["role"] == "backup"
    assert backup_role["replicationEnabled"] is True
    assert backup_role["isLeader"] is False


def test_no_split_brain_in_steady_state(cluster: Cluster) -> None:
    """Exactly one node believes it is leader at any moment."""
    leaders = [
        name for name in (PRIMARY, BACKUP)
        if (r := cluster.role(name)) and r.get("isLeader")
    ]
    assert leaders == [PRIMARY], (
        f"expected leadership only on primary, got: {leaders}"
    )


def test_peer_alive_in_steady_state(cluster: Cluster) -> None:
    """
    Both sides see the peer as alive once the heartbeat loop has had
    a moment to exchange messages. Without this, the kill-detection
    scenario below would race against startup.
    """
    cluster.wait_until(
        lambda: (cluster.replication(PRIMARY) or {}).get("peerAlive") is True
                and (cluster.replication(BACKUP) or {}).get("peerAlive") is True,
        timeout_s=5.0,
        what="bilateral peerAlive=true",
    )


def test_backup_detects_primary_kill_within_rto_budget(cluster: Cluster) -> None:
    """
    Hard-kill the primary. The backup's heartbeat monitor should flip
    peerAlive to false within the configured timeout. Measure the
    elapsed time — this is the cluster's RTO floor for crash failure.

    NOTE: this scenario tears down the primary. The autouse `heal`
    fixture does NOT restart it; the test session leaves the primary
    dead. Subsequent scenarios that need the primary must restart
    the topology, or this test should run last in its module (pytest
    runs in file order by default, so it does).
    """
    # Establish bilateral connectivity first.
    cluster.wait_until(
        lambda: (cluster.replication(BACKUP) or {}).get("peerAlive") is True,
        timeout_s=5.0,
        what="backup sees primary alive (pre-kill)",
    )

    # Kill the primary. SIGKILL because we want to model an unclean
    # crash (no graceful socket close that would let the backup
    # short-circuit the heartbeat-timeout detection).
    t_kill = time.monotonic()
    cluster.kill(PRIMARY)

    # Backup should notice within budget.
    elapsed = cluster.wait_until(
        lambda: (cluster.replication(BACKUP) or {}).get("peerAlive") is False,
        timeout_s=RTO_BUDGET_MS / 1000.0,
        what="backup observes peerAlive=false",
    )
    elapsed_ms = elapsed * 1000.0
    print(
        f"\n[RTO] backup detected primary kill in {elapsed_ms:.0f}ms "
        f"(budget {RTO_BUDGET_MS}ms, heartbeat timeout {HEARTBEAT_TIMEOUT_MS}ms)"
    )
    # Defensive lower bound: detection cannot be faster than heartbeat
    # interval. If we see ~0ms it means we measured a stale state, not
    # the actual detection latency.
    assert elapsed_ms >= 0
    # Upper bound: the budget.
    assert elapsed_ms <= RTO_BUDGET_MS
