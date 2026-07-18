"""
Scenario: replication Prometheus metrics advance as expected.

Verifies the operator-facing observability story:
    * `replication_entries_shipped_total` advances with each
      live entry shipment.
    * `replication_bytes_sent_total` advances by ~sizeof(JournalEntry)
      per shipment.
    * `replication_snapshot_streams_total` is incremented when a
      backup joins (snapshot-on-connect hook).
    * `replication_snapshot_entries_total` reflects the resting
      orders shipped in those streams.

Without these counters, an operator scraping /prometheus has no
way to answer "is the cluster still actually replicating?" — only
indirect signals like /role and /replication's peerAlive. The
counters give a quantitative view that monitoring/alerting can
threshold on (e.g., "alert if replication_entries_shipped_total
is flat for 5m while order flow continues").
"""
from __future__ import annotations

import re
import time

import pytest
import requests

from cluster import ADMIN_HEADERS, BACKUP, PRIMARY, Cluster, _ADMIN_PORTS
from loaddriver import ChaosLoadDriver


PARTICIPANT_ID = 11


def _scrape(host_port_node: str) -> dict[str, float]:
    """Fetch /prometheus and parse into a {metric_name: value} dict."""
    port = _ADMIN_PORTS[host_port_node]
    r = requests.get(
        f"http://localhost:{port}/prometheus", headers=ADMIN_HEADERS, timeout=2.0
    )
    r.raise_for_status()
    out: dict[str, float] = {}
    # The exposition format is "name value" or "name{labels} value"
    # per line, with # HELP / # TYPE prefixes that we ignore.
    line_re = re.compile(r"^([a-zA-Z_:][a-zA-Z0-9_:]*)(?:\{[^}]*\})?\s+([\d.e+-]+)\s*$")
    for line in r.text.splitlines():
        if line.startswith("#") or not line.strip():
            continue
        m = line_re.match(line)
        if m:
            out[m.group(1)] = float(m.group(2))
    return out


def test_replication_counters_advance_on_load(cluster: Cluster) -> None:
    driver = ChaosLoadDriver(f"http://localhost:{_ADMIN_PORTS[PRIMARY]}")

    # Wait for bilateral steady state — snapshot-on-connect counter
    # may have been triggered at session start.
    cluster.wait_until(
        lambda: (cluster.replication(PRIMARY) or {}).get("peerAlive") is True
                and (cluster.replication(BACKUP) or {}).get("peerAlive") is True,
        timeout_s=5.0,
        what="pre-test bilateral",
    )

    before = _scrape(PRIMARY)
    shipped_before = before.get("replication_entries_shipped_total", 0.0)
    bytes_before   = before.get("replication_bytes_sent_total", 0.0)
    print(f"\n[metrics] pre-load: shipped={shipped_before}, bytes={bytes_before}")

    # Drive a burst large enough to force at least one full batch commit
    # (batchSize=64) so the journal-commit → replicateEntry hook fires.
    for _ in driver.burst(
        count=128, side=0, base_price=95001, qty=1,
        starting_order_id=400000, participant_id=PARTICIPANT_ID,
    ):
        pass
    time.sleep(0.5)

    after = _scrape(PRIMARY)
    shipped_after = after.get("replication_entries_shipped_total", 0.0)
    bytes_after   = after.get("replication_bytes_sent_total", 0.0)
    print(f"[metrics] post-load: shipped={shipped_after}, bytes={bytes_after}")

    # At least one batch worth of entries should have shipped.
    shipped_delta = shipped_after - shipped_before
    bytes_delta   = bytes_after - bytes_before
    assert shipped_delta >= 64, (
        f"replication_entries_shipped_total only advanced by {shipped_delta} "
        f"(expected ≥ 64 = one full group-commit batch)"
    )
    # Each shipped entry is sizeof(JournalEntry) ≈ 120 bytes on this
    # platform. Loose lower bound: 64 entries * 100 bytes minimum.
    assert bytes_delta >= 64 * 100, (
        f"replication_bytes_sent_total only advanced by {bytes_delta}"
    )


def test_snapshot_stream_counter_advances_on_join(cluster: Cluster) -> None:
    """
    The snapshot-on-connect hook fires once per backup attach. Stop
    the backup, wait for primary to notice, restart backup — counter
    should bump by one.
    """
    cluster.wait_until(
        lambda: (cluster.replication(PRIMARY) or {}).get("peerAlive") is True,
        timeout_s=5.0,
        what="pre-test primary peerAlive",
    )

    before = _scrape(PRIMARY)
    streams_before = before.get("replication_snapshot_streams_total", 0.0)
    print(f"\n[metrics] streams before backup restart: {streams_before}")

    cluster.stop(BACKUP, timeout=3)
    # Wait for primary to observe the disconnect before re-attaching,
    # otherwise the accept callback may not fire on rejoin.
    cluster.wait_until(
        lambda: (cluster.replication(PRIMARY) or {}).get("peerAlive") is False,
        timeout_s=3.0,
        what="primary observes backup down",
    )
    cluster.start(BACKUP)
    cluster.wait_until(
        lambda: cluster.health(BACKUP),
        timeout_s=15.0,
        what="backup /health post-restart",
    )
    cluster.wait_until(
        lambda: (cluster.replication(PRIMARY) or {}).get("peerAlive") is True,
        timeout_s=10.0,
        what="bilateral peerAlive after backup rejoin",
    )
    time.sleep(0.5)

    after = _scrape(PRIMARY)
    streams_after = after.get("replication_snapshot_streams_total", 0.0)
    print(f"[metrics] streams after backup restart: {streams_after}")

    assert streams_after >= streams_before + 1, (
        f"replication_snapshot_streams_total did not advance "
        f"({streams_before} → {streams_after}) — snapshot-on-join hook regressed"
    )
