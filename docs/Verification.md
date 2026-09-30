# TLA+ Verification Report

> **MatchingEngine.tla** (order book **+ matching/cross layer**): VERIFIED — 368,192,427 states generated, 171,187,419 distinct, **zero violations**, complete exploration · 2026-07-12 · `MatchingEngine4.cfg` (MaxOrders=4, MaxTime=2) · BFS depth 11 · 17m 21s · non-vacuity: `MatchingEngineBroken.cfg` (BROKEN_NO_FIFO=TRUE) reproduces a `FIFOExecution` violation, so the time-priority invariant is not vacuous · **scope:** no invariant checks price priority, `GTD_Expiry_Correctness` is vacuous (the model only places Limit orders), and `Quantity_Conservation` is not checked and is false (see below)  
> **Replication.tla** (lease-propagation model): **VACUOUS** — zero violations at both cfgs, but in both the backup can never promote, so `NoSplitBrain` and `NoCommittedLoss` hold trivially. `TickDegraded` stops advancing the timers once `heartbeatTimer` reaches `HeartbeatTimeout + 1`, the two timers are always equal, and `BackupPromote` needs `leaseTimer > LeaseTimeout`, which is larger (2 vs 5; 3 vs 7). The bug-injected variant (lease check stripped) is the only configuration in which promotion is reachable, and there it splits the brain — the spec has no primary step-down. No `ReplicationBroken` cfg is committed, and CI does not run this spec.

> **New specs (2026-05-28)**: Auction.tla · EpochDurability.tla · FixSession.tla · Oco.tla · Risk.tla — each verified with broken-variant sanity check (TLC finds violation in <1000 states)

## Model Configuration

```
SPECIFICATION Spec
CONSTANTS
    MaxOrders      = 4
    Participants   = {1, 2}
    Prices         = {100, 200}
    MaxQty         = 3
    MaxTime        = 2
    BROKEN_NO_FIFO = FALSE
```

MaxOrders=4 admits up to 3 resting orders on one side at a single price level
(place 3, cross with a 4th) — the regime where FIFO time-priority defects
manifest. MaxOrders=5 was attempted and is intractable on commodity hardware
(>228M distinct states with >200M still queued after ~20 min, no convergence);
4 is the largest exhaustively-checkable bound. The default `MatchingEngine.cfg`
runs MaxOrders=3 (1.26M distinct) for a fast exhaustive check; `MatchingEngine4.cfg`
is the deeper run reported here.

**Workers**: 11 (Apple Silicon M-series, auto-detected)  
**Memory**: 4096MB heap + 64MB offheap  
**Search**: Breadth-first, no symmetry reduction

## Verified Invariants

| Invariant | Description | Status |
|-----------|-------------|--------|
| `NoNegativeQuantity` | No order has negative `qty` or `remainingQty` | ✅ Verified |
| `FIFO_Preservation` | Orders at the same price maintain timestamp ordering | ✅ Verified |
| `MatchingConservation` | `placed = resting + filled + cancelled` across every order's lifecycle | ✅ Verified |
| `FIFOExecution` | A fill always consumes the earliest-timestamped resting order at a price (time priority within a price level; no invariant checks price priority) — **non-vacuous**, broken variant violates it | ✅ Verified |
| `GTD_Expiry_Correctness` | Expired GTD orders are always cancelled | ⚠️ Vacuous — `PlaceLimit` is the only order-creating action and it sets `type = "Limit"`, so no GTD order ever exists |

## State Space Coverage

```
368,192,427 states generated
171,187,419 distinct states found
11 levels deep (BFS depth)
0 states left on queue (complete exploration)
```

**Fingerprint collision probability**: ~0.18% (calculated, optimistic; 0.13% from
actual fingerprints) — acceptable for this state space size

### Which count comes from which `MaxOrders`

The state count is a function of the `MaxOrders` bound, so a bare "N states
verified" claim is ambiguous. Every count this repository publishes maps to
exactly one config:

| Config | `MaxOrders` | States generated | Distinct | Runtime | Role |
|--------|------------:|-----------------:|---------:|--------:|------|
| `spec/MatchingEngine.cfg`  | 3 | — | 1,260,000 (1.26M) | seconds | Fast exhaustive check. Not run by CI: `tla.yml` checks MatchingEngine and Replication "out-of-band" |
| `spec/MatchingEngine4.cfg` | 4 | **368,192,427** | **171,187,419** | 17m 21s | **Authoritative** — the deepest exhaustively-checkable bound, reported above and cited in README / BENCHMARKS / PerformanceWhitepaper |
| (attempted)                | 5 | — | >228M distinct with >200M still queued after ~20 min | no convergence | Intractable on commodity hardware |

**`MaxOrders=4` is the figure to cite.** MaxOrders counts orders *placed over
the whole behaviour*, not orders resting simultaneously: 4 admits up to 3
resting on one side at a single price plus a 4th that crosses them — the
smallest bound that exercises FIFO time-priority against a multi-order queue.
MaxOrders=3 cannot construct that interleaving, which is why the 1.26M figure
is a regression gate rather than the headline.

A superseded figure — 454,022,166 generated / 181,004,838 distinct, 12m 35s —
circulated in earlier revisions of README.md, BENCHMARKS.md,
PerformanceWhitepaper.md, Project_Overview.md, Architecture.md and
PRODUCTION_ROADMAP.md. It came from the pre-matching-layer version of the spec
(no `Match` action, so neither `MatchingConservation` nor `FIFOExecution` was
checked) and does not describe the spec in this tree. It has been removed
everywhere; if it reappears, it is stale.

## What This Proves

The spec models a simplified order book **with matching** at:
- 4 orders maximum, 2 participants, 2 price levels, quantities 1–3
- Order types: Limit only. `OrderTypes` names GTD and others, but no action places them, so `ExpireGTD` never fires
- Actions: PlaceLimit, **Match** (price-cross, FIFO front-of-queue, min-fill),
  CancelOrder, ExpireGTD, AdvanceTime

All safety properties hold across every reachable state — no sequence of
events can produce negative quantities, violate FIFO ordering, execute out of
time priority at a price level, or lose quantity across a fill/cancel
(`MatchingConservation`). The GTD invariant also holds, but only because no GTD
order is ever placed.

## What This Does NOT Prove

- **No concurrent access**: Single-threaded sequential model only.
- **Small constants**: 2 participants, 2 prices, qty ≤ 3. Real systems operate
  at much larger scales — the spec proves the algorithm is correct for the
  modeled domain, not that the implementation handles edge cases at scale.
- **`Quantity_Conservation` not checked, and false**: it is defined as
  `placed = remaining + 2 × traded` and leaves out cancelled quantity, so a
  single place-then-cancel violates it. This line used to say it was excluded
  because its `SUBSET` enumeration is too expensive; `MatchingConservation`
  uses the same `SUBSET` recursion and is in every cfg.

## Replication.tla — Lease-Propagation Model

Updated after the live chaos suite (`deploy/chaos/`) caught a split-brain bug under packet loss that the original spec didn't catch. The original spec had a "god-mode" `~primaryAlive` guard on `BackupPromote`, which let the invariants hold vacuously — TLC never had to reason about the case where the primary is alive but unreachable.

The current spec drops the god-mode guard and replaces it with the actual protocol mechanism:

- Primary broadcasts a `LeaseGrant` on every heartbeat tick (modeled as `TickHealthy` resetting both the heartbeat timer and the lease timer).
- A network partition or primary crash advances both timers (`TickDegraded`).
- `BackupPromote` requires `heartbeatTimer > HeartbeatTimeout` AND `leaseTimer > LeaseTimeout` — the second clause is the fence.

### Default cfg (`spec/Replication.cfg`)

```
MaxEntries       = 6
HeartbeatTimeout = 2
LeaseTimeout     = 5
INVARIANTS NoCommittedLoss NoDuplicateExecution NoSplitBrain
```

Result: **1373 states generated, 448 distinct, 0 violations**.

### Strengthened cfg

```
MaxEntries       = 10
HeartbeatTimeout = 3
LeaseTimeout     = 7
```

Result: **4192 states generated, 1320 distinct, 0 violations**.

### Bug-injected sanity check

Removing the `leaseTimer > LeaseTimeout` line from `BackupPromote` (simulating the pre-fix protocol) causes TLC to terminate immediately with `Invariant NoSplitBrain is violated` at 188 distinct states. That does **not** show the real result is genuine: with the line in place `BackupPromote` is unreachable under both cfgs (see the header), so the pass is vacuous. The variant is a hand edit; no cfg for it is committed.

## Spec Files

| File | Purpose |
|------|---------|
| `spec/MatchingEngine.tla` | Core matching engine spec — order book + matching/cross layer (171M distinct states verified at MaxOrders=4) |
| `spec/MatchingEngine.cfg` | Fast exhaustive config, MaxOrders=3 (1.26M distinct) |
| `spec/MatchingEngine4.cfg` | Deep config, MaxOrders=4 (171M distinct, reported above) |
| `spec/MatchingEngineBroken.cfg` | Non-vacuity variant (BROKEN_NO_FIFO=TRUE → FIFOExecution violation) |
| `spec/Replication.tla` | Primary-backup replication spec with lease propagation (passes vacuously — promotion unreachable) |
| `spec/Replication.cfg` | TLC configuration for Replication |
| `spec/Refinement.tla` | C++ → TLA+ refinement mapping — prose sketch only, never checked: it uses `IntrusiveListToSeq`, which nothing defines, maps actions (`PlaceMarket`, `PlaceGTD`, ...) the spec does not have, and has no cfg |
| `spec/MpscQueue.tla` | Lock-free ring buffer linearizability |
| `spec/EngineConsumer.tla` | Worker loop shutdown safety |
| `spec/Snapshot.tla` / `spec/SnapshotLocked.tla` | Torn-snapshot prevention |
| `spec/Auction.tla` | Auction state machine: valid transitions, no match outside Continuous except the uncross, single clearing price, no trade while halted, volatility auction eventually reopens (no price collar is modelled) |
| `spec/Auction.cfg` / `spec/AuctionBroken.cfg` | Config + broken variant (sanity check) |
| `spec/EpochDurability.tla` | Epoch-store durability under crash |
| `spec/EpochDurability.cfg` / `spec/EpochDurabilityBroken.cfg` | Config + broken variant |
| `spec/FixSession.tla` | FIX inbound sequence recovery: messages delivered strictly in order (no logon or heartbeat is modelled) |
| `spec/FixSession.cfg` / `spec/FixSessionBroken.cfg` | Config + broken variant |
| `spec/Oco.tla` | OCO (one-cancels-other) atomicity — cancel sibling on fill |
| `spec/Oco.cfg` / `spec/OcoBroken.cfg` | Config + broken variant |
| `spec/Risk.tla` | Risk limit enforcement, two tiers: one Firm over Traders T1, T2 |
| `spec/Risk.cfg` / `spec/RiskBroken.cfg` | Config + broken variant |

## New Specs (2026-05-28)

Each new spec follows the same pattern as `Replication.tla`: a correct spec is verified to have zero violations, and a corresponding broken-variant spec (with a specific safety property removed) is verified to reproduce a violation within a bounded number of states — confirming the verification is genuine, not vacuous.

| Spec | Property Verified | Broken Variant |
|------|------------------|----------------|
| `Auction.tla` | Valid state transitions; no match outside Continuous except the uncross; single clearing price; no trade while halted; `VolatilityEventuallyReopens` (liveness) | `BROKEN_NO_REOPEN` → TLC finds a `VolatilityEventuallyReopens` counterexample (a liveness violation) |
| `EpochDurability.tla` | Committed epoch entries survive crash; no epoch can be observed in a state it never transitioned to | Removing durability write fence → TLC finds a stale-read violation |
| `FixSession.tla` | The receiver delivers messages strictly in `MsgSeqNum` order: a gap is held, not delivered (sending the ResendRequest is not modelled) | `BROKEN_NO_GAP_DETECT` (deliver past a gap) → `DeliveredInOrder` violated |
| `Oco.tla` | When one leg of an OCO pair fills, the sibling is cancelled before any further fills; no double-fill | Removing sibling-cancel action → TLC finds a double-fill state |
| `Risk.tla` | Neither tier is breached: `FirmWithinCap`, `TradersWithinCap`, `FirmIsSumOfTraders` — two tiers (Firm over Traders), not four | `BROKEN_NO_FIRM_AGG` (drop the firm-aggregate check) → `FirmWithinCap` violated |

## Reproducing

```bash
cd spec/
# Deep MatchingEngine verification, MaxOrders=4 (~17 min, 171M distinct states):
./check.sh MatchingEngine MatchingEngine4.cfg
# Fast exhaustive check, MaxOrders=3 (~seconds, 1.26M distinct):
./check.sh MatchingEngine MatchingEngine.cfg
# Non-vacuity: broken variant must report "Invariant FIFOExecution is violated":
./check.sh MatchingEngine MatchingEngineBroken.cfg

# Replication lease-propagation verification (seconds):
./check.sh Replication
```

Expected output for the correct cfgs: `Model checking completed. No error has been found.` For Replication that output is the vacuous pass described above.
