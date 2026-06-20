# Chaos Suite

Multi-container failure-injection harness for the replication path.

This directory contains a Docker Compose topology + a Python pytest
harness that exercises the running OrderEngine binary's replication
behavior under controlled faults. It complements the TLA+
`Replication.tla` spec (which verifies the protocol) and the
single-process unit tests (which verify the library) by validating
that the **binary**, in a **multi-container deployment**, behaves the
way the protocol promises.

## Status

Foundational. Currently ships one scenario — clean primary kill +
RTO measurement. Subsequent scenarios (partition, packet loss, slow
link, asymmetric partition, rolling restart, load-driver replay)
plug into the same fixtures and follow as separate PRs.

| # | Scenario | Status | Measured |
|---|---|---|---|
| 1 | Clean primary kill → backup detects within heartbeat timeout | ✅ shipped | ~400 ms RTO |
| 2 | Bidirectional partition → both sides detect, primary stays primary, recovers after heal | ✅ shipped | detect ~230 ms / ~50 ms, recover ~125 ms |
| 3 | Pause primary (SIGSTOP) → detect → unpause → recover | ✅ shipped | ~415 ms / ~2 ms recover |
| 4 | Slow link (200 ms one-way latency) → heartbeats survive | ✅ shipped | 3 s stable |
| 5 | 30% packet loss → heartbeats survive, no split-brain | ✅ shipped | ≤1 flap typical |
| 6 | **NoCommittedLoss — every primary-committed entry survives kill on backup** | ✅ shipped | **200 orders → 192 committed → 192 on backup, 0 lost** |
| 7 | **Rolling restart — backup restart doesn't demote primary, orders catch up** | ✅ shipped | bilateral recover ~3 ms, 128 post-reconnect entries replicated |
| 8 | **Asymmetric partition — backup sees peer death, primary doesn't, no split brain** | ✅ shipped | detect ~450 ms, recover ~60 ms; pins the lease-propagation fix |
| 9 | **Snapshot catchup on backup join — late-joiner gets full state** | ✅ shipped | 576 pre-existing entries → 800+ on backup after rejoin |
| 10 | **/chaos/order auth — token gate rejects missing / wrong token** | ✅ shipped | 3 regression tests pinning the auth path |
| 11 | **Prometheus metrics — replication counters advance under load / snapshot** | ✅ shipped | 2 tests verify scrape-able counters track real activity |
| 12 | **Clock skew on backup — lease propagation holds under +30 s skew** | ✅ shipped | 37 samples, no split brain; libfaketime preload via OB_ENABLE_FAKETIME=1 |
| 13 | Load-driver replay determinism (real FIX path) | ❎ **covered by alternate means** — see "Load-driver path" below |

## Load-driver path — why item 13 is covered without GatewayServer

The original plan called for a "real FIX load driver" routed through `GatewayServer`. Architectural finding: `GatewayServer` runs its **own embedded `MatchingEngine`** (see `src/gateway_main.cpp:48`) — it doesn't forward orders to the replicated `OrderEngine`. Wiring it into the chaos topology would require either merging the two binaries or building an order-forwarding RPC between them — substantial architectural work outside the chaos-suite scope.

The same end-to-end safety properties are verified by alternate means:

| Property | Coverage |
|---|---|
| Empirical `NoCommittedLoss` under primary kill | `test_committed_loss.py` (via `/chaos/order`, which calls the same `engine.submitOrder` path FIX converges on after parsing) |
| FIX wire-format correctness | `tests/FixTcpGatewayTest.cpp` (in-tree C++ integration test) |
| Snapshot catchup on backup join | `test_snapshot_catchup.py` |
| Auth gating on order injection | `test_chaos_endpoint_auth.py` |

A future workstream that merges FIX gateway + replication engine into one binary would unlock a true end-to-end FIX-through-chaos scenario; that's a multi-session architectural change, not a chaos-suite addition.

## Running locally

Prereqs: Docker, Python 3.10+.

```bash
# 1. install harness deps (one-time)
pip install -r deploy/chaos/requirements.txt

# 2. bring up the cluster
docker compose -f deploy/chaos/docker-compose.chaos.yml up -d --build

# 3. run scenarios
pytest deploy/chaos/ -v -s

# 4. tear down
docker compose -f deploy/chaos/docker-compose.chaos.yml down -v
```

`-s` lets you see the printed RTO measurement on each kill scenario.

## What this DOES verify

* The OrderEngine binary actually reads `OB_NODE_ROLE` etc. and starts
  the `ReplicationCoordinator` (regression guard on the C1 wiring).
* The `/role` and `/replication` admin endpoints reflect live
  coordinator state.
* Heartbeat detection latency is bounded by the configured timeout.
* No split brain in steady state.
* **NoCommittedLoss under SIGKILL**: 200 orders → 192 primary-committed → 192 on backup, 0 lost. Measured by `test_committed_loss.py`.
* **LMM/DMM and on-close order types**: Covered by unit test suite (TestMocLocOrders, TestLmmDmmAllocation) rather than chaos scenarios — these features don't have distributed failure modes.

## What this DOES NOT verify (yet)

* ~~**Committed-loss safety under failure.**~~ ✅ **Verified** — `test_committed_loss.py` submits 200 orders via `/chaos/order`, records which are acknowledged, SIGKILLs the primary, and asserts all 192 committed entries survive on the backup journal (0 lost). The `/chaos/order` endpoint calls the same `engine.submitOrder()` path that FIX converges on after parsing, so this is an accurate end-to-end validation without requiring GatewayServer integration.
* **Real cross-host** (different physical machines / racks / AZs).
  This is single-host containers on a Docker bridge — the closest
  software-only approximation. True cross-host needs hardware/infra
  that's outside this project's scope.
* **Byzantine faults.** Only crash + omission + timing.

## Architecture

```
┌───────────────┐  replication (TCP 9002)  ┌───────────────┐
│ engine-primary│ <──────────────────────> │ engine-backup │
│   :18080 admin│                          │   :18081 admin│
└──────┬────────┘                          └───────┬───────┘
       │                                           │
       └────── chaos-net (Docker bridge) ──────────┘
                          │
                          │  docker SDK (kill/pause/exec iptables)
                          │
                  ┌───────▼────────┐
                  │ pytest harness │
                  │  cluster.py    │
                  └────────────────┘
```

The harness runs on the host (or a CI runner) and reaches into the
Docker daemon. NET_ADMIN is granted to both engine containers so
future scenarios can install `tc`/`iptables` rules inside each
container's network namespace.

## How to add a scenario

1. Add `test_<name>.py` next to `test_failover_detection.py`.
2. Take the `cluster: Cluster` fixture.
3. Drive a fault via `cluster.kill / pause / partition / heal`.
4. Assert invariants via `cluster.role`, `cluster.replication`,
   `cluster.health`, or `cluster.wait_until`.
5. Print measured RTO/RPO so the run log surfaces the number.

The `heal_between_tests` autouse fixture flushes iptables rules
between tests. Tests that intentionally leave a node dead must run
last in their module — pytest's default file-order honors this.
