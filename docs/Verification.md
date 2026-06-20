# TLA+ Verification Report

> **MatchingEngine.tla**: VERIFIED — 454M states explored, 181M distinct, zero violations · 2026-05-12 · TLC 2026.05.04 · 12m 35s  
> **Replication.tla** (lease-propagation model): VERIFIED — 1373 states at default cfg, 4192 states at stronger MaxEntries=10, zero violations · 2026-05-19 · realistic promotion rule (heartbeat-miss AND lease-expiry required) · bug-injected variant (lease check stripped) reproduces split-brain in 188 states, confirming the verification is genuine

> **New specs (2026-05-28)**: Auction.tla · EpochDurability.tla · FixSession.tla · Oco.tla · Risk.tla — each verified with broken-variant sanity check (TLC finds violation in <1000 states)

## Model Configuration

```
SPECIFICATION Spec
CONSTANTS
    MaxOrders    = 4
    Participants = {1, 2}
    Prices       = {100, 200}
    MaxQty       = 3
    MaxTime      = 5
```

**Workers**: 11 (Apple Silicon M-series, auto-detected)  
**Memory**: 4096MB heap + 64MB offheap  
**Search**: Breadth-first, no symmetry reduction

## Verified Invariants

| Invariant | Description | Status |
|-----------|-------------|--------|
| `NoNegativeQuantity` | No order has negative `qty` or `remainingQty` | ✅ Verified |
| `FIFO_Preservation` | Orders at the same price maintain timestamp ordering | ✅ Verified |
| `GTD_Expiry_Correctness` | Expired GTD orders are always cancelled | ✅ Verified |

## State Space Coverage

```
454,022,166 states generated
181,004,838 distinct states found
14 levels deep (BFS depth)
0 states left on queue (complete exploration)
```

**Fingerprint collision probability**: 0.33% (acceptable for this state space size)

## What This Proves

The spec models a simplified order book with:
- 4 orders maximum, 2 participants, 2 price levels, quantities 1–3
- Order types: Limit, GTD with time-based expiry
- Actions: PlaceLimit, CancelOrder, ExpireGTD, AdvanceTime

All safety properties hold across every reachable state — no sequence of
events can produce negative quantities, violate FIFO, or leave an expired
GTD order active.

## What This Does NOT Prove

- **No matching in the spec**: The spec does not model order matching/trade
  execution. `PlaceLimit` adds to the book but never crosses. Adding matching
  would dramatically expand the state space.
- **No concurrent access**: Single-threaded sequential model only.
- **Small constants**: 2 participants, 2 prices, qty ≤ 3. Real systems operate
  at much larger scales — the spec proves the algorithm is correct for the
  modeled domain, not that the implementation handles edge cases at scale.
- **`Quantity_Conservation` not checked**: The invariant exists in the spec but
  was excluded from the config because its `SUBSET` enumeration is exponentially
  expensive with TLC.

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

Removing the `leaseTimer > LeaseTimeout` line from `BackupPromote` (simulating the pre-fix protocol) causes TLC to terminate immediately with `Invariant NoSplitBrain is violated` at 188 distinct states — confirming the verification is genuine, not vacuous.

## Spec Files

| File | Purpose |
|------|---------|
| `spec/MatchingEngine.tla` | Core matching engine TLA+ specification (454M states verified) |
| `spec/MatchingEngine.cfg` | TLC configuration (constants, invariants) |
| `spec/Replication.tla` | Primary-backup replication spec with lease propagation (verified) |
| `spec/Replication.cfg` | TLC configuration for Replication |
| `spec/Refinement.tla` | C++ → TLA+ refinement mapping (proof sketches) |
| `spec/MpscQueue.tla` | Lock-free ring buffer linearizability |
| `spec/EngineConsumer.tla` | Worker loop shutdown safety |
| `spec/Snapshot.tla` / `spec/SnapshotLocked.tla` | Torn-snapshot prevention |
| `spec/Auction.tla` | Opening/closing auction uncross safety, price collar admission, halt/resume correctness |
| `spec/Auction.cfg` / `spec/AuctionBroken.cfg` | Config + broken variant (sanity check) |
| `spec/EpochDurability.tla` | Epoch-store durability under crash |
| `spec/EpochDurability.cfg` / `spec/EpochDurabilityBroken.cfg` | Config + broken variant |
| `spec/FixSession.tla` | FIX session state machine (logon/heartbeat/gap-fill) safety |
| `spec/FixSession.cfg` / `spec/FixSessionBroken.cfg` | Config + broken variant |
| `spec/Oco.tla` | OCO (one-cancels-other) atomicity — cancel sibling on fill |
| `spec/Oco.cfg` / `spec/OcoBroken.cfg` | Config + broken variant |
| `spec/Risk.tla` | Hierarchical risk limit enforcement (Firm/Account/Strategy/Trader) |
| `spec/Risk.cfg` / `spec/RiskBroken.cfg` | Config + broken variant |

## New Specs (2026-05-28)

Each new spec follows the same pattern as `Replication.tla`: a correct spec is verified to have zero violations, and a corresponding broken-variant spec (with a specific safety property removed) is verified to reproduce a violation within a bounded number of states — confirming the verification is genuine, not vacuous.

| Spec | Property Verified | Broken Variant |
|------|------------------|----------------|
| `Auction.tla` | Auction uncross produces valid clearing price; no orders filled outside price band during continuous trading | Removing price-band check in uncross → TLC finds a fill outside the band |
| `EpochDurability.tla` | Committed epoch entries survive crash; no epoch can be observed in a state it never transitioned to | Removing durability write fence → TLC finds a stale-read violation |
| `FixSession.tla` | FIX session never advances sequence number before sending the corresponding message; ResendRequest handled correctly | Removing sequence-before-send ordering → TLC finds a gap in the delivered sequence |
| `Oco.tla` | When one leg of an OCO pair fills, the sibling is cancelled before any further fills; no double-fill | Removing sibling-cancel action → TLC finds a double-fill state |
| `Risk.tla` | Hierarchical risk limits are never breached at any tier (Firm/Account/Strategy/Trader); child limit cannot exceed parent | Allowing a child limit to exceed parent → TLC finds a breach |

## Reproducing

```bash
cd spec/
# Full MatchingEngine verification (~12 min):
java -XX:+UseParallelGC -cp tla2tools.jar tlc2.TLC MatchingEngine \
     -config MatchingEngine.cfg -workers auto -deadlock

# Replication lease-propagation verification (seconds):
./check.sh Replication
```

Expected output for both: `Model checking completed. No error has been found.`
