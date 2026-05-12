# TLA+ Verification Report

> **Status: VERIFIED** — 454M states explored, 181M distinct, zero violations  
> **Date**: 2026-05-12 · **Tool**: TLC 2026.05.04 · **Duration**: 12m 35s

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

## Spec Files

| File | Purpose |
|------|---------|
| `spec/MatchingEngine.tla` | Core matching engine TLA+ specification |
| `spec/MatchingEngine.cfg` | TLC configuration (constants, invariants) |
| `spec/Replication.tla` | Primary-backup replication spec (not yet verified) |
| `spec/Refinement.tla` | C++ → TLA+ refinement mapping (proof sketches) |

## Reproducing

```bash
cd spec/
java -XX:+UseParallelGC -cp tla2tools.jar tlc2.TLC MatchingEngine \
     -config MatchingEngine.cfg -workers auto -deadlock
```

Expected output: `Model checking completed. No error has been found.`  
Expected time: ~12 minutes on Apple Silicon M-series.
