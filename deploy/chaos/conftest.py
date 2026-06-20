"""
pytest fixtures for the chaos suite.

`cluster` fixture spins up the chaos topology once per test session and
heals network rules between tests. Container lifecycle (up/down) is
deliberately handled outside pytest so a developer iterating on a
single scenario doesn't pay the ~20s startup on every run:

    docker compose -f deploy/chaos/docker-compose.chaos.yml up -d --build
    pytest deploy/chaos/ -v

If the topology isn't up, fixtures will fail fast with a clear error.
"""
from __future__ import annotations

import pytest

from cluster import BACKUP, PRIMARY, Cluster


@pytest.fixture(scope="session")
def cluster() -> Cluster:
    c = Cluster.connect()
    import docker as docker_module
    # Check existence + collect "needs restart" set.
    any_dead = False
    for name in (PRIMARY, BACKUP):
        try:
            container = c._container(name)
        except docker_module.errors.NotFound:
            pytest.fail(
                f"Container '{name}' does not exist. Bring the topology up first:\n"
                f"    docker compose -f deploy/chaos/docker-compose.chaos.yml up -d --build"
            )
        if container.status != "running":
            any_dead = True

    # Detect a stale split-brain state left over from a prior pytest
    # session: if either container has been restarted by docker but
    # the cluster ended in a split-brain (e.g. committed-loss test
    # killed primary, backup promoted, primary came back fresh and
    # cannot demote the backup), the next session would inherit it.
    # Force a clean restart of BOTH whenever any container needs
    # restarting OR the live state already shows split brain.
    needs_reset = any_dead
    if not needs_reset:
        for name in (PRIMARY, BACKUP):
            c.wait_until(lambda n=name: c.health(n), timeout_s=10.0, what=f"{name} /health")
        leaders = [
            name for name in (PRIMARY, BACKUP)
            if (r := c.role(name)) and r.get("isLeader")
        ]
        if len(leaders) != 1 or leaders[0] != PRIMARY:
            needs_reset = True

    if needs_reset:
        for name in (PRIMARY, BACKUP):
            try:
                c.stop(name, timeout=3)
            except docker_module.errors.APIError:
                pass
            c.start(name)
    # Final readiness gate after any restart.
    for name in (PRIMARY, BACKUP):
        c.wait_until(lambda n=name: c.health(n), timeout_s=30.0, what=f"{name} /health")
    return c


@pytest.fixture(autouse=True)
def reset_between_tests(cluster: Cluster):
    """
    Before each test: ensure both nodes are running, the network is
    clean, AND bilateral replication is established. After each
    test: heal network rules.

    Auto-reconnect lives in ReplicationTransport now, so the backup
    re-establishes its TCP connection on its own once the primary
    is reachable. The fixture's job here is just to (a) restart any
    dead containers from a prior kill-scenario, (b) heal network
    rules, (c) wait for bilateral connectivity before yielding.
    """
    for name in (PRIMARY, BACKUP):
        if not cluster.is_running(name):
            cluster.start(name)
            cluster.wait_until(
                lambda n=name: cluster.health(n),
                timeout_s=15.0,
                what=f"{name} /health after restart",
            )

    cluster.heal()

    cluster.wait_until(
        lambda: (cluster.replication(PRIMARY) or {}).get("peerAlive") is True
                and (cluster.replication(BACKUP) or {}).get("peerAlive") is True,
        timeout_s=10.0,
        what="bilateral peerAlive=true (transport auto-reconnect)",
    )

    yield
    cluster.heal()
