# TLA+ Verification Report

> **MatchingEngine.tla** (continuous price-time matching with STP, modify and replace): zero violations at `MatchingEngine.cfg` — 6,796,145 states generated, 2,726,157 distinct, depth 14, complete · each of the seven `MatchingEngineBroken*.cfg` variants fails with the counterexample its injected bug should produce · checked by CI on every push (`.github/workflows/tla.yml`) <!-- claim:tla-matching-engine -->

> **Auction.tla** (session state machine through the enforced transition table): zero violations at `Auction.cfg`, `NoReopenAfterClose` included — 478,401 states generated, 199,348 distinct · four broken cfgs fail as required · 2026-10-22 <!-- claim:tla-auction -->

> **Library-only specs**: Oco.tla · Risk.tla — each verified with a broken-variant sanity check (TLC finds a violation in under 1,000 states); 198 and 800 distinct states. They model code `OrderEngine` never calls (see Scope) <!-- claim:tla-small-specs -->

## Scope: what each spec models, and whether OrderEngine runs it

"Runs" means the shipped binary `OrderEngine` (`src/main.cpp`) executes that
code in a production-shaped configuration, not that some test reaches it.
Checked by reading every caller in `src/` on 2026-11-14.

| Spec | Code it models | Run by `OrderEngine`? |
|---|---|---|
| `MatchingEngine.tla` | `OrderBook::match`, `modifyOrder`, `cancelReplace` (Limit orders, default STP) | Yes: every order the gateway accepts |
| `Auction.tla` | `OrderBook::transitionAllowed`, `setTradingStateLocked`, the uncross | Partly by default: the volatility breaker (on, 5%) enters and leaves `VolatilityAuction`; the session table runs only with `--session-schedule` |
| `MpscQueue.tla` | `include/MpscQueue.h`, the per-worker request queue | Yes: `startAsync` (`main.cpp`) |
| `EngineConsumer.tla` | `MatchingEngine::workerLoop` wake-up protocol | Yes |
| `SnapshotLocked.tla` | `OrderBook::getSnapshot` under `bookLock_` | Yes, when the admin server serves `GET /book` or `/audit` |
| `Oco.tla` | `ContingencyManager` behind `MatchingEngine::registerOco` | **No.** Nothing in `src/` outside `MatchingEngine.cpp` calls `registerOco`; no gateway message or admin route creates an OCO pair. Library code, exercised by tests |
| `Risk.tla` | `HierarchicalRiskManager` and the position ledger (`tryReserveWorking`) | **No.** `mapParticipant`, `setRiskTierLimits` and `setPositionLimit` have no caller in `src/`, so `riskActive_` and `positionLimitsActive_` stay false and the checks return at their first line. Library code, exercised by tests. The spec has two tiers; the code has four |

`Oco.tla` and `Risk.tla` stay in CI, in their own step labelled library-only,
because they are cheap and the code they model is public API with tests; they
say nothing about the binary. `Snapshot.tla`, which modelled the lockless
`getSnapshot` that no longer exists, is deleted; `SnapshotLockedBroken.cfg`
(`BUG = "reader_unlocked"`) is that model now, and CI requires it to fail.

## Deadlock checking

`spec/check.sh` runs TLC with its deadlock check on (it used to pass
`-deadlock`, which turned the check off for every spec). A state with no
successor fails the run with `Deadlock reached` (exit 11). A spec whose
behaviours legitimately end says so with an explicit terminal step, and each
terminal step is as narrow as the end it describes:

| Spec | Terminal step | Why behaviours end | Kind |
|---|---|---|---|
| `MatchingEngine.tla` | `AllDone`: every order submitted, none resting, no aggressor | `MaxOrders` is spent | bound (modelling artefact) |
| `Auction.tla` | `BudgetSpent`: `eventCount = MaxEvents` | the event budget is spent; with budget left every state has a move on the table | bound (modelling artefact) |
| `EngineConsumer.tla` | `Idle`: every item consumed, worker parked, no wake pending | the producers are done; the worker waits for ever, as `workerLoop` does | real end (quiescence) |
| `Oco.tla` | `Done`: no leg live | every leg is resolved | real end |
| `MpscQueue.tla`, `Risk.tla`, `SnapshotLocked.tla` | none needed | some action is always enabled | — |

Without the terminal steps, TLC reported a deadlock in exactly those three
specs, each at the end named above, and nothing else. `EngineConsumerBroken.cfg`
shows the check has teeth: with the worker sampling its wake counter after the
empty check instead of before it, a push in that window is lost and TLC stops
with `Deadlock reached` (the worker asleep on a non-empty queue).

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
`OrderBook::transitionAllowed` enforces (docs/MatchingRules.md Rule 7.7).
`LegalEdges` is that table edge for
edge: any state to `Halted`; `PreOpen` to `Continuous` or `AuctionOpen`;
`AuctionOpen` to `Continuous` or `AuctionClose`; `Continuous` to
`AuctionOpen`, `AuctionClose`, `VolatilityAuction`, `PostClose` or `PreOpen`;
`AuctionClose` to `PostClose`; `VolatilityAuction` to `Continuous`,
`AuctionClose` or `PostClose`; `Halted` to any other state; `PostClose` to
`PreOpen`. It also has the table's three side conditions:
`Continuous -> PreOpen` only before the book's first session,
`PostClose -> PreOpen` only with a trading date later than the closed
session's, and `Halted` only to the state it was halted from (`preHalt`, as
`OrderBook::preHaltState_`) or to `PostClose`: a halt is a pause, not a
route. Books boot in `Continuous`. As in `setTradingStateLocked`, leaving
an auction state for `Continuous` or `PostClose` is the uncross, and every
other change prints nothing. The book is abstract (a sequence of quantities
with no prices), so a crossed book cannot be expressed.

| Property | Kind | Says | Broken cfg (`BUG`) that must break it |
|---|---|---|---|
| `ValidTransitions` | action property | Every state change is on `LegalEdges` and meets its side conditions | `AuctionBrokenEdge.cfg` (`postclose_continuous`), `AuctionBrokenDate.cfg` (`same_day_reopen`) |
| `VolatilityEventuallyReopens` | liveness (weak fairness on the reopening) | A `VolatilityAuction` always ends | `AuctionBroken.cfg` (`no_reopen`) |
| `NoMatchOutsideContinuousExceptUncross` | invariant | A trade prints in `Continuous`, or is the uncross of an auction state | — |
| `NoTradeWhileHalted` | invariant | No trade prints in `Halted` or `PostClose` | — |
| `SingleClearingPrice`, `TypeOK` | invariants | An uncross prints one record at one price; types | — |
| `NoReopenAfterClose` | invariant | Once a session reaches `AuctionClose` or `PostClose`, nothing trades or auctions until the next `PreOpen` | `AuctionBrokenHalt.cfg` (`halt_detour`: `Halted -> Continuous` from any pre-halt state) |

### Last run

One run on 2026-10-22 of the files in this tree (TLC2 2026.05.04, 2 workers,
the Apple Silicon dev box).

```
Auction.cfg
CONSTANTS MaxOrders = 3, MaxEvents = 6, MaxDate = 2, Prices = {100, 200}, BUG = "none"

Model checking completed. No error has been found.
478401 states generated, 199348 distinct states found, 0 states left on queue.
The depth of the complete state graph search is 15.
Finished in 26s   (exit 0)
```

| Cfg | TLC result | Distinct states when it stopped | Exit |
|---|---|---:|---:|
| `AuctionBroken.cfg` | Temporal property VolatilityEventuallyReopens was violated | 48,209 | 13 |
| `AuctionBrokenEdge.cfg` | Action property ValidTransitions is violated | 127 | 13 |
| `AuctionBrokenDate.cfg` | Action property ValidTransitions is violated | 1,395 | 13 |
| `AuctionBrokenHalt.cfg` | Invariant NoReopenAfterClose is violated: `Continuous -> AuctionClose -> Halted -> Continuous` | 509 | 12 |

Every one of the 26 edges in `LegalEdges` is taken in some behaviour at
these bounds: for each edge, a run with a property saying that edge is never
taken fails (exit 13). The same check on `PostClose -> Continuous` and
`AuctionClose -> Continuous`, which are not on the table, passes (exit 0).

### A halt is no detour around the table

Before the pause rule, any state could halt and `Halted` could go to
`Continuous`, so the two moves Rule 7.7 took off the table were reachable in
two steps: TLC's shortest counterexample to `NoReopenAfterClose` was
`Continuous -> AuctionClose -> Halted -> Continuous`, and
`Continuous -> PostClose -> Halted -> Continuous` reopened a closed market.
The same detour took an opening or volatility auction to `Continuous`
without its uncross. Now `Halted` resumes only to the state it was halted
from (or the session ends), `NoReopenAfterClose` is in `Auction.cfg`, and
`AuctionBrokenHalt.cfg` puts the detour back and must fail it.

## Spec Files

| File | Purpose |
|------|---------|
| `spec/MatchingEngine.tla` | Continuous price-time matching with STP, Modify and Replace (section above) |
| `spec/MatchingEngine.cfg` | The bounds CI checks (MaxOrders=3, MaxReplaces=1) |
| `spec/MatchingEngineBroken*.cfg` | Seven non-vacuity variants, one injected bug each; CI requires each to fail on its property |
| `spec/MpscQueue.tla` | MPSC ring: no item invented or duplicated, per-producer FIFO, every item eventually consumed |
| `spec/EngineConsumer.tla` | Worker loop wake-up protocol (`MatchingEngine::workerLoop`): no item invented or duplicated, every item eventually consumed, no lost wake-up (deadlock) |
| `spec/EngineConsumerBroken.cfg` | Non-vacuity variant: wake counter sampled after the empty check; CI requires `Deadlock reached` |
| `spec/SnapshotLocked.tla` | Torn-snapshot prevention: `getSnapshot` under `bookLock_` |
| `spec/SnapshotLockedBroken.cfg` | Non-vacuity variant: no lock; CI requires `SnapshotIsPointInTime` to fail |
| `spec/Auction.tla` | Session state machine through the enforced transition table, order admission per state, uncross (section above) |
| `spec/Auction.cfg`, `spec/AuctionBroken*.cfg` | The bounds CI checks; four broken variants CI requires to fail |
| `spec/Oco.tla` | OCO (one-cancels-other) atomicity — cancel sibling on fill. Library only |
| `spec/Oco.cfg` / `spec/OcoBroken.cfg` | Config + broken variant |
| `spec/Risk.tla` | Risk limit enforcement, two tiers: one Firm over Traders T1, T2. Library only |
| `spec/Risk.cfg` / `spec/RiskBroken.cfg` | Config + broken variant |

## Library-only specs: Oco and Risk

Each has a committed broken cfg, with one safety property's enforcement removed, that `tla.yml` requires to fail on that property, so the correct spec's pass is not vacuous for it. The bounds are small: the correct cfgs reach 800 (Risk) and 198 (Oco) distinct states. Neither models code `OrderEngine` runs (Scope, above). <!-- claim:tla-small-specs -->

| Spec | Property Verified | Broken Variant |
|------|------------------|----------------|
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
```

Expected output for the correct cfgs: `Model checking completed. No error has been found.`
