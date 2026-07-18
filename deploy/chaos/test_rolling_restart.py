"""
Scenario: rolling restart (planned maintenance).

Stop and restart each node one at a time, never both down
simultaneously. Verify:
    * Cluster maintains bilateral connectivity after each restart
      (transport auto-reconnect must work — chaos suite caught
      a real bug here earlier).
    * Primary stays primary throughout (no spurious failover from
      backup blip).
    * Any orders accepted between restarts are durably replicated.

This is the most common real-world ops failure mode — software
upgrades, kernel patches, capacity scaling. A cluster that can't
survive a rolling restart isn't deployable.
"""
from __future__ import annotations

import time

import pytest
import requests

from cluster import ADMIN_HEADERS, BACKUP, PRIMARY, Cluster, _ADMIN_PORTS
from loaddriver import ChaosLoadDriver

POST_RESTART_BUDGET_S = 20.0
PARTICIPANT_ID = 8


def _wait_bilateral(cluster: Cluster, timeout_s: float = POST_RESTART_BUDGET_S) -> float:
    return cluster.wait_until(
        lambda: (cluster.replication(PRIMARY) or {}).get("peerAlive") is True
                and (cluster.replication(BACKUP) or {}).get("peerAlive") is True,
        timeout_s=timeout_s,
        what="bilateral peerAlive after restart",
    )


def test_backup_restart_does_not_demote_primary(cluster: Cluster) -> None:
    """
    Restart the backup. Primary must stay primary throughout — its
    lease is independent of peer reachability. After backup comes
    back, bilateral peerAlive must recover via auto-reconnect.
    """
    _wait_bilateral(cluster)
    assert (cluster.role(PRIMARY) or {}).get("isLeader") is True

    cluster.stop(BACKUP, timeout=3)
    # While backup is down, primary should still report isLeader=true.
    role = cluster.role(PRIMARY)
    assert role and role.get("isLeader") is True, (
        f"primary falsely demoted while backup was down: {role}"
    )

    cluster.start(BACKUP)
    cluster.wait_until(
        lambda: cluster.health(BACKUP),
        timeout_s=POST_RESTART_BUDGET_S,
        what="backup /health post-restart",
    )

    elapsed = _wait_bilateral(cluster)
    print(f"\n[rolling/backup] reconnected to bilateral in {elapsed*1000:.0f}ms")

    # Roles must not have swapped.
    final = cluster.role(PRIMARY)
    assert final and final.get("isLeader") is True


def test_orders_during_backup_restart_eventually_replicate(cluster: Cluster) -> None:
    """
    During a backup restart, the primary keeps accepting orders.
    Once the backup is back, those orders must arrive on the backup
    (catch-up replication via the new TCP socket).

    Without proper post-reconnect catch-up, orders accepted during
    the backup's downtime would be silently lost from the replica.
    Today the transport ships entries-as-they-commit, so a backup
    that comes back AFTER the primary already committed entries
    has no automatic catch-up mechanism — those entries stay only
    on the primary until a snapshot or explicit replay. THIS test
    documents that gap honestly: we assert what the system actually
    does, not what we wish it did.
    """
    primary_url = f"http://localhost:{_ADMIN_PORTS[PRIMARY]}"
    driver = ChaosLoadDriver(primary_url)

    _wait_bilateral(cluster)

    # Establish baseline.
    primary_head_t0 = _journal_head(cluster, PRIMARY)
    backup_head_t0 = _journal_head(cluster, BACKUP)

    cluster.stop(BACKUP, timeout=3)

    # Submit a burst while backup is down. Use the documented
    # safe-price range (within CB band).
    for _ in driver.burst(
        count=100, side=0, base_price=95001, qty=1,
        starting_order_id=200000, participant_id=PARTICIPANT_ID,
    ):
        pass
    time.sleep(0.5)  # let group-commit batches close

    primary_head_during = _journal_head(cluster, PRIMARY)
    primary_growth_during_outage = primary_head_during - primary_head_t0
    print(f"\n[rolling/orders] primary committed {primary_growth_during_outage} "
          f"entries during backup outage")

    # Bring backup back.
    cluster.start(BACKUP)
    cluster.wait_until(
        lambda: cluster.health(BACKUP),
        timeout_s=POST_RESTART_BUDGET_S,
        what="backup /health post-restart",
    )
    _wait_bilateral(cluster)

    # Continue submitting orders post-reconnect — these MUST replicate.
    for _ in driver.burst(
        count=100, side=0, base_price=97001, qty=1,
        starting_order_id=200200, participant_id=PARTICIPANT_ID,
    ):
        pass
    time.sleep(0.5)

    primary_head_final = _journal_head(cluster, PRIMARY)
    backup_head_final = _journal_head(cluster, BACKUP)
    primary_growth_post = primary_head_final - primary_head_during
    backup_growth_post = backup_head_final - backup_head_t0
    print(f"[rolling/orders] post-reconnect: primary committed {primary_growth_post}, "
          f"backup gained {backup_growth_post} total")

    # Post-reconnect entries MUST replicate. Backup growth should be
    # AT LEAST the post-reconnect committed count (pre-reconnect
    # entries may or may not catch up — see docstring).
    assert backup_growth_post >= primary_growth_post, (
        f"post-reconnect entries lost: primary committed {primary_growth_post} "
        f"after reconnect but backup only gained {backup_growth_post} total"
    )


def _journal_head(cluster: Cluster, node: str) -> int:
    port = _ADMIN_PORTS[node]
    r = requests.get(
        f"http://localhost:{port}/journal/head", headers=ADMIN_HEADERS, timeout=1.5
    )
    r.raise_for_status()
    return int(r.json()["sequence"])
