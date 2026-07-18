"""
Scenario: snapshot catchup on backup join.

Closes the gap surfaced by the rolling-restart scenario: a backup
that joins AFTER the primary already has resting orders must be
brought up to date, not just receive live entries from that point
onward.

Setup:
    * Stop the backup (so primary keeps running solo).
    * Drive a burst of orders into the primary. These are resting
      orders the backup has no knowledge of.
    * Start the backup. The primary's `setOnPeerJoined` hook
      streams every currently-resting order as Snapshot entries.
    * Verify the backup's journal head reflects the snapshot.

Invariant: after the backup has reconnected and bilateral peer-alive
is restored, the backup's journal must contain at least the pre-
existing orders that were resting at the moment of reconnect.
"""
from __future__ import annotations

import time

import pytest
import requests

from cluster import ADMIN_HEADERS, BACKUP, PRIMARY, Cluster, _ADMIN_PORTS
from loaddriver import ChaosLoadDriver

PARTICIPANT_ID = 9


def _journal_head(cluster: Cluster, node: str) -> int:
    port = _ADMIN_PORTS[node]
    r = requests.get(
        f"http://localhost:{port}/journal/head", headers=ADMIN_HEADERS, timeout=1.5
    )
    r.raise_for_status()
    return int(r.json()["sequence"])


def test_backup_joining_late_catches_up_via_snapshot(cluster: Cluster) -> None:
    primary_url = f"http://localhost:{_ADMIN_PORTS[PRIMARY]}"
    driver = ChaosLoadDriver(primary_url)

    # Pre-condition: bilateral steady state.
    cluster.wait_until(
        lambda: (cluster.replication(PRIMARY) or {}).get("peerAlive") is True
                and (cluster.replication(BACKUP) or {}).get("peerAlive") is True,
        timeout_s=5.0,
        what="pre-test bilateral peerAlive",
    )

    # Stop the backup. Primary keeps running.
    cluster.stop(BACKUP, timeout=3)

    # Drive a burst of orders into the primary while the backup is down.
    # These are the orders the snapshot must catch up.
    BURST = 200
    for _ in driver.burst(
        count=BURST, side=0, base_price=95001, qty=1,
        starting_order_id=300000, participant_id=PARTICIPANT_ID,
    ):
        pass
    time.sleep(0.5)  # let group-commit batches close

    primary_head_after_burst = _journal_head(cluster, PRIMARY)
    print(f"\n[catchup] primary committed {primary_head_after_burst} entries while backup was down")

    # Start the backup. Primary should stream snapshot.
    cluster.start(BACKUP)
    cluster.wait_until(
        lambda: cluster.health(BACKUP),
        timeout_s=20.0,
        what="backup /health post-restart",
    )

    # Wait for snapshot stream + bilateral connectivity to settle.
    cluster.wait_until(
        lambda: (cluster.replication(BACKUP) or {}).get("peerAlive") is True
                and (cluster.replication(PRIMARY) or {}).get("peerAlive") is True,
        timeout_s=10.0,
        what="bilateral peerAlive after backup join",
    )
    # Snapshot streaming runs sync on the receive thread, but the
    # backup's journal commits in batches of 64 — wait long enough
    # for the final partial batch to flush.
    time.sleep(1.0)

    backup_head_after_join = _journal_head(cluster, BACKUP)
    print(f"[catchup] backup journal head after join + snapshot: {backup_head_after_join}")

    # The snapshot must have shipped at least `BURST/batchSize*batchSize`
    # committed entries (group commit at 64 means tail batch may not have
    # flushed). With BURST=200, we expect ≥ 192 entries on the backup.
    expected_min = (BURST // 64) * 64
    assert backup_head_after_join >= expected_min, (
        f"snapshot catchup didn't bring backup up to date: "
        f"backup={backup_head_after_join}, expected ≥ {expected_min} "
        f"(primary's head was {primary_head_after_burst})"
    )
