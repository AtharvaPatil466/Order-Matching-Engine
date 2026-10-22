# TLA+ Verification Report

> **MatchingEngine.tla** (continuous price-time matching with STP, modify and replace): zero violations at `MatchingEngine.cfg` — 6,796,145 states generated, 2,726,157 distinct, depth 14, complete · each of the seven `MatchingEngineBroken*.cfg` variants fails with the counterexample its injected bug should produce · checked by CI on every push (`.github/workflows/tla.yml`)
> **Replication.tla** (lease-propagation model): **VACUOUS** — zero violations at both cfgs, but in both the backup can never promote, so `NoSplitBrain` and `NoCommittedLoss` hold trivially. `TickDegraded` stops advancing the timers once `heartbeatTimer` reaches `HeartbeatTimeout + 1`, the two timers are always equal, and `BackupPromote` needs `leaseTimer > LeaseTimeout`, which is larger (2 vs 5; 3 vs 7). The bug-injected variant (lease check stripped) is the only configuration in which promotion is reachable, and there it splits the brain — the spec has no primary step-down. No `ReplicationBroken` cfg is committed, and CI does not run this spec.

> **Auction.tla** (session state machine through the enforced transition table): zero violations at `Auction.cfg` — 318,551 states generated, 119,448 distinct · three broken cfgs fail as required · `NoReopenAfterClose` is violated by the table itself (see below) · 2026-10-22

> **New specs (2026-05-28)**: EpochDurability.tla · FixSession.tla · Oco.tla · Risk.tla — each verified with broken-variant sanity check (TLC finds violation in <1000 states)

## MatchingEngine.tla

### What it models

One symbol's continuous book, as `OrderBook::match`, `modifyOrder` and
`cancelReplace` run it (`src/OrderBook.cpp`):

- **Submit**: a Limit order arrives and becomes the in-flight aggressor. While
  one is in flight nothing but Match, StpCancelIncoming and Rest can fire,
  which is how the engine holds the book lock for a whole `addOrder`.
- **Match**: the aggressor fills against the front order of the best opposite
  level it crosses, for the smaller of the two quantities, at the resting
  order's price.
- **StpCancelIncoming**: the front order is the aggressor's own participant's,
  so the aggressor's remainder is cancelled and nothing trades. This is
  `STPMode::DefaultCancelIncoming`, the only mode either binary runs (neither
  calls `setSTPMode`).
- **Rest**: the aggressor crosses nothing, and its remainder joins the back of
  its level.
- **Cancel**: a resting order leaves the book.
- **Modify** (`modifyOrder`): a resting order's quantity is reduced in place
  and it keeps its queue position. Zero and increases are refused, as in the
  code.
- **Replace** (`cancelReplace` to a new price, quantity unchanged): the order
  leaves the book, takes a new timestamp and arrives again as an aggressor, so
  it can trade or be STP-cancelled. Replaced to its own price, it rests at the
  back of its level.

Not modelled: Market, IOC, FOK, iceberg and hidden orders; the other three
STP modes (library only, see [Compliance.md](Compliance.md)); a replace that
changes price and quantity together; auctions; pro-rata; risk and admission
checks; the journal and crash recovery; and any concurrency between threads or
symbols. The earlier spec's `ExpireGTD`, `AdvanceTime` and
`GTD_Expiry_Correctness` are gone: no action ever placed a GTD order, so the
invariant held vacuously.

### Properties

| Property | Kind | Says | Broken cfg (`BUG`) that must break it |
|---|---|---|---|
| `NoNegativeQuantity` | invariant | No order's remaining, filled or cancelled quantity is negative | — |
| `NoEmptyRestingOrder` | invariant | Every resting order has quantity left | `MatchingEngineBrokenModify.cfg` (`modify_to_zero`: Modify accepts 0, MATCH-2) |
| `FIFO_Preservation` | invariant | At every level, queue order is timestamp order | `MatchingEngineBrokenReplace.cfg` (`replace_keeps_priority`) |
| `MatchingConservation` | invariant | For every order, quantity = remaining + filled + cancelled; total filled on buys = total filled on sells | — |
| `NoCrossedBookAfterStep` | invariant | Best bid < best ask whenever both sides rest. Holds in every state, not only between submissions, because the aggressor is off the book while it matches | `MatchingEngineBrokenCross.cfg` (`rest_crossed`) |
| `NoSelfTrade` | invariant | No fill has the same participant on both sides | `MatchingEngineBrokenStp.cfg` (`no_stp`) |
| `PricePriority` | action property | The resting order a fill takes is on the best level of its side | `MatchingEngineBrokenPrice.cfg` (`worst_price_first`) |
| `FIFOExecution` | action property | The resting order a fill takes is the oldest at its level | `MatchingEngineBroken.cfg` (`no_fifo`) |
| `ExecPriceIsPassive` | action property | A fill prints at the resting order's price, never worse than the aggressor's limit | `MatchingEngineBrokenExecPrice.cfg` (`aggressor_price`) |

The action properties are `[][...]_vars` formulas checked on every step TLC
generates. `MatchingConservation` and `NoNegativeQuantity` have no broken cfg,
so nothing yet shows that either can fail.

### Last run

Every number below is from one run on 2026-10-18 of the files in this tree,
with `spec/tla2tools.jar` (TLC2 version 2026.05.04), 2 workers, on an Apple
Silicon machine shared with other jobs. CI downloads the jar from the
tlaplus `v1.8.0` release tag, so its version and timing can differ; the
distinct-state count of an exhaustive run should not.

```
MatchingEngine.cfg
CONSTANTS MaxOrders = 3, MaxReplaces = 1, Participants = {1, 2},
          Prices = {100, 200}, MaxQty = 3, BUG = "none"

Model checking completed. No error has been found.
6796145 states generated, 2726157 distinct states found, 0 states left on queue.
The depth of the complete state graph search is 14.
Fingerprint collision probability: calculated 6.0E-7, from actual fingerprints 1.5E-6
Finished in 03min 16s   (exit 0)
```

| Broken cfg | TLC result | Distinct states when it stopped | Exit |
|---|---|---:|---:|
| `MatchingEngineBroken.cfg` | Action property FIFOExecution is violated | 7,032 | 13 |
| `MatchingEngineBrokenPrice.cfg` | Action property PricePriority is violated | 8,518 | 13 |
| `MatchingEngineBrokenExecPrice.cfg` | Action property ExecPriceIsPassive is violated | 645 | 13 |
| `MatchingEngineBrokenCross.cfg` | Invariant NoCrossedBookAfterStep is violated | 427 | 12 |
| `MatchingEngineBrokenStp.cfg` | Invariant NoSelfTrade is violated | 355 | 12 |
| `MatchingEngineBrokenModify.cfg` | Invariant NoEmptyRestingOrder is violated | 84 | 12 |
| `MatchingEngineBrokenReplace.cfg` | Invariant FIFO_Preservation is violated | 3,435 | 12 |

With `MaxOrders = 3`, two orders can rest at one level and a third can cross
both, or two levels can rest on one side and a third can reach both, so FIFO
selection, price priority, partial fills, sweeps, STP and a re-aggressing
replace are all reachable (each broken cfg's counterexample is one of those
interleavings). No larger bound has been run on this spec.

### Earlier figures

README and other documents used to cite 171,187,419 distinct states
(`MatchingEngine4.cfg`, MaxOrders=4) and 1.26M (`MatchingEngine.cfg`,
MaxOrders=3), and before that 181,004,838. All three came from earlier versions
of this spec: no STP, no Modify or Replace, orders resting before they matched
(so the book could rest crossed), and no price-priority check. They do not
describe the spec in this tree. `MatchingEngine4.cfg` and `MatchingEngine5.cfg`
were deleted with them.

### What it does NOT show

- That the C++ does what the spec does. No refinement mapping links the two.
  `Refinement.tla`, which claimed to be one, never parsed and has been
  deleted.
- Anything at scale: 2 participants, 2 prices, quantities 1–3, 3 orders and 1
  replace per behaviour.
- Anything about the parts listed under "Not modelled" above.

## Auction.tla

### What it models

The session state machine, with every state change going through the table
`OrderBook::transitionAllowed` enforces (docs/MatchingRules.md Rule 7.7, lane
M commit `15bcf01`, not yet on `main`). `LegalEdges` is that table edge for
edge: any state to `Halted`; `PreOpen` to `Continuous` or `AuctionOpen`;
`AuctionOpen` to `Continuous` or `AuctionClose`; `Continuous` to
`AuctionOpen`, `AuctionClose`, `VolatilityAuction`, `PostClose` or `PreOpen`;
`AuctionClose` to `PostClose`; `VolatilityAuction` to `Continuous`,
`AuctionClose` or `PostClose`; `Halted` to `Continuous` or `PostClose`;
`PostClose` to `PreOpen`. It also has the table's two side conditions:
`Continuous -> PreOpen` only before the book's first session, and
`PostClose -> PreOpen` only with a trading date later than the closed
session's. Books boot in `Continuous`. As in `setTradingStateLocked`, leaving
an auction state for `Continuous` or `PostClose` is the uncross, and every
other change prints nothing. The book is abstract (a sequence of quantities
with no prices), so a crossed book cannot be expressed.

| Property | Kind | Says | Broken cfg (`BUG`) that must break it |
|---|---|---|---|
| `ValidTransitions` | action property | Every state change is on `LegalEdges` and meets both side conditions | `AuctionBrokenEdge.cfg` (`postclose_continuous`), `AuctionBrokenDate.cfg` (`same_day_reopen`) |
| `VolatilityEventuallyReopens` | liveness (weak fairness on the reopening) | A `VolatilityAuction` always ends | `AuctionBroken.cfg` (`no_reopen`) |
| `NoMatchOutsideContinuousExceptUncross` | invariant | A trade prints in `Continuous`, or is the uncross of an auction state | — |
| `NoTradeWhileHalted` | invariant | No trade prints in `Halted` or `PostClose` | — |
| `SingleClearingPrice`, `TypeOK` | invariants | An uncross prints one record at one price; types | — |
| `NoReopenAfterClose` | invariant, **not in `Auction.cfg`** | Once a session reaches `AuctionClose` or `PostClose`, nothing trades or auctions until the next `PreOpen` | **Violated by the enforced table**, see below |

### Last run

One run on 2026-10-22 of the files in this tree, with the same TLC jar,
worker count and machine as MatchingEngine above.

```
Auction.cfg
CONSTANTS MaxOrders = 3, MaxEvents = 6, MaxDate = 2, Prices = {100, 200}, BUG = "none"

Model checking completed. No error has been found.
318551 states generated, 119448 distinct states found, 0 states left on queue.
The depth of the complete state graph search is 13.
Fingerprint collision probability: calculated 1.3E-9, from actual fingerprints 1.9E-9
Finished in 08s   (exit 0)
```

| Cfg | TLC result | Distinct states when it stopped | Exit |
|---|---|---:|---:|
| `AuctionBroken.cfg` | Temporal property VolatilityEventuallyReopens was violated | 53,780 | 13 |
| `AuctionBrokenEdge.cfg` | Action property ValidTransitions is violated | 125 | 13 |
| `AuctionBrokenDate.cfg` | Action property ValidTransitions is violated | 1,539 | 13 |
| `Auction.cfg` + `NoReopenAfterClose` | Invariant NoReopenAfterClose is violated: `Continuous -> AuctionClose -> Halted -> Continuous` | 504 | 12 |

Every one of the 22 edges in `LegalEdges` is taken in some behaviour at
these bounds: for each edge, a run with a property saying that edge is never
taken fails (exit 13). The same check on `PostClose -> Continuous`, which is
not on the table, passes (exit 0).

### The table still reopens a closed session

Rule 7.7 says it took `PostClose -> Continuous` (reopening a closed market)
and `AuctionClose -> Continuous` (abandoning a closing auction) off the
table. Both are still reachable in two steps, because any state may halt and
`Halted` may go to `Continuous`. TLC's shortest counterexample to
`NoReopenAfterClose` is `Continuous -> AuctionClose -> Halted ->
Continuous`. With the invariant narrowed to `PostClose` alone, it is
`Continuous -> PostClose -> Halted -> Continuous` (exit 12). The same detour
also takes an opening or volatility auction to `Continuous` without its
uncross (`Halted` is not an auction state), which this abstract book cannot
show. `NoReopenAfterClose` goes into `Auction.cfg` once the table closes the
detour (for example, resume to the pre-halt state, or no halt from
`AuctionClose` or `PostClose`).

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
| `spec/MatchingEngine.tla` | Continuous price-time matching with STP, Modify and Replace (section above) |
| `spec/MatchingEngine.cfg` | The bounds CI checks (MaxOrders=3, MaxReplaces=1) |
| `spec/MatchingEngineBroken*.cfg` | Seven non-vacuity variants, one injected bug each; CI requires each to fail on its property |
| `spec/Replication.tla` | Primary-backup replication spec with lease propagation (passes vacuously — promotion unreachable) |
| `spec/Replication.cfg` | TLC configuration for Replication |
| `spec/MpscQueue.tla` | Lock-free ring buffer linearizability |
| `spec/EngineConsumer.tla` | Worker loop shutdown safety |
| `spec/Snapshot.tla` / `spec/SnapshotLocked.tla` | Torn-snapshot prevention |
| `spec/Auction.tla` | Session state machine through the enforced transition table, order admission per state, uncross (section above) |
| `spec/Auction.cfg`, `spec/AuctionBroken*.cfg` | The bounds CI checks; three broken variants CI requires to fail |
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
| `EpochDurability.tla` | Committed epoch entries survive crash; no epoch can be observed in a state it never transitioned to | Removing durability write fence → TLC finds a stale-read violation |
| `FixSession.tla` | The receiver delivers messages strictly in `MsgSeqNum` order: a gap is held, not delivered (sending the ResendRequest is not modelled) | `BROKEN_NO_GAP_DETECT` (deliver past a gap) → `DeliveredInOrder` violated |
| `Oco.tla` | When one leg of an OCO pair fills, the sibling is cancelled before any further fills; no double-fill | Removing sibling-cancel action → TLC finds a double-fill state |
| `Risk.tla` | Neither tier is breached: `FirmWithinCap`, `TradersWithinCap`, `FirmIsSumOfTraders` — two tiers (Firm over Traders), not four | `BROKEN_NO_FIRM_AGG` (drop the firm-aggregate check) → `FirmWithinCap` violated |

## Reproducing

```bash
cd spec/
# MatchingEngine at the CI bounds (a few minutes):
./check.sh MatchingEngine
# Each broken variant must fail with its property's counterexample, e.g.
# "Action property PricePriority is violated":
./check.sh MatchingEngine MatchingEngineBrokenPrice.cfg

# Replication lease-propagation verification (seconds):
./check.sh Replication
```

Expected output for the correct cfgs: `Model checking completed. No error has been found.` For Replication that output is the vacuous pass described above.
