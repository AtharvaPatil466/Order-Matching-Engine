"""
Scenario: backup's clock jumps forward — verify no false promotion.

libfaketime preload + dynamic /etc/faketimerc lets us advance the
backup's perceived time without restarting the container. The engine
uses C++ `steady_clock` for heartbeat / lease timers, which
libfaketime also accelerates by default — so a +N seconds skew makes
the backup believe N seconds have elapsed since its last LeaseGrant.

Expected behavior under the lease-propagation fix:
    * After a forward skew of >LeaseDuration on the backup, the
      backup's local view of the primary's lease appears expired.
    * The backup's failure callback fires; tryAcquire() is called.
    * In real time, the primary is still sending LeaseGrants
      every heartbeat tick (100 ms). Within <100 ms of real time,
      a new LeaseGrant arrives and refreshes the backup's local
      lease — pushing it forward by leaseDurationMs.
    * Backup's tryAcquire sees the refreshed lease and refuses
      to promote.
    * No split brain.

This is the strongest test of the lease-propagation mechanism so
far: it perturbs the backup's local clock — which is the precise
attack surface that lease fencing is designed to handle.
"""
from __future__ import annotations

import time

import pytest

from cluster import BACKUP, PRIMARY, Cluster


def test_backup_clock_skew_does_not_split_brain(cluster: Cluster) -> None:
    cluster.wait_until(
        lambda: (cluster.replication(PRIMARY) or {}).get("peerAlive") is True
                and (cluster.replication(BACKUP) or {}).get("peerAlive") is True,
        timeout_s=5.0,
        what="pre-skew bilateral peerAlive",
    )

    # Skew backup forward by 30 seconds — well past the 5-second
    # default lease duration. If the lease-propagation fix is missing
    # or broken, the backup would observe a long-stale lease and
    # promote itself while the primary keeps operating.
    cluster.set_faketime(BACKUP, 30)
    print("\n[clock_skew] backup advanced by +30s")

    # Hold the perturbation long enough for at least a few heartbeat
    # ticks to arrive from primary at backup's "now+30s" timeline.
    # Sample roles repeatedly; assert no split brain at any moment.
    deadline = time.monotonic() + 4.0
    samples = 0
    while time.monotonic() < deadline:
        leaders = [
            n for n, r in (
                (PRIMARY, cluster.role(PRIMARY)),
                (BACKUP, cluster.role(BACKUP)),
            ) if r and r.get("isLeader")
        ]
        assert len(leaders) <= 1, (
            f"clock_skew triggered split-brain: {leaders} "
            f"(backup falsely promoted under skewed clock — "
            f"lease-propagation regression)"
        )
        # Primary should remain primary throughout.
        primary_role = cluster.role(PRIMARY)
        assert primary_role and primary_role.get("isLeader") is True, (
            f"primary falsely demoted under backup clock skew: {primary_role}"
        )
        samples += 1
        time.sleep(0.1)
    print(f"[clock_skew] held +30s skew for 4s, {samples} samples, no split brain")

    # Heal and verify the cluster returns to clean bilateral state.
    cluster.reset_faketime(BACKUP)
    elapsed = cluster.wait_until(
        lambda: (cluster.replication(PRIMARY) or {}).get("peerAlive") is True
                and (cluster.replication(BACKUP) or {}).get("peerAlive") is True,
        timeout_s=5.0,
        what="bilateral peerAlive after reset",
    )
    print(f"[clock_skew] recovered in {elapsed*1000:.0f}ms after reset_faketime")
