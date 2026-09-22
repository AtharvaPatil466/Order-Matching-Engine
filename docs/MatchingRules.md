# Matching Rules

The matching semantics of this engine, written as a venue rulebook rather than a code
walkthrough. Every rule is stated first in plain language; the `file:line` evidence sits
underneath it. A reader who does not read C++ should be able to follow the rules; a reader
who wants to reimplement the engine should be able to do so from this text without opening
`OrderBook.cpp`. That is the bar this document is written to.

**Written against commit `05ea604`.** Line references are exact at that commit and nowhere
else. If you are reading this at a later commit and a citation looks wrong, the citation is
wrong — check the code, then fix this document.

## How to read the status labels

Each rule carries one:

| Label | Meaning |
|---|---|
| **Deliberate — commented and pinned** | The code explains the choice *and* a test fails if it changes. |
| **Deliberate — commented, not pinned** | The code explains the choice. No test would catch a regression. |
| **Undocumented — no test pins this, may be incidental** | The code makes a choice, nothing says why, nothing would notice if it changed. It may be a considered decision whose rationale was never written down, or it may be whatever fell out of the implementation. **From reading the code you cannot tell which.** |
| **Known defect** | Flagged in [§10](#10-suspected-defects). Documented here so the rulebook does not launder it into a rule. |

Where a rule's behaviour genuinely cannot be determined by reading the source — because it
depends on container iteration order or on a race — this document says so instead of
guessing.

## Summary of the seven questions

| # | Question | Answer in one line | Status |
|---|---|---|---|
| 1 | Iceberg under pro-rata | Allocation weight is the **displayed slice**; the refreshed slice **keeps** its list position | Deliberate — commented and pinned ([§3](#3-iceberg-orders)) |
| 2 | STP during allocation | Resolved in a **pre-pass before** allocation; any removal **restarts** the level and re-derives every ratio | Deliberate — commented and pinned ([§4](#4-self-trade-prevention)) |
| 3 | Stop cascades | No second-level election in one sweep; fan-out capped at 64, the rest **latched and deferred** | Deliberate — commented and pinned ([§5](#5-stop-stop-limit-and-market-if-touched)) |
| 4 | Trailing stops / pegs | Full linear rescan of **every** trailing stop and **every** peg on **every** order submission; a repegged order **loses** time priority | Undocumented — no test pins the complexity or the priority loss ([§6](#6-trailing-stops-and-pegged-orders)) |
| 5 | Auction uncross | max volume → min imbalance → nearest reference price → market pressure | Deliberate — commented, thinly pinned ([§7](#7-auctions-and-the-volatility-uncross)) |
| 6 | GTD expiry | **Full linear scan** of the order table, on a **1 Hz background thread**, under the book lock | Undocumented — no test pins the mechanism or its cost ([§8](#8-time-in-force-and-expiry)) |
| 7 | Price type | `int64_t` integer ticks, 4 implied decimals | Deliberate — commented and pinned ([§1](#1-prices-quantities-and-arithmetic)) |

---

## 1. Prices, quantities and arithmetic

### Rule 1.1 — Prices are 64-bit signed integers on a fixed tick grid

A price is an `int64_t` count of ticks. There are 10,000 ticks to one currency unit, so
prices carry exactly four implied decimal places. A price of `1000000` means 100.0000.
There is no floating-point price anywhere in the engine: no price is stored, compared,
matched, published or journalled as a `double`.

> `include/Types.h:10` — `using Price = int64_t;`
> `include/Types.h:202` — `constexpr int64_t PRICE_PRECISION = 10000;`
> `include/Types.h:217,221` — `toDouble()` / `toPrice()` exist for **test and display
> convenience only**; nothing on the matching path calls them.

Quantities are `uint64_t` whole units (`include/Types.h:11`). Order notional is computed in
`__int128` and scaled back down by `PRICE_PRECISION`, because `price × qty` overflows
`int64_t` at venue scale and signed overflow wraps negative — which silently *passes* a risk
cap rather than failing it.

> `include/Types.h:213-215` — `constexpr __int128 orderNotional(Price, Quantity)`

**Status: Deliberate — commented and pinned.** The `__int128` widening carries its own
rationale comment and the tick-grid choice is load-bearing across the wire protocols.

### Rule 1.2 — The only doubles near a price are percentage configuration values

Four `double`s exist in the matching translation unit. None of them is a price, and none of
them is ever stored as one:

| Value | Where | What it is |
|---|---|---|
| `cbThreshold_` | `include/OrderBook.h:957` | Circuit-breaker threshold, default 0.05 (5%) |
| `priceBandPct_` | `include/OrderBook.h:958` | Price-band half-width, 0 = disabled |
| `marketProtectionPct_` | `include/OrderBook.h:959` | Market-order sweep collar, 0 = disabled |
| `vwap_` | `include/OrderBook.h:948` | Session VWAP — an analytics output, never an input to matching |

Each is used in exactly one shape: multiply an `int64_t` reference price by the percentage,
truncate the product back to `Price`, and compare integers from then on.

> `src/OrderBook.cpp:375-377` and `src/OrderBook.cpp:604-606` — price band. Both sites carry
> the comment *"Use integer math against an absolute deviation rather than floating-point
> ratio to keep the check exact at the tick grid and stable under hot-loop reordering."*
> `src/OrderBook.cpp:1127-1128` — market-order protection bound, same shape.

The one genuine floating-point **ratio** is the circuit-breaker deviation. Both its operands
are `int64_t`; the division to a ratio is the only floating-point step, and its result is
compared against a `double` threshold and then discarded:

> `src/OrderBook.cpp:342` —
> `double deviation = std::abs(static_cast<double>(price - referencePrice_)) / referencePrice_;`

**Status: Deliberate — commented.** The price-band sites explain why they avoid the ratio
form; the breaker site does not, and is the one remaining ratio comparison.

### Rule 1.3 — Pro-rata allocation weight is computed in double precision

The proportional share each resting order receives at a price level is computed as a
`double` and truncated toward zero. The DMM/LMM floor is likewise a `double` multiply,
rounded half-up.

> `src/OrderBook.cpp:1542-1544` —
> `share = static_cast<Quantity>(static_cast<double>(avail) / totalLevelQty * toFill)`
> `src/OrderBook.cpp:1559` — `floor = static_cast<Quantity>(avail * DMM_FLOOR_PCT + 0.5)`

This is a **quantity**, not a price, so it does not compromise Rule 1.1. It does mean
allocation is only exact while quantities stay below 2^53. At realistic venue sizes that is
not reachable; the engine does not check it.

**Status: Undocumented — no test pins this, may be incidental.** No comment explains why
the share is computed in floating point rather than as `avail * toFill / totalLevelQty` in
`__int128`, which would be exact and is the form used elsewhere in the file for notional.

---

## 2. The two matching algorithms

### Rule 2.1 — Each symbol selects one algorithm; it is not per-order and not per-level

A book is either price-time FIFO or pro-rata for its whole life. The algorithm is chosen at
book construction or by `setMatchAlgorithm`, and the same branch selects it at every match
entry point.

> `include/Types.h:187-190` — `enum class MatchAlgorithm { PriceTime, ProRata }`
> `src/OrderBook.cpp:1014-1017` — the branch in `addOrder`
> `src/OrderBook.cpp:2073-2076` — the same branch in `cancelReplace`

### Rule 2.2 — Price priority is absolute in both algorithms

Both algorithms consume the opposite book strictly one price level at a time, best first,
and stop the moment the next best level is worse than the incoming order's limit. Neither
algorithm ever allocates across two price levels simultaneously.

> `src/OrderBook.cpp:1256-1262` (price-time) and `src/OrderBook.cpp:1449-1454` (pro-rata)

### Rule 2.3 — Within a price level, price-time is strict FIFO by book-entry time

A level is an intrusive doubly-linked list. Entry appends to the tail
(`include/FlatPriceMap.h:146,151` → `OrderList::push_back`). Matching takes the head. There
is no size priority, no top-of-book allocation, no matching-event pro-rata component.

### Rule 2.4 — Within a price level, pro-rata allocates by available quantity, with a market-maker floor

Pro-rata at one level runs in this exact order:

1. **STP pre-pass** (see [§4](#4-self-trade-prevention)). If it removed or reduced anything,
   abandon everything computed so far and restart at step 2 from the new book state.
2. Sum the **available** quantity of every order at the level. Available means the displayed
   slice for an iceberg and the full leaves for everything else (`src/OrderBook.cpp:1521-1524`).
3. `toFill = min(incoming leaves, level total)` (`src/OrderBook.cpp:1531`).
4. For each of the **first 1024** orders at the level in list order, `share = floor(available
   / total × toFill)` (`src/OrderBook.cpp:1538-1546`). Orders past index 1023 receive no
   allocation on this pass — see [defect B1](#b1--pro-rata-silently-allocates-only-the-first-1024-orders-at-a-level).
5. **DMM/LMM floor.** Any order whose owner is a Designated or Lead Market Maker is topped up
   to at least 40% of its available quantity (capped at `toFill`). The top-up is *taken from*
   Regular participants' allocations, first-come in list order, and does not create new
   volume (`src/OrderBook.cpp:1549-1576`).
6. **Rounding remainder.** `toFill − Σ shares` is distributed in two passes: DMM/LMM first, in
   list order, then Regular, in list order (`src/OrderBook.cpp:1578-1599`). This is the only
   point at which queue position affects a pro-rata outcome.
7. Execute every non-zero allocation. Zero allocations produce no trade and no message
   (`src/OrderBook.cpp:1605-1606`).

> The 40% floor is `src/OrderBook.cpp:1553` — `static constexpr double DMM_FLOOR_PCT = 0.40;`

**Status: Deliberate — pinned, partly uncommented.** The mechanics are pinned by
`tests/TestLmmDmmAllocation.cpp` (DMM floor, large and small incoming, Regular-only
regression) and `tests/ManualTest.cpp:866`. But the **40% figure itself** carries no comment
and no citation to any venue's published floor. It is a magic number with a test around it,
which pins the behaviour without justifying the value.

**How venues specify it.** A guaranteed market-maker allocation ahead of the pro-rata
distribution is standard — CME's matching algorithms include an LMM/"Top Order" allocation
step, and several venues publish DMM guarantees. I do not know of a venue that publishes
exactly 40%, and I am not going to assert one. Treat 40% as this engine's own parameter.

---

## 3. Iceberg orders

An iceberg carries a total quantity (`remainingQty`), a displayed slice (`visibleQty`), and
a refresh size (`displayQty`). Only the slice is public.

> `include/Order.h:47` (`visibleQty`), `include/Order.h:56` (`displayQty`)
> `include/Order.h:110-112` — `displayQuantity()`, the single definition of "what the market
> can see" for one order, shared by the L2 aggregation and the order-level feed.

### Rule 3.1 — An iceberg with a zero display size is rejected at admission

`RejectReason::InvalidDisplayQty`. A display size larger than the order is clamped down
rather than rejected.

> `src/OrderBook.cpp:533-537`

This is not cosmetic: a resting iceberg with `visibleQty == 0` makes `available == 0`, which
makes `fillQty == 0`, which makes `while (remainingQty > 0)` never terminate. The rejection
is the livelock guard, and it is what makes it safe to say that every resting iceberg has a
strictly positive slice.

**Status: Deliberate — commented and pinned.** `tests/CriticalFixesTest.cpp:360`.

### Rule 3.2 — Under BOTH algorithms, an iceberg trades on its displayed slice only

In continuous trading an incoming order can never consume more than the currently displayed
slice of an iceberg in a single fill, under either algorithm. In pro-rata the slice is also
the **allocation weight**: the hidden reserve contributes nothing to the level total and
nothing to the ratio.

> `src/OrderBook.cpp:1334-1335` (price-time: `available` is `visibleQty` for an iceberg)
> `src/OrderBook.cpp:1521-1524` (pro-rata: the level total sums `visibleQty` for an iceberg)
> `src/OrderBook.cpp:1540-1541` (pro-rata: so does each order's numerator)

This is the answer to "displayed or total?": **displayed, for both the fill cap and the
pro-rata weight.**

**Status: Deliberate — commented and pinned** by `tests/TestOrderBook.cpp:891`, whose
arithmetic (`iceberg = 100/150 × 11`) is only correct if the weight is the slice.

**How venues specify it.** Weighting a hidden reserve in pro-rata would defeat the purpose of
hiding it — the reserve would earn allocation it does not display — so displayed-quantity
weighting is the design that makes sense and is what I understand CME to do with native
iceberg display quantity. **I do not know Eurex's exact pro-rata iceberg weighting rule and
will not guess at it.** If this engine is ever pointed at a Eurex-style product, this is the
first rule to check against their rulebook.

### Rule 3.3 — When a slice refreshes, price-time sends it to the BACK of the queue

Under price-time FIFO, exhausting an iceberg's displayed slice causes the replenished slice
to be treated as a **new arrival**. It is unlinked from its position and appended to the tail
of its price level. Any order that arrived after the iceberg was originally placed, but
before the refresh, now fills ahead of it.

> `src/OrderBook.cpp:1390-1404` — `level->remove(bookOrder); level->push_back(bookOrder);`

**Status: Deliberate — commented and pinned.** The code cites the convention by name, and
`tests/TestOrderBook.cpp:853` (`IcebergPriority_PriceTime_LosesPriorityOnRefresh`) fails if
the two lines above are removed.

**How venues specify it.** Back-of-queue on refresh is the near-universal convention for
price-time books — NYSE, Nasdaq reserve orders, LSE and Eurex iceberg all treat a refreshed
slice as a new order for time priority. This engine matches that.

### Rule 3.4 — When a slice refreshes, pro-rata KEEPS its position

Under pro-rata, the slice is replenished **in place**. The order does not move in the level
list.

> `src/OrderBook.cpp:1645-1663`

The engine's stated reasoning: pro-rata allocates by volume weight, not by time, so list
position affects nothing except the rounding-remainder distribution (Rule 2.4 step 6). An
iceberg therefore participates continuously in proportional allocation for as long as hidden
quantity remains.

**Status: Deliberate — commented and pinned** by `tests/TestOrderBook.cpp:891`
(`IcebergPriority_ProRata_KeepsPriorityOnRefresh`), which is constructed specifically so that
the *rounding remainder* is the discriminator: the iceberg's extra +1 lot is only earned if it
is still in front.

**How venues specify it.** CME and Eurex do answer this differently, which is exactly why it
needed writing down. The code comment asserts this is "CME-style". I am confident the general
principle is sound — under a purely volume-weighted algorithm, queue position is nearly
meaningless, so re-queuing a refresh would be a distinction without a difference — but **I am
not certain this matches CME's published iceberg rule verbatim**, and the code cites no
rulebook. Treat the *principle* as well-founded and the *attribution* as unverified.

### Rule 3.5 — In an auction cross, an iceberg fills against its FULL remaining size

The uncross is the one place where the reserve is exposed. An auction fills against
`remainingQty`, not `visibleQty`, so a single cross can consume an iceberg's entire hidden
quantity at once. The slice is re-derived and re-announced after every fill so the public
feed can follow a fill that exceeded the slice it was displaying.

> `src/OrderBook.cpp:2477-2483` (`resliceIceberg`)
> `src/OrderBook.cpp:2574` — `fillQty = min({buyer->remainingQty, seller->remainingQty, remainingVolume})`
> `src/OrderBook.cpp:2790-2792` — price discovery likewise sums `remainingQty`, so hidden size
> counts toward the discovered volume and the published imbalance.

**Status: Deliberate — commented and pinned.** `tests/CriticalFixesTest.cpp:1100`.

**How venues specify it.** Full-size participation in an auction is the standard treatment —
an auction is a single-price event with no queue to protect, so hiding size from it serves no
purpose. This is consistent with how I understand Xetra/Eurex and the Nasdaq crosses to work.

---

## 4. Self-trade prevention

Detection is unconditional: two orders belong to the same participant, or they do not. The
configured mode selects the **action**, never whether prevention happens. There is no setting
that permits a self-cross, and the unconfigured default prevents.

> `src/OrderBook.cpp:401-405` — `checkSMP` is `incoming.participantId == resting.participantId`
> `include/SelfTradeProtection.h:32-43` — the five modes
> `include/SelfTradeProtection.h:102-111` — `isSelfTrade` ignores the mode entirely

| Mode | Action |
|---|---|
| `DefaultCancelIncoming` (0, unconfigured) | Cancel the incoming order |
| `CancelResting` (1) | Remove the resting order, incoming continues |
| `CancelIncoming` (2) | Cancel the incoming order, resting survives |
| `CancelBoth` (3) | Remove both |
| `DecreaseAndCancel` (4) | Reduce the resting order; cancel it if it reaches zero — **see [defect B2](#b2--decreaseandcancel-never-decrements-the-incoming-order)** |

### Rule 4.1 — An STP-cancelled order reports `CancelledBySTP`, never `Cancelled` and never `Filled`

A distinct terminal status exists precisely so an STP removal cannot be mistaken for either a
client cancel or an execution. Critically, `remainingQty` is **left intact** when the incoming
order is STP-cancelled: zeroing it would make `filled = initialQty − remainingQty` report the
entire order as executed.

> `include/Types.h:50-60` — why the status exists
> `include/Types.h:64-71` — `isExecution()`, the single predicate for "did this order trade?"
> `src/OrderBook.cpp:1047-1087` — `stpCancelRestingOrder`, shared by all three matching paths
> `src/OrderBook.cpp:1088-1104` — `finalizeIfStpCancelled`

An order that traded against *other* participants before hitting the self-cross reports
`PartiallyFilled` with its genuine executed quantity. Only a zero-fill STP termination reports
`CancelledBySTP` (`src/OrderBook.cpp:1092-1099`).

**Status: Deliberate — commented and pinned.** `tests/StpSemanticsTest.cpp` covers all five
modes on all three paths.

### Rule 4.2 — Under pro-rata, STP is resolved BEFORE allocation, and any removal re-runs the allocation

This is the answer to question 2. **The engine re-runs allocation over the surviving makers;
it does not proceed with the original ratios.**

The mechanism: before any arithmetic, pro-rata walks the entire best level applying STP to
every same-participant order it finds. If that pass removed or reduced anything, it sets
`levelChanged` and `continue`s the outer loop — which discards every computed total, ratio
and remainder and recomputes them from the post-STP book. If the level was emptied entirely,
the same `continue` picks up the next best price.

> `src/OrderBook.cpp:1458-1518` — the pre-pass
> `src/OrderBook.cpp:1513-1517` — *"Quantities at this level moved: restart the outer loop so
> the level totals and allocations are computed from the new state."*

Two properties fall out of doing it first rather than inline:

- **Nothing stale.** No maker can be allocated a share derived from a level total that
  included an order STP subsequently removed.
- **No use-after-free.** Removing a resting order mid-allocation-walk would free the node
  whose `->next` the loop is about to read. The pre-pass reads `o->next` before any teardown
  (`src/OrderBook.cpp:1473`).

**Status: Deliberate — commented and pinned.**
`tests/StpSemanticsTest.cpp:363` (`CancelRestingRemovesMakerAndLetsTakerContinue`) is the
one that pins the re-run: the taker removes its own 60 lots and still fills 40 against the
other participant. An implementation that proceeded with the original ratios, or that
abandoned the sweep, fails it.

**How venues specify it.** CME, Nasdaq and Cboe all publish STP with cancel-newest /
cancel-oldest / cancel-both / decrement variants. I do not know of a venue that publishes
whether a mid-allocation STP event re-derives its pro-rata ratios — it is the kind of detail
that lives in a matching-engine specification rather than a public rulebook. **That is exactly
why the question was worth asking, and this engine's answer (re-run) is the one that produces
allocations consistent with the book that actually existed at match time.**

### Rule 4.3 — Under price-time, STP is resolved per fill, as the sweep reaches each order

The price-time path has no pre-pass and needs none: it takes one resting order at a time, so
there is no batch of ratios to invalidate. `CancelResting` and `DecreaseResting` `continue`
to the next resting order; every other action stops the sweep.

> `src/OrderBook.cpp:1275-1331`

The `switch` has **no `default:` case**, deliberately. A missing case is a `-Wswitch -Werror`
compile error rather than a new silent phantom fill (`src/OrderBook.cpp:1283-1288`).

The whole block is skipped on the common path by an O(1) per-participant occupancy counter
that answers "does this participant have *any* resting order in this book?" without a scan.
The counter can overcount (harmless extra STP pass) but never undercount, so a self-trade
cannot slip past it.

> `src/OrderBook.cpp:1133-1157`; parity between the counter and a brute-force rescan is
> asserted in `tests/StpOccupancyTest.cpp`.

### Rule 4.4 — In an auction cross, "incoming" means the later-arriving order by timestamp

An auction has no aggressor — both orders were resting before the uncross — so
`CancelIncoming` / `CancelResting` have no literal referent. The engine maps "incoming" to
the **later-arriving** order by `timestamp`, on the grounds that price-time is the auction's
own ordering and the newer order is the one that created the self-cross. Equal timestamps
fall to the buyer.

> `src/OrderBook.cpp:2519-2571`, with the mapping at `src/OrderBook.cpp:2540`

**Status: Deliberate — commented and pinned** (`tests/StpSemanticsTest.cpp`, auction section),
**and carrying an acknowledged defect** — see [B3](#b3--auction-price-discovery-runs-before-auction-stp).

---

## 5. Stop, stop-limit and market-if-touched

### Rule 5.1 — Stops are parked, not booked. They are invisible until elected.

A `Stop`, `StopLimit` or `MIT` order never enters `bids_`/`asks_` on arrival. It is appended
to a flat array of armed stops and `addOrder` returns immediately — no matching, no market
data, no depth contribution.

> `src/OrderBook.cpp:824-829` (`parkNonMatchingOrder`)
> `include/OrderBook.h:878` — `FixedVector<Order*, 16384> stopOrders_;`

The arming capacity is 16,384 per book. A 16,385th stop is silently dropped by
`FixedVector::push_back` returning `false`, which nothing checks
(`include/OrderBook.h:196-200`) — the client still receives an `Accepted`, and the order still
exists in `orderLookup_`, so it is cancellable but will never trigger. **Status:
Undocumented — no test pins this, may be incidental.**

### Rule 5.2 — Election tests

Evaluated against the last trade price, at or through the level:

| Type | Buy elects when | Sell elects when |
|---|---|---|
| `Stop`, `StopLimit` | `last ≥ stopPrice` | `last ≤ stopPrice` |
| `MIT` | `last ≤ stopPrice` | `last ≥ stopPrice` |

`MIT` is the favourable-direction mirror of a stop and carries its trigger in the same field.

> `src/OrderBook.cpp:2232-2251`

On election the order's type is rewritten in place:

| Armed as | Becomes | At price |
|---|---|---|
| `Stop` | `Limit` | its original `price` |
| `StopLimit` | `Limit` | `stopLimitPrice` |
| `MIT` | `Market` | — unfilled remainder is cancelled, never rested |

> `src/OrderBook.cpp:2266-2275`, `src/OrderBook.cpp:2285-2293`

### Rule 5.3 — A triggered stop's own fill CANNOT elect another stop in the same sweep

`checkStopOrders(Price)` takes the last trade price **by value** and has exactly one call
site. A stop that fires inside the sweep updates `lastTradePrice_`, but the sweep continues to
evaluate every remaining stop against the price it was called with. A second-level election
therefore defers to the next arriving order.

> `src/OrderBook.cpp:2205` — the signature
> `src/OrderBook.cpp:1021-1023` — the only call site

A laddered book in which each stop's own fill would trip the next advances **exactly one
rung per incoming order**, however deep the ladder.

**Status: Deliberate — commented and pinned.**
`tests/StopCascadeBoundTest.cpp:173` (`testLadderDoesNotCascade`) asserts exactly 1 election
per order at a ladder depth of 256.

**How venues specify it.** CME does the opposite: a triggered stop's execution can elect
further stops within the same match event, and CME bounds the resulting cascade with
protection points and with Velocity Logic / Stop Logic reserved states rather than by
structurally forbidding the second level. This engine's choice is a different, simpler
design: cascades are impossible rather than bounded, at the cost of spreading a genuine
multi-level stop run across multiple incoming messages.

### Rule 5.4 — Fan-out within one sweep is capped at 64 executions; the rest are latched, not dropped

One print can elect every stop resting at or through that price. Each election runs a full
match plus book mutation plus market-data publish. Before the bound, one order into a book
with 10,000 co-located stops took **2.13 ms** against a 237 ns median.

The engine executes at most `kMaxStopExecutionsPerSweep = 64` elected stops inline per sweep.
`checkStopOrders` and `updateTrailingStops` each get their own budget of 64, so one incoming
order can execute up to 128 elections in total.

> `include/OrderBook.h:645` — the constant, with the full measurement table at
> `include/OrderBook.h:612-644`
> `src/OrderBook.cpp:2257-2263` and `src/OrderBook.cpp:2347-2351` — the two budget checks

### Rule 5.5 — The election is latched BEFORE the budget is consulted

Order of operations, and it matters:

```
if (triggered) {
    order->isStopTriggered = true;        // latch first
    if (executed >= 64) { ++i; continue; } // then check budget
    ++executed;
    ... execute ...
}
```

> `src/OrderBook.cpp:2253-2264` (stops), `src/OrderBook.cpp:2343-2351` (trailing stops)

Because the latch happens first, the set of stops that were *elected* by a print is exactly
the set the print's price justified — independent of how many the engine had budget to run.
On every subsequent sweep the latch is read before the trigger test is re-evaluated
(`src/OrderBook.cpp:2230`, `src/OrderBook.cpp:2324`), so:

**A deferred stop is never un-elected.** If the market moves back through the trigger level
before the deferred stop gets its turn, it still fires. Without the latch the bound would
silently *cancel* stops rather than *defer* them.

**Status: Deliberate — commented and pinned.**
`tests/StopCascadeBoundTest.cpp:146` (`testDeferredStopsSurvivePriceRecovery`) prints back
above every trigger level after the budget runs out and asserts all of them still fire.
`tests/StopCascadeBoundTest.cpp:120` (`testFanOutIsBoundedAndNothingIsDropped`) asserts
exactly 64 fire on the first sweep and that the remainder drain in `ceil(N/64)` sweeps with
none lost. `tests/StopCascadeBoundTest.cpp:187` pins the identical bound on trailing stops.

### Rule 5.6 — What the owner of a deferred stop observes: nothing

This is the part a participant needs to know and the part the engine does not tell them.

- **No message is sent at election time.** The deferral branch emits no order update, no
  execution report, nothing. The first thing the owner hears is the fill when the stop
  eventually runs (`src/OrderBook.cpp:2257-2263`).
- **The order still looks armed.** `getOrder()` returns it with `status == Accepted` (set once
  at `src/OrderBook.cpp:700` and not touched by election) and `type` still `Stop`/`StopLimit`/
  `MIT` — the type is only rewritten at execution. The `isStopTriggered` latch is a private
  field carried on no wire protocol (`include/Order.h:71`; grep confirms it is read only
  inside `OrderBook`).
- **It remains cancellable, and a cancel wins.** A cancel arriving before the deferred
  execution removes the order and discards the election, silently
  (`src/OrderBook.cpp:1879` → `src/OrderBook.cpp:1843-1846`). The owner is never told their
  stop had already been elected.
- **Deferral is measured in arriving orders, not in time.** The sweep runs only from
  `addOrder` (`src/OrderBook.cpp:1021-1023`). Cancels, modifies, expiry sweeps and uncrosses
  do **not** drain the backlog. If order flow stops, a latched stop sits latched indefinitely.

So from the outside, a deferred stop is indistinguishable from an un-elected one. A
participant cannot tell whether their stop is armed or already committed.

**Status: Undocumented consequence of a deliberate design.** The bound itself is documented
and pinned; the client-observable consequence of it is not stated anywhere, and no test
asserts what a deferred stop's owner sees. This is worth a wire-protocol field — a "triggered,
pending execution" state — and there is not one.

### Rule 5.7 — Deferral order is scan order, and scan order is arbitrary

**Which 64 of the elected stops fire first is not time priority and is not specified by
anything.**

`stopOrders_` is a flat array scanned from index 0. When a stop executes, `erase_swap(i)`
removes it by **moving the last element of the array into slot `i`**
(`include/OrderBook.h:202-207`). Cancellation and expiry do the same thing via `erase_value`
(`include/OrderBook.h:210-218`, reached from `src/OrderBook.cpp:1843-1846`).

Consequences:

- On a pristine book that has never removed a stop, array order equals arrival order, so the
  first 64 elected do fire in arrival order.
- **After a single removal of any kind — one execution, one cancel, one expiry — that stops
  being true.** The tail element has jumped into the middle. Array order is now a permutation
  of arrival order determined by the entire history of removals.
- Within one sweep the permutation is still evolving: each execution swaps a new element into
  the slot the scan is currently reading.

This is not a race and it is fully deterministic given an identical input sequence — journal
replay reproduces it — but it is **not derivable from the order book's public state**. Two
participants whose stops sit at the same trigger level have no way to reason about which of
them is executed and which is deferred, and it is not the one who armed first.

**Status: Undocumented — no test pins this, may be incidental.** `erase_swap` was chosen for
O(1) removal from a flat array; there is no evidence anyone decided the resulting fairness
properties were acceptable. A venue publishing this rulebook would have to either specify
time-ordered deferral or state plainly that deferral order is unspecified. Right now the code
does the latter without saying so.

---

## 6. Trailing stops and pegged orders

### Rule 6.1 — Every trailing stop is rescanned on every order submission, unconditionally

**This is the answer to "does the engine rescan every trailing order when the reference price
moves?" — it rescans them whether or not the reference moved.**

`updateTrailingStops` is called from `addOrder` whenever the book has ever traded
(`lastTradePrice_ > 0`) — not when a trade occurs, not when the price changes. An order that
rests without matching, or that is rejected after allocation, still pays for a full linear
walk of `trailingStopOrders_`.

> `src/OrderBook.cpp:1021-1023` — the guard is `if (lastTradePrice_ > 0)`, nothing more
> `src/OrderBook.cpp:2308-2378` — the sweep

**Complexity per submitted order: O(T)** where T is the number of armed trailing stops, up to
the 16,384 array cap. The same is true of `checkStopOrders`: **O(S)** per submitted order,
which the code flags in its own ponytail comment as a *second* latency term, independent of
the execution bound:

> `src/OrderBook.cpp:2207-2215` — *"the scan is O(all resting stops) on every sweep, ~1.8 ns
> each here, so a full 16384-entry `stopOrders_` costs ~30 us of pure scanning whether or not
> anything is elected."*

The named upgrade path is to index the stop arrays by trigger price so a sweep touches only
the stops a print actually reached. It has not been done.

**Status: Deliberate — commented, not pinned.** The cost is measured and written down for
`checkStopOrders`; the identical cost for `updateTrailingStops` is not, and no test would
notice either becoming quadratic.

### Rule 6.2 — The trailing ratchet is one-way and is skipped once latched

For a sell trailing stop: each time the price makes a new high, `trailRefPrice` advances and
`stopPrice` becomes `trailRefPrice − trailAmount`, floored at zero. The stop elects when the
price falls to or through `stopPrice`. A buy trailing stop is the mirror, ratcheting down.

> `src/OrderBook.cpp:2326-2341`

Once an election is latched, **the ratchet is skipped along with the trigger test**
(`src/OrderBook.cpp:2320-2326`). This is deliberate: a deferred election whose trail kept
following the price could stop qualifying, which would cancel it rather than defer it.

### Rule 6.3 — Stop elections feed trailing-stop elections within one submission, but not the reverse

An asymmetry worth stating because it is not obvious from either function:

```cpp
if (lastTradePrice_ > 0) {
    checkStopOrders(lastTradePrice_);     // argument read here
    updateTrailingStops(lastTradePrice_); // argument read AGAIN, after the above ran
}
```

> `src/OrderBook.cpp:1021-1023`

`lastTradePrice_` is a member. `checkStopOrders` runs `match()` on each election, which
updates it. So **trailing stops are evaluated against the price after the stop cascade, while
ordinary stops were evaluated against the price before it.** Trailing stops therefore see one
extra level of price propagation that ordinary stops do not, and there is no reverse path —
a trailing stop's fill cannot elect an ordinary stop until the next submission.

**Status: Undocumented — no test pins this, may be incidental.** Nothing comments on the
re-read, and it is equally consistent with a deliberate one-way propagation and with nobody
having noticed that the second argument is evaluated later.

### Rule 6.4 — Every pegged order is repriced on every order submission, and a repeg loses time priority

`updatePeggedOrders` runs from `addOrder` whenever any pegged order exists, recomputing every
peg's target from the current book:

| Peg type | Target |
|---|---|
| `MidPeg` | `mid + pegOffset`, where `mid = bb + (ba − bb)/2` |
| `PrimaryPeg` | same-side touch `+ pegOffset` |

> `src/OrderBook.cpp:1025-1026` (call site), `src/OrderBook.cpp:2380-2425` (the sweep)
> `src/OrderBook.cpp:2991-2999` (`getMidPrice`, overflow-safe)

**Complexity per submitted order: O(P)**, P = number of resting pegs, cap 16,384. Each peg's
target is O(1) to compute, so the sweep is a linear walk with a handful of integer ops per
entry.

When a peg's target differs from its current price, the order is **removed from the book,
repriced, given a fresh timestamp, and re-inserted** — and insertion appends to the tail of
the destination level.

> `src/OrderBook.cpp:2411-2415` — `removeFromBook` → `order->price = newPrice` →
> `order->timestamp = nowNs()` → `addToBook`
> `include/FlatPriceMap.h:146,151` — insertion is `push_back`

**So yes: a repegged order loses time priority, every time it repegs.** A peg that follows a
moving touch is perpetually at the back of its queue.

The depth change is published as a `Delete` at the old price and an `Add` at the new one, so
incremental subscribers can follow it (`src/OrderBook.cpp:2416-2420`).

**Status: Undocumented — no test pins this, may be incidental.** The comment at
`src/OrderBook.cpp:2407-2410` explains why the *market-data events* are published; nothing
explains or pins the *priority* consequence, and no test asserts it.

**How venues specify it.** Losing priority on a reprice is the common treatment — a repeg is
a new price, and price-time priority is defined per price level, so there is nowhere for the
old priority to live. I do not know of a venue that publishes a "sticky priority across
repegs" rule, and this engine's behaviour is unsurprising. What is unusual is the *trigger*:
see Rule 6.5.

### Rule 6.5 — Pegs only follow the market when a new order arrives

`updatePeggedOrders` has exactly one call site, in `addOrder`. It is **not** called from
`cancelOrder`, `cancelReplace`, `expireOrders`, `uncross` or the kill switch.

> `src/OrderBook.cpp:1025-1026` — the sole call site

So a cancel that removes the best bid moves the touch, and every primary-pegged buy is now
pegged to a price that no longer exists — until some unrelated order is submitted. In a quiet
book that can be an arbitrarily long time.

> `tests/MarketDataL2ReconstructionTest.cpp:219` acknowledges this in passing:
> *"updatePeggedOrders runs on the NEXT submission"*.

**Status: Undocumented — no test pins this, may be incidental.** One test comment notices the
behaviour while testing something else. Nothing asserts it, and nothing explains it. This is
the rule most likely to surprise a participant, and it interacts badly with
[defect B4](#b4--pegged-orders-can-rest-at-a-crossing-price).

---

## 7. Auctions and the volatility uncross

### Rule 7.1 — Auction states accumulate; they do not match

`PreOpen`, `AuctionOpen`, `AuctionClose` and `VolatilityAuction` all admit orders into the
book without running continuous matching. All trades are produced by a single `uncross()` at
the session boundary, at one price.

> `src/OrderBook.cpp:1008-1017`
> `include/Types.h:166-185` — the trading-state enum and what each state admits

Market orders submitted during an auction cannot rest in a price-indexed book, so they are
parked separately and folded into discovery.

> `src/OrderBook.cpp:809-822`

### Rule 7.2 — The clearing price is discovered by a four-step tie-break cascade

Candidate prices are every populated limit level on either side, deduplicated and sorted
ascending. For each candidate `p`:

- `cumBuy` = all buy quantity at limits `≥ p`, plus every parked market buy
- `cumSell` = all sell quantity at limits `≤ p`, plus every parked market sell
- `vol = min(cumBuy, cumSell)`, `imb = |cumBuy − cumSell|`

The winner is selected by this cascade, applied as a strict ordering:

1. **Maximum executable volume.** Higher `vol` wins.
2. **Minimum imbalance.** On equal volume, lower `imb` wins.
3. **Nearest the reference price.** On equal volume and equal imbalance, and *only if a
   reference price is set*, the candidate with the smaller `|p − referencePrice_|` wins.
4. **Market pressure.** On a remaining tie — or when no reference price exists — a buy surplus
   clears **higher** and a sell surplus clears **lower**.

> `src/OrderBook.cpp:2757-2860` — `discoverUncrossPrice()`
> `src/OrderBook.cpp:2819-2842` — the cascade, with the four steps named in a comment
> `src/OrderBook.cpp:2791` — candidate prices, capped at `FixedVector<Price, 4096>`

Parked market orders participate at **every** candidate price, since they have no limit to
anchor on, and so add uniformly to both cumulants (`src/OrderBook.cpp:2759-2765`).

If no candidate produces crossing volume — one-sided book, or a spread that never closes —
there is no cross. Parked market orders and unfilled LOC orders are cancelled, and a book-wide
imbalance figure is still published so the indicative feed carries something meaningful for a
one-sided book.

> `src/OrderBook.cpp:2456-2464`, `src/OrderBook.cpp:2777-2781`, `src/OrderBook.cpp:2849-2853`

**The indicative price the market sees before the cross and the price the cross executes at
are the same function.** `computeAuctionState()` (published pre-cross) and `uncross()` both
call `discoverUncrossPrice()`, which is a pure read and never mutates the book. They cannot
disagree.

> `src/OrderBook.cpp:2864-2867`, `src/OrderBook.cpp:2453-2456`

**Complexity.** For each of `L` candidate prices the discovery walks *every* level on *both*
sides. That is **O(L²)** level visits plus O(L × orders) order visits — a full quadratic
re-scan, not a cumulative sweep. An auction runs once per session so this is defensible, and
the code says as much about the *publishing* being per-fill, but **nothing comments on the
quadratic discovery itself and nothing bounds it beyond the 4096-candidate array cap.**

**Status: Deliberate — commented, thinly pinned.** The cascade is named in a comment at both
the declaration (`include/OrderBook.h:692-697`) and the implementation.
`tests/TestAuctionSession.cpp` covers the lifecycle; I did not find a test that discriminates
step 3 from step 4, so the *position of the reference-price step in the cascade* is not pinned.

**How venues specify it.** The shape — volume, then imbalance, then a reference — is the
standard auction cascade. The engine's ordering resembles the **Nasdaq** opening/closing
cross: maximize executable volume, then minimize imbalance, then minimize distance to a
reference price. **Xetra/Eurex order it differently**: after volume and surplus they apply
*market pressure* (surplus side determines direction) and only then fall back to the
reference price. This engine places reference price **ahead of** market pressure, so on a
volume-and-imbalance tie it will pick a different price than Xetra would. That is a real,
stateable difference and it is not written down anywhere in the code.

### Rule 7.3 — The cross executes strict price-time at one price, with no pro-rata

Regardless of the book's configured algorithm, the uncross pairs the front of the best bid
level against the front of the best ask level, repeatedly, until the discovered paired volume
is exhausted. Every trade prints at `bestUncrossPrice`.

> `src/OrderBook.cpp:2495-2651`

Parked market orders are inserted into the book at the discovered price first, so the
execution loop treats them as limit orders queued there (`src/OrderBook.cpp:2488-2491`). Any
market-order remainder is cancelled after the cross rather than left resting — a market order
is auction-only and does not get to become a limit at the clearing price
(`src/OrderBook.cpp:2653-2670`). Unfilled `LOC` orders are cancelled likewise
(`src/OrderBook.cpp:2673`, definition at `src/OrderBook.cpp:2714`).

**Status: Undocumented — no test pins this, may be incidental.** That the cross ignores
`matchAlgorithm_` and is always FIFO is a real semantic choice — a pro-rata product would
plausibly want pro-rata allocation at the cross — and nothing states it or tests it.

### Rule 7.4 — A volatility breach opens an auction; reopening is operator-driven

When an order's price deviates from `referencePrice_` by more than `cbThreshold_`, the book
transitions to `VolatilityAuction` rather than halting. The breaching order is rejected
(`VolatilityCircuitBreaker`); subsequent orders are admitted into the auction. A manual halt
remains a hard halt.

> `src/OrderBook.cpp:626-645` — `admitCircuitBreaker`
> `src/OrderBook.cpp:340-345` — `checkCircuitBreaker`

Reopening runs the standard uncross, re-anchors `referencePrice_` to the reopening print so
the post-auction bands measure from the fresh price, and returns to `Continuous`:

> `src/OrderBook.cpp:2869-2884` — `resumeVolatilityAuction()`, with the re-anchor at
> `src/OrderBook.cpp:2881`

**There is no automatic dwell timer and no automatic reopen.** `resumeVolatilityAuction()` has
no scheduler behind it; something outside the book must call it.

### Rule 7.5 — What this engine calls LULD is a single fixed percentage band, not US-equities LULD

The name appears throughout (`OutsidePriceBand`, `priceBandPct_`, `LULDManager`), so the
distance from the real thing needs stating:

| US-equities LULD (Reg NMS Plan) | This engine |
|---|---|
| Reference price is a rolling 5-minute average | `referencePrice_` is seeded from the first limit price seen and re-anchored only on a volatility-auction reopen (`src/OrderBook.cpp:591-595`, `src/OrderBook.cpp:2881`) |
| Band percentage is tiered by price and by time of day, and doubles near the open/close | One flat `priceBandPct_` for the whole session |
| A quote at the band creates a *Limit State*; 15 seconds in Limit State becomes a 5-minute pause | No Limit State, no dwell timer |
| A pause reopens via an auction on a published schedule | Reopen is an explicit external call |
| Trades outside the band are prohibited | The band is an **admission** filter on limit-style orders only — see [defect B5](#b5--triggered-stops-and-pegs-bypass-the-price-band-and-the-circuit-breaker) |

The band is checked only for `Limit`, `IOC`, `FOK`, `PostOnly`, `Iceberg` and `Hidden`.
`Market`, `Stop`, `StopLimit`, `TrailingStop`, `MIT` and `Pegged` are all excluded.

> `src/OrderBook.cpp:597-611`

**Status: Undocumented — no test pins the exclusion list, and it may be incidental.** Some of
the exclusions are necessary (a `Market` order has no price to band-check; a `Stop` carries a
trigger, not a tradeable limit). Whether the *triggered* form should be re-checked is a
separate question the code never asks.

### Rule 7.6 — `LULDManager` is not wired to anything

`OrderBook` holds a `LULDManager luld_` member and exposes it through a getter. **Nothing in
the engine ever calls a method on it.** Its band computation, pause state machine, dwell timer
and resume logic are all unreachable.

> `include/OrderBook.h:359-360`, `include/OrderBook.h:972` — the member and its accessors
> A repository-wide grep finds no other use: no `luld_.` call site exists.

Everything described in Rules 7.4 and 7.5 is done by `referencePrice_` / `cbThreshold_` /
`priceBandPct_` on `OrderBook` directly. `LULDManager` is a parallel, dormant implementation.

**Status: [Defect B8](#b8--luldmanager-is-dead-code).** Listed here so nobody reads
`LULDManager.h` and believes it describes engine behaviour. It does not.

---

## 8. Time in force and expiry

### Rule 8.1 — GTD and DAY expiry is a full linear scan on a 1 Hz background thread

**This is the answer to question 6: it is neither a timer wheel nor a priority queue. It is a
scan.**

The mechanism end to end:

1. `startExpiryTimer(intervalMs)` spawns a thread that sleeps, wakes, and calls
   `expireOrdersFromClock()`. **Default interval: 1000 ms.**
   > `include/MatchingEngine.h:194`, `src/MatchingEngine.cpp:461-484`
2. In async mode the sweep is posted as an `ExpireCheck` control message to every worker; in
   sync mode it runs inline.
   > `src/MatchingEngine.cpp:499-518`, `src/MatchingEngine.cpp:936-957`, `src/MatchingEngine.cpp:1960-1983`
3. Each book takes its **exclusive** `bookLock_` and calls `orderLookup_.forEach` — which walks
   **every slot of the hash table's capacity**, live or empty, testing each live order's
   `timeInForce` and `expiryTime`.
   > `src/OrderBook.cpp:2181-2201`, `include/FlatHashMap.h:222-237`
4. Expired ids are collected into a **4096-entry stack buffer** and then cancelled one at a
   time, still under the lock, with the journal callback fired *before* each cancel so replay
   reproduces expirations deterministically without a virtual clock.
   > `src/OrderBook.cpp:2186-2200`

### Rule 8.2 — What it costs, and who pays

**It does not cost the hot path per order.** No matching path reads `expiryTime` — `match()`,
`matchProRata()` and `uncross()` contain no expiry test at all. Submitting an order pays
nothing for expiry.

**It costs the hot path per sweep.** The sweep takes the same exclusive `bookLock_` that
`addOrder` takes, so once per interval, matching stalls for the duration of a full hash-table
walk plus the cancels. With the example configuration's `order_pool_capacity = 10000`
(`config/engine.conf.example:23`) the table capacity is the next power of two above that at
the configured load factor, so the walk is on the order of 16k–32k slot reads — cheap in
absolute terms, but it is a **lock-held stall on the matching thread**, not background work,
and its duration scales with pool capacity rather than with how many orders are actually
expiring.

### Rule 8.3 — An expired order remains fully tradeable until the next sweep

Because nothing on the matching path checks `expiryTime`, an order whose expiry has passed
continues to rest, display, and fill normally until a sweep reaches it. **The window is up to
one full interval — one second at the default.**

This is a genuine semantic, not a detail: a client whose GTD order expires at T can receive a
fill stamped as late as T + 1s.

**Status: Undocumented — no test pins this, may be incidental.** `tests/GTDReplayTest.cpp`
and `tests/TestOrderBook.cpp:389` drive expiry with an injected clock and assert that expiry
*happens*; nothing asserts anything about the window during which it has not happened yet.

### Rule 8.4 — At most 4096 orders expire per sweep

The stack buffer is fixed at 4096. Once full, the scan keeps running but collects nothing
more; the overflow waits for the **next** interval rather than triggering an immediate second
pass.

> `src/OrderBook.cpp:2186`, `src/OrderBook.cpp:2190`

With the example pool capacity of 10,000 this is reachable — a session-end cohort of 5,000 DAY
orders takes two sweeps, i.e. an extra second, to clear.

### Rule 8.5 — DAY is treated exactly as GTD, plus a separate session-end sweep

`expireOrders` makes no distinction between `GTD` and `DAY`: both require a non-zero
client-supplied `expiryTime`, and a `DAY` order submitted without one **never expires by
timer**.

> `src/OrderBook.cpp:2191-2192`

What actually retires DAY orders is a separate sweep at graceful shutdown, which cancels
every `DAY` order regardless of `expiryTime`.

> `src/MatchingEngine.cpp:1168-1181` — `cancelDayOrders()`, called from the shutdown report
> path at `src/MatchingEngine.cpp:1219`

**Status: Undocumented — no test pins the interaction, and it may be incidental.** Nothing in
the engine derives a session-end `expiryTime` for a `DAY` order, so between submission and
shutdown a `DAY` order with `expiryTime == 0` behaves identically to a `GTC` order. That may
be intended (shutdown is the session end) or may be a gap — `SessionScheduler` drives
trading-state transitions but never touches time-in-force.

**How venues specify it.** Every venue publishes DAY as "cancelled at the end of the trading
session", and deriving the session-end timestamp from the session calendar is how I would
expect it to be implemented. Venues do **not** publish their expiry *mechanism* — timer wheel
versus scan is an implementation detail, not a rule — so there is nothing to compare Rules
8.1–8.4 against. The part that *is* rulebook material is Rule 8.3, the tradeable-after-expiry
window, and I do not know of a venue that publishes one.

### Rule 8.6 — Absolute expiry timestamps are in an unspecified epoch

`expiryTime` is compared against `high_resolution_clock::now().time_since_epoch().count()`.

> `src/MatchingEngine.cpp:491-497` (`expiryNow`), `include/LatencyTracker.h:12-15` (`nowNs`)

See [defect B6](#b6--gtd-expiry-timestamps-are-compared-against-a-platform-dependent-epoch).

---

## 9. Amendment and priority

Collected here because they are the rules most often assumed rather than read.

### Rule 9.1 — `modifyOrder` only shrinks, and shrinking keeps priority

A quantity reduction is applied in place; the order keeps its queue position. A quantity
*increase* is refused with `InvalidQuantity` — growing in place would forfeit priority anyway,
so it is a cancel-replace, not a modify.

> `src/OrderBook.cpp:1933-1954` (the shrink), `src/OrderBook.cpp:1956-1960` (the refusal)

An iceberg's slice is clamped to the new leaves, and only the *displayed* shrink is published.
A modify-down that consumes hidden reserve without touching the slice emits nothing the market
can see (`src/OrderBook.cpp:1936-1952`).

### Rule 9.2 — `cancelReplace` loses priority on a price change or a quantity increase, keeps it on a shrink

| Change | Priority |
|---|---|
| Price changed | Lost. Removed, repriced, timestamped, matched if it now crosses, then re-rested at the tail. `src/OrderBook.cpp:2048-2097` |
| Same price, quantity down | **Kept.** In-place reduction. `src/OrderBook.cpp:2100-2113` |
| Same price, quantity up | Lost. Removed and re-added at the tail with a fresh timestamp. `src/OrderBook.cpp:2114-2124` |

A repriced order is **off the book while it matches** and acts as an aggressor. Its display
entry is removed before the match so its fills surface through the resting maker's execution
only, as any aggressor's do.

> `src/OrderBook.cpp:2050-2059`

### Rule 9.3 — A replace is a fresh admission decision

`cancelReplace` re-runs the full admission chain — trading-state gate, risk limits, price
band, circuit breaker, and the `PostOnly` would-cross test. A rejected replace leaves the
original order untouched and resting, and returns the *specific* reason rather than
"not found".

> `src/OrderBook.cpp:2025-2041`

Pool pressure is excluded, and correctly so: a replace reuses the existing `Order` and never
allocates, so there is nothing to shed.

### Rule 9.4 — Ownership failures are reported as `OrderNotFound` on the wire

`NotOrderOwner` exists internally for logs and metrics, but `clientVisibleReason()` maps it to
`OrderNotFound` before it reaches a client. Answering "that order exists, it is just not
yours" would turn a cancel into an order-id oracle: walk the id space, keep the ids that come
back "not yours", and you have mapped a competitor's resting book.

> `include/Types.h:141-146`, `include/Types.h:156-164`

### Rule 9.5 — `minQty` is a minimum on the FIRST execution only

`minQty` is checked once at admission against crossable liquidity and the stored field is
never read again. Matching enforces no per-fill minimum.

If insufficient crossable liquidity exists: an `IOC` is cancelled; a **crossing** limit order
is cancelled (resting it would leave bid ≥ ask, a locked book); a **non-crossing** limit order
rests and waits.

> `src/OrderBook.cpp:872-915`, with the locked-book reasoning at `src/OrderBook.cpp:885-897`

**Status: Deliberate — commented and pinned.** The comment states the semantic explicitly and
explains why the crossing case cannot simply rest.

---

## 10. Suspected defects

Behaviour that looks like a defect rather than a choice. **Nothing in this section is a rule.**
Flagged, not fixed.

### B1 — Pro-rata silently allocates only the first 1024 orders at a level

`matchProRata` caps its allocation array at `MAX_LEVEL_ORDERS = 1024` and stops building
allocations at that index — but the level **total** was summed over *all* orders at the level,
with no cap.

> `src/OrderBook.cpp:1436` — `static constexpr size_t MAX_LEVEL_ORDERS = 1024;`
> `src/OrderBook.cpp:1521-1524` — the total loop, uncapped
> `src/OrderBook.cpp:1538` — `for (... o && allocCount < MAX_LEVEL_ORDERS; ...)`

At a level holding more than 1024 orders, orders past index 1023 receive **zero** allocation on
each pass. The algorithm still terminates and still cannot over-fill — the remainder passes cap
each top-up at `avail − allocs[i].qty` — but the behaviour silently degenerates from "pro-rata
across the level" to "pro-rata across the front 1024, FIFO-gated". Deep-queue participants are
excluded from allocation entirely until the front of the queue is consumed, which is precisely
the outcome pro-rata exists to avoid.

**Severity: high for a pro-rata product.** No comment acknowledges the truncation and no test
exercises a level deeper than 1024. Two plausible fixes — allocate in chunks, or compute the
total over the same capped window so the ratios are at least self-consistent — but this is a
behaviour change and out of scope here.

### B2 — `DecreaseAndCancel` never decrements the incoming order

On all three matching paths, `DecreaseResting` reduces the resting order by the match quantity
and leaves the incoming order's `remainingQty` untouched. The sweep therefore re-triggers on
the same resting order and decrements it again, repeatedly, until it is exhausted and
cancelled.

> `src/OrderBook.cpp:1318-1329` (price-time), `src/OrderBook.cpp:1499-1509` (pro-rata),
> `src/OrderBook.cpp:2560-2570` (auction)

Net effect: `DecreaseAndCancel` is behaviourally identical to `CancelResting` whenever the
incoming order survives the first pass. A resting 100 met by an incoming 30 ends at **0**, not
70.

This is acknowledged in prose at `tests/StpSemanticsTest.cpp:411-422`, which calls it *"a
pre-existing defect of the price-time path"* and pins the current behaviour deliberately so
the three paths cannot drift. **It is not acknowledged in any document, and the mode's name
and its header comment (`include/SelfTradeProtection.h:11` — *"Decrease resting qty by match
amount; cancel if zero"*) both describe the intended behaviour, not the actual one.**

**How venues specify it.** CME and Nasdaq both publish decrement-and-cancel as decrementing
**both** orders by the minimum of the two quantities and cancelling whichever reaches zero.
This engine's version is not that. A participant configuring `DecreaseAndCancel` expecting the
published semantics will lose their entire resting order instead of the overlapping part.

### B3 — Auction price discovery runs before auction STP

`uncross()` discovers `bestUncrossPrice` and `pairedVolume` from a book that still contains
the self-crossing orders, then removes them during execution. The printed price and volume can
therefore exceed what the post-STP book actually supports.

> `src/OrderBook.cpp:2528-2532` — the code's own `KNOWN LIMITATION` comment

The code states the correct fix (filter self-crossing pairs *before* discovery) and states
that it was out of scope for the change that introduced the mode-aware auction STP. It remains
undone. This is a **published-price correctness** issue, not a book-integrity one: the book
ends consistent, but the indicative that went out and the print that resulted can both be
wrong.

### B4 — Pegged orders can rest at a crossing price

Neither `restPeggedOrder` (initial placement) nor `updatePeggedOrders` (repeg) checks whether
the computed peg price crosses the opposite side, and neither calls `match()`. The order is
handed straight to `addToBook`.

> `src/OrderBook.cpp:788-798`, `src/OrderBook.cpp:2411-2415`

`pegOffset` is client-supplied and **unvalidated** — `validateOrderRequest` bounds quantity,
iceberg display size, price sign and duplicate ids, and says nothing about `pegOffset`
(`src/OrderBook.cpp:517-552`). A buy `MidPeg` with an offset larger than half the spread, or a
buy `PrimaryPeg` with an offset larger than the spread, computes a price above the best ask
and **rests there without matching**: bid ≥ ask, a locked or crossed book with no trade.

This is the identical defect class that `screenMinQty` was explicitly fixed for. Its comment
even names the symptom: *"Resting unconditionally therefore parked the order at a price that
crosses the opposite side, leaving bid >= ask: an invalid book"*
(`src/OrderBook.cpp:885-893`). The fix was applied to the `minQty` path and not to the pegged
path.

Rule 6.5 makes it worse: because pegs are only recomputed on the next `addOrder`, a crossed
peg can sit crossed indefinitely.

**Severity: high.** A crossed book is visible to every market-data subscriber and breaks any
consumer that assumes `bestBid < bestAsk`.

### B5 — Triggered stops and pegs bypass the price band and the circuit breaker

The admission filters check the price band and the circuit breaker only for `Limit`, `IOC`,
`FOK`, `PostOnly`, `Iceberg` and `Hidden`.

> `src/OrderBook.cpp:597-611` (band), `src/OrderBook.cpp:628-630` (breaker)

A `Stop` is admitted without either check, because at admission it carries a trigger rather
than a tradeable price — reasonable. But when it elects, `checkStopOrders` rewrites its type
to `Limit`/`Market` and calls `match()` **directly**, never re-entering `addOrder`.

> `src/OrderBook.cpp:2266-2278`

So a triggered stop executes with **no band check and no breaker check, ever**. A stop
cascade can therefore print straight through a LULD band that would have rejected the same
order submitted directly — which is the exact scenario price bands exist to contain. The same
holds for `TrailingStop` (`src/OrderBook.cpp:2353-2356`) and for `Pegged`, which is excluded
from the band list and rests without any check at all.

**Severity: high for anything claiming LULD-style controls.** No comment considers the
re-check, and no test covers a stop firing outside the band. It is genuinely ambiguous whether
this is a deliberate "a stop is already committed" stance or simply an unconsidered gap —
**from the code you cannot tell.**

### B6 — GTD expiry timestamps are compared against a platform-dependent epoch

`expiryNow()` and `nowNs()` both return
`std::chrono::high_resolution_clock::now().time_since_epoch().count()`.

> `src/MatchingEngine.cpp:491-497`, `include/LatencyTracker.h:12-15`

`high_resolution_clock` is an alias whose target is implementation-defined:

- **libstdc++** (Linux / GCC — the CI target) aliases it to `system_clock`: nanoseconds since
  the **Unix epoch**.
- **libc++** (macOS / Clang — the development box) aliases it to `steady_clock`: nanoseconds
  since an **unspecified point**, in practice system boot.

A client-supplied absolute `expiryTime` therefore means two entirely different instants
depending on which standard library the engine was built against. The same GTD order that
expires correctly on Linux expires immediately, or never, on macOS.

Internal consistency is preserved — `Order::timestamp` uses the same clock, so time priority
and the auction's newer/older STP mapping are unaffected — so this only bites the one field
that crosses the API boundary as an absolute value. `setExpiryClock()`
(`include/MatchingEngine.h:196`) lets a test inject a clock, which is why every expiry test
passes on both platforms and none of them would catch this.

**Fix shape:** pin the clock to `system_clock` explicitly, or document `expiryTime` as
relative. Both are behaviour changes.

### B7 — The auction's final tie-break reads the incumbent's surplus side, not the candidate's

Step 4 of the cascade (Rule 7.2) is:

> `src/OrderBook.cpp:2838-2841` — `better = bestBuySurplus ? (p > bestPrice) : (p < bestPrice);`

`bestBuySurplus` is the surplus direction of the **current incumbent**, not of the candidate
`p` being evaluated. On a tie in volume *and* imbalance where the two candidates have surplus
on **opposite sides** — equal magnitude, opposite sign — the direction of the tie-break is
decided by whichever candidate happened to be scanned first.

Candidates are scanned in ascending price order, so the outcome is fully deterministic and
reproducible. It is not a race. But the rule it implements is asymmetric in a way the comment
above it does not describe: "buy surplus clears higher, sell surplus lower" reads as a property
of the candidate, and it is implemented as a property of the incumbent.

**Severity: low** — it requires an exact double tie with opposed surplus — but it means the
cascade as *documented* and the cascade as *implemented* are not the same function. Either the
comment or the code is wrong, and a rulebook cannot state which without a decision.

### B8 — `LULDManager` is dead code

Constructed as an `OrderBook` member, exposed through `getLULDManager()`, and **never
driven**. No call site anywhere in the repository invokes any of its methods.

> `include/OrderBook.h:359-360`, `include/OrderBook.h:972`

Its entire state machine — `Normal`/`Paused`/`Resuming`, the dwell timer, `shouldPause`,
`enterPause`, `shouldResume`, `completeResume` — is unreachable. A reader who opens
`LULDManager.h` to learn how this engine handles volatility pauses will learn something false:
the real behaviour is Rules 7.4 and 7.5, implemented on `OrderBook` directly with a different
(and simpler) model.

**Severity: documentation-grade, but genuinely misleading.** Either wire it or delete it.

### B9 — The expiry sweep's 4096 cap does not trigger an immediate second pass

When more than 4096 orders expire in one interval, the sweep collects 4096, keeps scanning the
rest of the table doing nothing (the guard `return`s from the lambda, not the loop), and the
overflow waits a **full interval** rather than being swept again immediately.

> `src/OrderBook.cpp:2186-2196`

**Severity: low.** It only shows up at session boundaries with large DAY cohorts, and the
consequence is a bounded delay rather than an incorrect fill. Listed because Rule 8.4 would
otherwise read as if the cap were free.

---

## 11. What this document does not cover

Deliberately out of scope, so their absence is not mistaken for "the engine has no rule":

- **Fees, rebates and clearing.** See `include/FeeEngine.h`.
- **Risk limits, position limits, kill switch and rate limiting.** Admission-side controls;
  see `docs/Compliance.md` and `include/HierarchicalRiskManager.h`.
- **Market-data encoding.** ITCH/MoldUDP64/SBE wire formats and sequencing; see
  `Architecture.md`.
- **Journalling, replay and replication determinism.** See `docs/Verification.md`.
- **Session scheduling.** `SessionScheduler` drives the trading-state transitions that §7
  assumes; it does not participate in matching.

## 12. Maintaining this document

If you change a matching rule, change the rule here in the same commit. Two specific traps:

1. **Line numbers rot silently.** Every `file:line` above was verified at `05ea604`. A stale
   reference is worse than no reference, because it teaches the next reader to distrust all of
   them.
2. **Do not promote a defect to a rule.** §10 exists so that fixing a defect is a *correction*
   rather than a *breaking change*. Moving an entry from §10 into the rules because it has
   been there a long time is how a bug becomes a compatibility obligation.
