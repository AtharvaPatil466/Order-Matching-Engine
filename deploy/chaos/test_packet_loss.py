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

Measurement caveat: packet_loss() installs netem on `dev eth0 root`,
i.e. the container's WHOLE egress — which carries the admin HTTP
replies this test observes through, not just the replication link.
So a sample can be lost because the *instrument* got dropped while
the replication link is perfectly healthy. Those samples are counted
as `unreachable` and excluded from the flap statistic; only a
successful reply reporting peerAlive != true is a real flap. In
practice the admin channel has held up (run 30751470960 saw 0
unreachable of 16), so this is a guard against a miscount, not a
correction for one we have observed.

MEASURED, and worth acting on: at 30% loss the flap rate is ~37%
(6/16, run 30751470960) and was 60% (3/5) in the 2026-08-02 failure.
The ~2.7% above is the design estimate; the gap between it and 37% is
exactly the heartbeat-tuning signal described below, and it has not
been chased down. The suite is green because 37% is a minority of
samples and this test's contract is "not sustained degradation" — do
not read that green as the protocol matching its design margin.

Sampling is ~1.3/s, not the 10/s the 100ms sleep suggests: each
iteration makes four sequential admin round-trips. That is why the
window is seconds long rather than milliseconds — the flap ceiling is
a fraction of the sample count, so too few samples made the effective
ceiling both tighter than intended and statistically meaningless.
"""
from __future__ import annotations

import time

import pytest

from cluster import BACKUP, PRIMARY, Cluster

LOSS_PCT = 30
# Under loss the admin channel is slow (TCP retransmits), so wall-clock
# time buys far fewer samples than the 100ms tick suggests — the CI run
# that prompted this managed ~1.25/s, i.e. 5 samples in 4s. Observe
# longer so the flap statistic has some power behind it.
OBSERVE_S = 12.0
# Below this many *successful* bilateral reads the run tells us nothing;
# fail loudly rather than pass on an empty sample. Deliberately well under
# the ~15 samples OBSERVE_S buys: this is a guard against a vacuous pass,
# not a second throughput bar for the admin channel to clear.
MIN_OBSERVATIONS = 5


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
    observed = 0
    unreachable = 0
    flap_count = 0
    while time.monotonic() < deadline:
        backup_repl = cluster.replication(BACKUP)
        primary_repl = cluster.replication(PRIMARY)
        if backup_repl is None or primary_repl is None:
            # The admin reply itself was dropped by our own netem rule —
            # that says nothing about the replication link. Don't score it.
            unreachable += 1
        else:
            observed += 1
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
        f"\n[packet_loss] {LOSS_PCT}% loss: {samples} samples "
        f"({observed} observed, {unreachable} admin-unreachable), "
        f"{flap_count} peerAlive flaps observed"
    )
    # An all-unreachable window would otherwise sail through the flap
    # check with flap_count == 0. Demand real evidence before passing.
    assert observed >= MIN_OBSERVATIONS, (
        f"only {observed} successful bilateral reads in {OBSERVE_S}s "
        f"({unreachable} unreachable of {samples} samples) — the admin "
        f"channel was too degraded to judge the replication link"
    )
    # Allow transient peerAlive flaps — at 30% loss, the probability
    # of 3 consecutive heartbeat drops in any 500ms window is ~2.7%,
    # so we expect ~1 flap on average with high variance. Bound the flap
    # count to "minority" of the samples that actually reported — the real
    # protocol failure mode is sustained degradation, not occasional
    # false negatives.
    max_flaps = max(2, observed // 2)
    assert flap_count <= max_flaps, (
        f"too many flaps ({flap_count}/{observed} observed) under "
        f"{LOSS_PCT}% loss — heartbeat tuning likely needed"
    )
