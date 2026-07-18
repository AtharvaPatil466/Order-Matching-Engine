"""
Cluster — minimal control surface for the chaos topology.

Wraps the docker SDK and the engine's admin HTTP so scenarios don't
have to reimplement "kill the primary and wait for the backup to
notice." Designed for the 1-primary + 1-backup topology declared in
docker-compose.chaos.yml.

Failure-mode primitives currently exposed:
    kill(node)         hard SIGKILL via docker kill
    pause(node)        SIGSTOP-equivalent (process frozen but TCP stays open)
    unpause(node)
    start(node)
    stop(node)
    partition(a, b)    iptables DROP on the a→b path (requires NET_ADMIN)
    heal()             clear all chaos-installed iptables rules

State observation:
    role(node)         GET /role → primary | backup | standalone
    replication(node)  GET /replication → {peerAlive, epoch, ...}
    health(node)       GET /health → bool
    wait_until(...)    poll a condition with timeout (RTO measurement)
"""
from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Callable, Dict, Optional

import docker
import requests


# Names must match container_name in docker-compose.chaos.yml.
PRIMARY = "chaos-engine-primary"
BACKUP = "chaos-engine-backup"

# Shared-secret bearer token for the admin API. Must match
# OB_ADMIN_TOKEN in docker-compose.chaos.yml. Test-topology-only
# secret (same pattern as OB_CHAOS_TOKEN).
ADMIN_TOKEN = "admin-suite-shared-secret"
ADMIN_HEADERS = {"Authorization": f"Bearer {ADMIN_TOKEN}"}

# Host-mapped admin ports (also from compose).
_ADMIN_PORTS = {
    PRIMARY: 18080,
    BACKUP: 18081,
}


@dataclass
class Cluster:
    client: docker.DockerClient

    @classmethod
    def connect(cls) -> "Cluster":
        return cls(client=docker.from_env())

    # ── container lifecycle ──────────────────────────────────────────

    def _container(self, name: str):
        return self.client.containers.get(name)

    def kill(self, name: str, signal: str = "SIGKILL") -> None:
        """Hard-kill a node. Simulates a process crash."""
        self._container(name).kill(signal=signal)

    def pause(self, name: str) -> None:
        """Freeze the node (SIGSTOP). TCP connections stay alive but no progress."""
        self._container(name).pause()

    def unpause(self, name: str) -> None:
        self._container(name).unpause()

    def stop(self, name: str, timeout: int = 5) -> None:
        self._container(name).stop(timeout=timeout)

    def start(self, name: str) -> None:
        self._container(name).start()

    def is_running(self, name: str) -> bool:
        try:
            return self._container(name).status == "running"
        except docker.errors.NotFound:
            return False

    # ── network manipulation ─────────────────────────────────────────

    def partition(self, src: str, dst: str) -> None:
        """
        Block traffic from `src` to `dst`. Implemented as an iptables
        OUTPUT DROP rule inside the src container's netns. `dst` is
        a compose network alias (e.g. "primary" / "backup"); it is
        resolved to an IP inside the src container before being
        passed to iptables — iptables itself doesn't use Docker's
        embedded DNS so a raw hostname like "backup" would fail.
        Requires NET_ADMIN cap (compose file grants it) and iptables
        in the image (Dockerfile installs it).
        """
        c = self._container(src)
        # Resolve alias → IP via getent inside the container. getent
        # uses the container's /etc/resolv.conf which points at
        # Docker's embedded DNS; iptables does not.
        rc, out = c.exec_run(f"getent hosts {dst}")
        if rc != 0:
            raise RuntimeError(
                f"partition({src}→{dst}) DNS resolution failed: {out!r}"
            )
        # "172.18.0.3       chaos-engine-backup" — first whitespace token.
        dst_ip = out.decode().split()[0]
        rule = f"iptables -A OUTPUT -d {dst_ip} -j DROP"
        rc, out = c.exec_run(rule)
        if rc != 0:
            raise RuntimeError(
                f"partition({src}→{dst} [{dst_ip}]) failed (exit {rc}): {out!r}"
            )

    def heal(self) -> None:
        """
        Clear all chaos-installed perturbations on both engines:
        iptables rules, tc qdiscs, faketime offsets. Best-effort:
        skips nodes that aren't running and tolerates missing tools
        (older images don't ship iptables/tc/faketime).
        """
        for name in (PRIMARY, BACKUP):
            try:
                if not self.is_running(name):
                    continue
                c = self._container(name)
                c.exec_run("iptables -F OUTPUT")
                c.exec_run("tc qdisc del dev eth0 root")
                c.exec_run("sh -c \": > /etc/faketimerc\"")
            except (docker.errors.NotFound, docker.errors.APIError):
                pass

    def slow_link(self, name: str, delay_ms: int) -> None:
        """
        Add `delay_ms` of one-way latency on the named container's
        egress (eth0). Implemented via `tc qdisc add ... netem delay`.
        Requires iproute2 in the image; if missing this raises with
        the same hint format as partition().
        """
        rule = f"tc qdisc add dev eth0 root netem delay {delay_ms}ms"
        exit_code, output = self._container(name).exec_run(rule)
        if exit_code != 0:
            raise RuntimeError(
                f"slow_link({name}, {delay_ms}ms) failed (exit {exit_code}): {output!r}\n"
                f"Hint: ensure iproute2 (tc) is installed in the engine image\n"
                f"      and the container has NET_ADMIN cap."
            )

    def packet_loss(self, name: str, loss_pct: float) -> None:
        """Add `loss_pct`% random packet drop on the named container's egress."""
        rule = f"tc qdisc add dev eth0 root netem loss {loss_pct}%"
        exit_code, output = self._container(name).exec_run(rule)
        if exit_code != 0:
            raise RuntimeError(
                f"packet_loss({name}, {loss_pct}%) failed (exit {exit_code}): {output!r}"
            )

    def set_faketime(self, name: str, offset_seconds: int) -> None:
        """
        Skew the container's perceived time by `offset_seconds`. Requires
        OB_ENABLE_FAKETIME=1 in the container's env (the chaos compose
        sets this on the backup). Writes the FAKETIME spec to
        /etc/faketimerc; libfaketime's FAKETIME_NO_CACHE=1 mode picks
        it up on the next time-syscall.

        Affects both wall-clock AND monotonic clocks by default — the
        engine uses steady_clock for heartbeats/lease, so a +N seconds
        skew makes the backup believe N more seconds have passed since
        its last LeaseGrant (testing the fencing under clock perturbation).
        """
        sign = "+" if offset_seconds >= 0 else "-"
        magnitude = abs(offset_seconds)
        spec = f"{sign}{magnitude}s"
        # sh -c to redirect; bare echo wouldn't survive the exec_run quoting.
        exit_code, output = self._container(name).exec_run(
            f"sh -c \"echo '{spec}' > /etc/faketimerc\""
        )
        if exit_code != 0:
            raise RuntimeError(
                f"set_faketime({name}, {offset_seconds}s) failed (exit {exit_code}): {output!r}\n"
                f"Hint: ensure OB_ENABLE_FAKETIME=1 was set when the container started."
            )

    def reset_faketime(self, name: str) -> None:
        """Restore real time on the named container."""
        try:
            if not self.is_running(name):
                return
            self._container(name).exec_run("sh -c \": > /etc/faketimerc\"")
        except (docker.errors.NotFound, docker.errors.APIError):
            pass

    # ── state observation via admin HTTP ─────────────────────────────

    def _admin_url(self, name: str, path: str) -> str:
        port = _ADMIN_PORTS[name]
        return f"http://localhost:{port}{path}"

    def role(self, name: str) -> Optional[Dict]:
        return self._get_json(name, "/role")

    def replication(self, name: str) -> Optional[Dict]:
        return self._get_json(name, "/replication")

    def health(self, name: str) -> bool:
        body = self._get_json(name, "/health")
        return bool(body and body.get("status") == "ok")

    def _get_json(self, name: str, path: str) -> Optional[Dict]:
        try:
            r = requests.get(
                self._admin_url(name, path), headers=ADMIN_HEADERS, timeout=1.5
            )
            if r.status_code != 200:
                return None
            return r.json()
        except (requests.RequestException, ValueError):
            return None

    # ── polling helper for RTO/timeout measurement ──────────────────

    def wait_until(
        self,
        predicate: Callable[[], bool],
        *,
        timeout_s: float = 10.0,
        interval_s: float = 0.05,
        what: str = "condition",
    ) -> float:
        """
        Poll `predicate` until it returns True. Returns elapsed seconds
        on success; raises TimeoutError otherwise. The elapsed time IS
        the RTO measurement — that's the whole reason this is its own
        method instead of a raw loop in each test.
        """
        start = time.monotonic()
        deadline = start + timeout_s
        while time.monotonic() < deadline:
            try:
                if predicate():
                    return time.monotonic() - start
            except Exception:
                pass
            time.sleep(interval_s)
        raise TimeoutError(
            f"timed out after {timeout_s}s waiting for {what}"
        )
