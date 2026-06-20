"""
Scenario: freeze + thaw the primary (SIGSTOP / SIGCONT via docker pause).

Unlike a kill, the TCP socket remains open while the process is
frozen — but no progress is made, so heartbeats stop flowing. Backup
should observe peerAlive=false within heartbeat timeout. On unpause,
heartbeats resume and peerAlive returns to true.

This validates that the heartbeat machinery distinguishes
"process is hung / paused" from "process is healthy" — a stuck
primary that holds open TCP connections is a common failure mode
(GC pause, kernel stall, debugger attach) and the cluster must
fail it over correctly.
"""
from __future__ import annotations

import time

import pytest

from cluster import BACKUP, PRIMARY, Cluster

HEARTBEAT_TIMEOUT_MS = 500
SLACK_MS = 1500
DETECT_BUDGET_MS = HEARTBEAT_TIMEOUT_MS + SLACK_MS
RECOVERY_BUDGET_S = 3.0


def test_pause_unpause_recovery(cluster: Cluster) -> None:
    cluster.wait_until(
        lambda: (cluster.replication(BACKUP) or {}).get("peerAlive") is True,
        timeout_s=5.0,
        what="pre-pause peerAlive=true",
    )

    cluster.pause(PRIMARY)
    try:
        elapsed = cluster.wait_until(
            lambda: (cluster.replication(BACKUP) or {}).get("peerAlive") is False,
            timeout_s=DETECT_BUDGET_MS / 1000.0,
            what="backup detects paused primary",
        )
        print(
            f"\n[pause] backup detected paused primary in {elapsed*1000:.0f}ms "
            f"(budget {DETECT_BUDGET_MS}ms)"
        )
    finally:
        # Always unpause — leaving the primary frozen would break
        # subsequent tests in this session.
        cluster.unpause(PRIMARY)

    # Recovery: peerAlive should flip back to true once heartbeats
    # resume (a few intervals at 100ms each).
    elapsed = cluster.wait_until(
        lambda: (cluster.replication(BACKUP) or {}).get("peerAlive") is True,
        timeout_s=RECOVERY_BUDGET_S,
        what="bilateral peerAlive=true after unpause",
    )
    print(f"[pause] peerAlive recovered in {elapsed*1000:.0f}ms after unpause")
