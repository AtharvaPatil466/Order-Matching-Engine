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
| 4 | Trailing stops / pegs | Full linear rescan of **every** trailing stop and **every** peg on **every** order submission and repriced replace; a repegged order **loses** time priority | Undocumented — no test pins the complexity or the priority loss ([§6](#6-trailing-stops-and-pegged-orders)) |
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

### Rule 3.1 — An iceberg shows at least 1/100 of its size, or it is rejected

`RejectReason::InvalidDisplayQty` when `displayQty < ceil(qty / kMaxIcebergSlices)`
(`kMaxIcebergSlices = 100`), which includes a zero display. A replace that grows the total
past `displayQty * kMaxIcebergSlices` is refused the same way, since a replace does not change
the display. A display size larger than the order is clamped down rather than rejected.
The floor bounds the work one aggressor can cause against one iceberg to 100 slices; a
display of 1 on a total of 1,000,000 used to be a million trades under the book lock
(MATCH-19). Pinned by `tests/IcebergMinDisplayTest.cpp`.

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
| `DecreaseAndCancel` (4) | Reduce both orders by the smaller remaining quantity; cancel whichever reaches zero (continuous trading; in an auction the older order is cancelled — see [B2](#b2--decreaseandcancel-never-decrements-the-incoming-order-fixed-in-continuous-trading)) |

### Rule 4.1 — An STP-cancelled order reports `CancelledBySTP`, never `Cancelled` and never `Filled`

A distinct terminal status exists precisely so an STP removal cannot be mistaken for either a
client cancel or an execution. Critically, `remainingQty` is **left intact** when the incoming
order is STP-cancelled: zeroing it made the order look fully filled, and it was reported as
executed in full. What an order executed is recorded in `cumQty` on every fill, never derived
from its size (see [rule 9.2](#rule-92--cancelreplace-loses-priority-on-a-price-change-or-a-same-size-re-add-keeps-it-on-a-shrink)).

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

The pairs are resolved **before** price discovery, not as they meet in the cross. Per
participant, buys and sells are walked most aggressive first (parked market orders in arrival
order, then limits in price-time order); every pair that crosses — either side priceless, or
bid ≥ ask — is resolved by the owner's mode with this mapping. `DecreaseAndCancel` cuts the
older order, which is where the cross's repeated decrements always ended. The published
indicative already excludes these orders, and `uncross()` cancels them (`CancelledBySTP`)
before it discovers the price, so a participant's own buy and sell can no longer move the
print or inflate the paired volume (roadmap 1.8-H5, AUCT-3; this closed defect B3).

> `planAuctionSelfCrosses()` and `stpCancelAuctionOrder()` in `src/OrderBook.cpp`

**Status: Deliberate — commented and pinned** (`tests/StpSemanticsTest.cpp`, auction section;
`tests/AuctionSelfCrossTest.cpp`).

---

## 5. Stop, stop-limit and market-if-touched

### Rule 5.1 — Stops are parked, not booked. They are invisible until elected.

A `Stop`, `StopLimit` or `MIT` order never enters `bids_`/`asks_` on arrival. It is appended
to a flat array of armed stops and `addOrder` returns immediately — no matching, no market
data, no depth contribution.

> `src/OrderBook.cpp:824-829` (`parkNonMatchingOrder`)
> `include/OrderBook.h:878` — `FixedVector<Order*, 16384> stopOrders_;`

The arming capacity is 16,384 per book. A 16,385th stop is rejected with `CapacityExhausted`
before it is acked, as is an order that would overflow any other parked list (auction market
orders, on-close orders, trailing stops). It used to be acked and then dropped by an unchecked
`FixedVector::push_back`, leaving an order in no list (roadmap 1.8-H6, AUCT-6). `push_back` is
now `[[nodiscard]]`. Pinned by `tests/AuctionCapacityTest.cpp`.

Admission checks the trigger fields, not `price`. A `Stop`, `StopLimit` or `MIT` with
`stopPrice <= 0`, and a `StopLimit` with `stopLimitPrice <= 0`, is rejected with
`InvalidPrice` before it is acked. `price` is not a limit for any of the three (Rule 5.2), so
it is not checked. A sell stop at 0 could never fire, a buy stop at 0 fired on the next print,
and a `StopLimit` at `stopLimitPrice` 0 rested at 0 once elected. `Stop` used to elect a
`Limit` at its `price`, so a stop-market sent with price 0 swept the book to zero and rested
its remainder as an ask at 0; roadmap 0.12 refused price 0 instead, and 1.6-F2 (OTM-4) made
`Stop` a stop-market. Pinned by `tests/StopMarketTest.cpp`.

> `validateOrderRequest()` in `src/OrderBook.cpp`

### Rule 5.2 — Election tests

Evaluated, at or through the level, against the lowest and highest price the triggering
order printed. A sweep prints monotonically in the aggressor's direction, so its first and
last prints bound every price it traded: `low = min(first, last)`, `high = max(first, last)`.
An order that printed nothing leaves both at the last trade price.

| Type | Buy elects when | Sell elects when |
|---|---|---|
| `Stop`, `StopLimit` | `high ≥ stopPrice` | `low ≤ stopPrice` |
| `MIT` | `low ≤ stopPrice` | `high ≥ stopPrice` |

`MIT` is the favourable-direction mirror of a stop and carries its trigger in the same field.
Only the last print used to be tested. That is harmless for a stop, whose trigger lies in the
sweep's direction, but an `MIT` triggers against it: a buy sweeping 100 then 110 never elected
the buy `MIT` at 105 that its 100 print touched (roadmap 1.6-F3, MATCH-17; pinned by
`tests/StopSweepOrderTest.cpp`).

> `runTriggerSweeps()` and `checkStopOrders()` in `src/OrderBook.cpp`; `match()` and
> `matchProRata()` return the sweep's first print

On election the order's type is rewritten in place:

| Armed as | Becomes | At price |
|---|---|---|
| `Stop` | `Market` | — unfilled remainder is cancelled, never rested |
| `StopLimit` | `Limit` | `stopLimitPrice` |
| `MIT` | `Market` | — unfilled remainder is cancelled, never rested |

`Stop` is a stop-market; `StopLimit` is the priced form.

> `checkStopOrders()` in `src/OrderBook.cpp`

### Rule 5.3 — A triggered stop's own fill CANNOT elect another stop in the same sweep

`checkStopOrders(low, high)` takes the triggering order's low and high print **by value**
(Rule 5.2) and is called once per sweep, from `runTriggerSweeps()`. A stop that fires inside
the sweep updates `lastTradePrice_`, but the sweep continues to evaluate every remaining stop
against the prices it was called with. A second-level election therefore defers to the next
arriving order (or repriced replace, Rule 5.8).

> `checkStopOrders()` in `src/OrderBook.cpp` — the signature
> `runTriggerSweeps()` — its only call site, reached from `addOrder` and `cancelReplace`

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
  `addOrder`, for an order that reaches matching, and from a `cancelReplace` that changes
  price, both in `Continuous` (Rule 5.8). A newly parked stop or peg, a FOK rejected for
  liquidity, an order the `minQty` screen finalized, cancels, modifies, same-price replaces,
  expiry sweeps and uncrosses do **not** drain the backlog. If order flow stops, a latched
  stop sits latched indefinitely.

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

### Rule 5.8 — Elections run after the triggering order is final

The stop, trailing-stop and peg sweeps (`runTriggerSweeps()`) run only once the order that
printed is finished: rested, filled, cancelled, or ended by self-trade prevention. `addOrder`
runs them after its finalize step, and so does a `cancelReplace` that changes price — the
repriced order is an aggressor and its prints elect stops like any other. Neither runs them in
`PreOpen` or an auction state (Rule 7.1).

So the aggressor's own report comes first: its rest or `Filled` update precedes the executions
of any stop it elected. The sweeps used to run between `match()` and the finalize step. A stop
elected by the incoming order's own print then matched a book that did not yet hold that
order's remainder, missed it, and rested; the remainder then rested through it, and nothing
matches two resting orders, so the book stayed crossed in `Continuous`. `cancelReplace` ran no
sweep at all, so a replace that printed through a trigger never elected it (roadmap 1.6-F3,
MATCH-5).

`OrderBook::validateIntegrity()` now refuses a crossed or locked book (best bid ≥ best ask) in
`Continuous`; the accumulation states, where crossing orders legitimately wait for the
uncross, pass.

> `runTriggerSweeps()`, its two call sites in `addOrder` and `cancelReplace`, and
> `validateIntegrity()` in `src/OrderBook.cpp`

**Status: Deliberate — commented and pinned.** `tests/StopSweepOrderTest.cpp` (incoming-print
and replace elections), `tests/IntegrityOracleTest.cpp` (crossed and locked books).

---

## 6. Trailing stops and pegged orders

### Rule 6.1 — Every trailing stop is rescanned on every order submission, unconditionally

**This is the answer to "does the engine rescan every trailing order when the reference price
moves?" — it rescans them whether or not the reference moved.**

`updateTrailingStops` is called from `runTriggerSweeps()` (Rule 5.8), after every order that
reaches matching and every repriced replace, whenever the book has ever traded
(`lastTradePrice_ > 0`) — not when a trade occurs, not when the price changes. An order that
rests without matching, or that is cancelled because it could not rest, still pays for a full
linear walk of `trailingStopOrders_`.

> `runTriggerSweeps()` in `src/OrderBook.cpp` — the guard is `if (lastTradePrice_ > 0)`, nothing more
> `updateTrailingStops()` — the sweep

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

A trailing stop parks with a reference: the last trade price, or its own `price` if the book
has not traded. Its `trailAmount` must lie inside `(0, reference)`, or it is rejected with
`InvalidPrice` at admission: a trail at or beyond the reference put the trigger, which is also
the elected limit, at or below zero, and a negative trail elected a buy at once at a negative
limit (roadmap 1.6-F2, OTM-4; pinned by `tests/StopMarketTest.cpp`).

For a sell trailing stop: each time the price makes a new high, `trailRefPrice` advances and
`stopPrice` becomes `trailRefPrice − trailAmount` (floored at zero, which the admission bound
keeps out of reach). The stop elects when the price falls to or through `stopPrice`. A buy
trailing stop is the mirror, ratcheting down. On election it becomes a `Limit` at `stopPrice`.

Each sweep tests the triggering order's first print and then the last, in the order they
traded, ratcheting and testing at each. A sell sweep printing 106 then 90 used to ratchet a
buy trailing stop's reference straight down to 90 without testing the 106 print against its
105 trigger (MATCH-17; pinned by `tests/StopSweepOrderTest.cpp`).

> `validateOrderRequest()`, `parkNonMatchingOrder()` and `updateTrailingStops()` in `src/OrderBook.cpp`

Once an election is latched, **the ratchet is skipped along with the trigger test**. This is
deliberate: a deferred election whose trail kept following the price could stop qualifying,
which would cancel it rather than defer it.

### Rule 6.3 — Stop elections feed trailing-stop elections within one submission, but not the reverse

An asymmetry worth stating because it is not obvious from either function:

```cpp
if (lastTradePrice_ > 0) {
    const Price last = lastTradePrice_;
    const Price first = firstPrint > 0 ? firstPrint : last;
    checkStopOrders(std::min(first, last), std::max(first, last));
    updateTrailingStops(first, lastTradePrice_);  // read AGAIN, after the above ran
}
```

> `runTriggerSweeps()` in `src/OrderBook.cpp`

`lastTradePrice_` is a member. `checkStopOrders` runs `match()` on each election, which
updates it. So **a trailing stop's second test is against the price after the stop cascade,
while ordinary stops were evaluated against the triggering order's prints only.** Trailing
stops therefore see one extra level of price propagation that ordinary stops do not, and there
is no reverse path — a trailing stop's fill cannot elect an ordinary stop until the next
submission.

**Status: Undocumented — no test pins this, may be incidental.** Nothing comments on the
re-read, and it is equally consistent with a deliberate one-way propagation and with nobody
having noticed that the second argument is evaluated later.

### Rule 6.4 — Every pegged order is repriced on every order submission, and a repeg loses time priority

`updatePeggedOrders` runs from `runTriggerSweeps()` (Rule 5.8) whenever any pegged order
exists, recomputing every peg's target from the current book:

| Peg type | Target |
|---|---|
| `MidPeg` | `mid + pegOffset`, where `mid = bb + (ba − bb)/2`, rounded toward the peg's own side (down for a buy, up for a sell) |
| `PrimaryPeg` | same-side touch `+ pegOffset` |

`bb` and `ba` are the best bid and ask **among orders that are not pegged**, so no peg,
itself included, moves its own reference. A peg with no reference (a `MidPeg` needs both
sides, a `PrimaryPeg` its own) keeps its price; at entry that is the order's `price`. The mid
rounds toward the peg's side so that a sell `MidPeg` on a one-tick spread lands on the ask,
not on the bid.

**A peg never trades on placement or reprice, only as a resting order.** At entry, a peg whose
target is at or through the opposite touch, or not above zero, is rejected with
`InvalidPrice`, as is a `pegOffset` beyond ±(2^32 − 1), which keeps `reference + offset` clear
of overflow. A reprice that reaches the opposite touch is clamped one tick behind it instead
of matching; one that would land at or below zero is skipped. (The opposite touch here is the
real best price on that side, pegs included.)

These rules are roadmap 1.6-F4 (STP-13). Before them, a `PrimaryPeg` buy +30000 on a
990000/1010000 market rested at 1020000, above the ask; a `PrimaryPeg` that counted itself as
the touch climbed one offset per later order; a `MidPeg` that counted itself in the mid
drifted; all three were crossed books ([B4](#b4--pegged-orders-can-rest-at-a-crossing-price-fixed)).
Roadmap 0.12 refused `Pegged` at admission until then.

> `bestNonPegPrice()`, `pegPrice()`, `behindOppositeTouch()`, `admitPeg()`,
> `restPeggedOrder()` and `updatePeggedOrders()` in `src/OrderBook.cpp`

**Complexity per sweep: O(P)**, P = number of resting pegs, cap 16,384, plus, for each peg, a
walk past any pegs resting at the touch to find the first non-pegged order (the code's
ponytail note names a per-side best-non-peg tracker as the upgrade path).

When a peg's target differs from its current price, the order is **removed from the book,
repriced, given a fresh timestamp, and re-inserted** — and insertion appends to the tail of
the destination level.

> `updatePeggedOrders()` — `removeFromBook` → `order->price = newPrice` →
> `order->timestamp = nowNs()` → `addToBook`
> `include/FlatPriceMap.h:146,151` — insertion is `push_back`

**So yes: a repegged order loses time priority, every time it repegs.** A peg that follows a
moving touch is perpetually at the back of its queue.

The depth change is published as a `Delete` at the old price and an `Add` at the new one, so
incremental subscribers can follow it.

**Status: the pricing is deliberate — commented and pinned** (`tests/PegOrderTest.cpp`:
through-the-touch reject, no ratchet, no mid drift, the clamp, the rounding;
`tests/OrderTypeAdmissionTest.cpp`). The offset bound is commented, not pinned. **The priority
loss is undocumented — no test pins it, may be incidental.** The comment in
`updatePeggedOrders()` explains why the *market-data events* are published; nothing explains
or pins the *priority* consequence.

**How venues specify it.** Losing priority on a reprice is the common treatment — a repeg is
a new price, and price-time priority is defined per price level, so there is nowhere for the
old priority to live. I do not know of a venue that publishes a "sticky priority across
repegs" rule, and this engine's behaviour is unsurprising. What is unusual is the *trigger*:
see Rule 6.5.

### Rule 6.5 — Pegs only follow the market when an order arrives or is repriced

`updatePeggedOrders` is called only from `runTriggerSweeps()`, which runs after an order that
reaches matching in `addOrder` and after a `cancelReplace` that changes price, in `Continuous`
(Rule 5.8). It is **not** called from `cancelOrder`, `modifyOrder`, a same-price
`cancelReplace`, `expireOrders`, `uncross` or the kill switch, nor when the arriving order is
itself a peg or a parked stop.

> `runTriggerSweeps()` in `src/OrderBook.cpp` — the sole call site

So a cancel that removes the best bid moves the touch, and every primary-pegged buy is now
pegged to a price that no longer exists — until some unrelated order is submitted or
repriced. In a quiet book that can be an arbitrarily long time. A cancel only widens the
spread, so the stale peg is left behind the market, not through it.

> `tests/MarketDataL2ReconstructionTest.cpp:219` acknowledges this in passing:
> *"updatePeggedOrders runs on the NEXT submission"*.

**Status: Undocumented — no test pins this, may be incidental.** One test comment notices the
behaviour while testing something else. Nothing asserts it, and nothing explains it. This is
the rule most likely to surprise a participant.

---

## 7. Auctions and the volatility uncross

### Rule 7.1 — Auction states accumulate; they do not match

`PreOpen`, `AuctionOpen`, `AuctionClose` and `VolatilityAuction` all admit orders into the
book without running continuous matching. All trades are produced by a single `uncross()` at
the session boundary, at one price.

Nothing else matches in these states. An arriving order does not run the stop / trailing-stop
sweep, so a stop is not elected on the stale last print and matched mid-auction; a
`cancelReplace` that crosses rests at its new price and waits for the uncross. Both used to
print continuous trades during the call period (roadmap 1.8-H4, MATCH-6).

Leaving an accumulation state for `Continuous` or `PostClose` runs `uncross()` first, under
the same book lock as the flip: the flip *is* the opening, reopening or closing cross. The open
used to be two calls — `uncross`, then a `setTradingState(Continuous)` that only assigned the
field — and orders arriving between them reached continuous trading crossed, or parked as
market orders until the close (AUCT-4, MATCH-12, TLA-3). `SessionScheduler` now issues the
flip alone. Entering `Halted` does not cross. Pinned by `tests/AuctionOpenAtomicTest.cpp`.

Every trading-state change and every explicit uncross is journaled (`TradingStateChange`,
`Uncross`, added in journal format v2; the current format, v3, still reads v1 and v2 and marks an
older file current before appending), and
replay applies them in journal order, so recovery re-runs each auction exactly. Through
`MatchingEngine` they are applied and journaled by the book's owning worker, in order with that
book's orders; a checkpoint writes each non-`Continuous` book's state before its orders. Before
this, replay ran every order accumulated in an auction as a continuous one (roadmap 1.8-H1,
AUCT-1; pinned by `tests/AuctionJournalReplayTest.cpp`). Leaving `VolatilityAuction` for
`Continuous` re-anchors the breaker reference to the reopening print, however it is reached.

> `setTradingStateLocked()` and the `inAuction` gates in `addOrder` / `cancelReplace`
> `include/Types.h:166-185` — the trading-state enum and what each state admits

Market orders submitted during an auction cannot rest in a price-indexed book, so they are
parked separately and folded into discovery.

> `src/OrderBook.cpp:809-822`

### Rule 7.2 — The clearing price is discovered by a four-step tie-break cascade

Candidate prices are every populated limit level on either side, merged from the two level
maps into one ascending list with no cap. For each candidate `p`:

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

**Zero imbalance with no reference clears at the highest tied price.** A candidate with zero
imbalance counts as a buy surplus (`cumBuy >= cumSell`), so when every tied candidate pairs its
whole volume and no reference price is set, step 4 keeps moving up and the highest of them wins.
This is the rule, kept deliberately; it is the same code path as [B7](#b7--the-auctions-final-tie-break-reads-the-incumbents-surplus-side-not-the-candidates),
which reads the surplus side of the incumbent rather than the candidate. Pinned with the
step-3 cases by `tests/AuctionReferencePriceTest.cpp`.

> `discoverUncrossPrice()` in `src/OrderBook.cpp`, with the four cascade steps named in a
> comment

The candidates used to be a `FixedVector<Price, 4096>` filled bids first, so past 4096
levels the ask levels were dropped and the true clearing price was never evaluated (roadmap
1.8-H6, AUCT-6; pinned by `tests/AuctionCapacityTest.cpp`).

Parked market orders participate at **every** candidate price, since they have no limit to
anchor on, and so add uniformly to both cumulants (`src/OrderBook.cpp:2759-2765`).

**Market orders alone cross at the last trade price.** With no limit level on either side
there is no candidate price, but parked market orders on both sides still pair: they cross for
`min(buy, sell)` at the last trade price (the book keeps no separate prior close), and the
surplus side's remainder is cancelled. Before any trade there is no reference and nothing
crosses. They used to be cancelled unfilled whatever the reference (roadmap 1.8-H7, AUCT-14).

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

**Complexity.** Each level's quantity is summed once and the cumulative curves come from one
ascending sweep: O(orders + L), with a heap-allocated list of `L` levels per call.

**Status: Deliberate — commented, thinly pinned.** The cascade is named in a comment at both
the declaration (`include/OrderBook.h:692-697`) and the implementation.
`tests/TestAuctionSession.cpp` covers the lifecycle; `tests/AuctionReferencePriceTest.cpp`
discriminates step 3 from step 4 on a zero-imbalance tie.

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

Parked market orders (and released `MOC`) outrank every limit on their side, whatever the
limit's price: each side's next order is its oldest unfilled parked market order, and only
when none is left the front of the best limit level. They never enter the book. They used to
be inserted at the discovered price, at the back of that level, so a later limit there — or
any limit priced through the cross — filled first and the market order was cancelled
unfilled (roadmap 1.8-H2, AUCT-2; pinned by `tests/AuctionMarketPriorityTest.cpp`). Any
market-order remainder is cancelled after the cross rather than left resting — a market order
is auction-only and does not get to become a limit at the clearing price. Unfilled `LOC`
orders are cancelled likewise (`cancelLocOrders()`).

An `MOC` / `LOC` sent before the close waits in `onCloseOrders_` until the book enters
`AuctionClose`. Any still waiting when the book enters `PostClose` — the close never ran: a
halt over it, or a volatility auction run into it — is cancelled. It used to wait for the next
day's close and execute there (roadmap 1.8-H3, AUCT-8 / OTM-9; pinned by
`tests/OnCloseSessionEndTest.cpp`).

> `uncross()` in `src/OrderBook.cpp` — `nextParked` and the step-3 cancel pass

**Status: Undocumented — no test pins this, may be incidental.** That the cross ignores
`matchAlgorithm_` and is always FIFO is a real semantic choice — a pro-rata product would
plausibly want pro-rata allocation at the cross — and nothing states it or tests it.

### Rule 7.4 — The breaker trips on the first fill's price and starts a timed volatility auction

In `Continuous`, before an arriving order matches, the book takes the price its **first fill**
would print at — the best opposite level it crosses, stepping over its owner's own orders
where self-trade prevention would — and compares it with `referencePrice_`. If it deviates by
more than `cbThreshold_`, the book enters `VolatilityAuction` before anything prints and a
`breaker_trip` event is logged. The order becomes the auction's first arrival: a limit rests,
a market order is parked for the reopening cross, an IOC or FOK is cancelled. Only the first
fill is tested; a sweep that starts in band runs on. A passive order trips nothing however far
it is from the market, and nothing trips in an auction state.

The breaker used to test each order's **limit** price and switch the book on a breach: one
passive order far from the market stopped continuous trading for the whole symbol, and nothing
reopened it (PRIOR-5, AUCT-8). The 0.14 stop-gap then rejected such orders one by one. A
**replace** is still refused on its new limit price (`VolatilityCircuitBreaker`): it is never
auctioned, so a crossing replace cannot print out of band either (roadmap 1.8-H3; pinned by
`tests/VolatilityBreakerTest.cpp`).

> `firstFillPrice` and `tripBreakerOnFirstFill` in `src/OrderBook.cpp`

**The auction reopens on a timer.** A book in `VolatilityAuction` — tripped or entered with
`setTradingState` — reopens after the dwell, 5 minutes by default (`volatility_auction_dwell_ms`).
The dwell runs on the engine's expiry clock and starts at the first expiry tick after the book
entered the state, so it can run up to one tick (1 s) long. The engine's periodic expiry hook
drives it on the book's owning thread: the `ExpireCheck` sweep in async mode, `expireOrders()`
— `GatewayServer`'s tick — in sync mode. The reopen is an ordinary journaled
`TradingStateChange` to `Continuous`, which runs the uncross and re-anchors `referencePrice_`
to the reopening print. `resumeVolatilityAuction()` reopens at once.

**A trip is journaled.** The engine writes a `TradingStateChange(VolatilityAuction)` record
ahead of the order that tripped the breaker, and replay never evaluates the breaker itself, so
recovery reproduces the trip even with another threshold, or with the reference a checkpoint
does not carry. The dwell clock is not journaled: after a restart into a volatility auction the
dwell starts again.

### Rule 7.5 — What this engine calls LULD is a single fixed percentage band, not US-equities LULD

The name appears throughout (`OutsidePriceBand`, `priceBandPct_`, `LULDManager`), so the
distance from the real thing needs stating:

| US-equities LULD (Reg NMS Plan) | This engine |
|---|---|
| Reference price is a rolling 5-minute average | `referencePrice_` is seeded from the first limit price seen and re-anchored only on a volatility-auction reopen (`src/OrderBook.cpp:591-595`, `src/OrderBook.cpp:2881`) |
| Band percentage is tiered by price and by time of day, and doubles near the open/close | One flat `priceBandPct_` for the whole session |
| A quote at the band creates a *Limit State*; 15 seconds in Limit State becomes a 5-minute pause | No Limit State; a would-be print outside the breaker threshold starts a volatility auction at once |
| A pause reopens via an auction on a published schedule | The volatility auction reopens with a cross after a fixed dwell (5 minutes by default) |
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

### Rule 7.7 — The session transition table is enforced

`OrderBook::setTradingState` refuses a transition that is not on this table: the book stays
where it was, a `trading_state_refused` event is logged, and the engine journals nothing.
`setTradingStateBatch` and `beginTradingSession` return how many books are now in the
requested state.

| From | To |
|---|---|
| any state | `Halted` |
| `PreOpen` | `Continuous` (opening cross), `AuctionOpen` |
| `AuctionOpen` | `Continuous` (cross), `AuctionClose` |
| `Continuous` | `AuctionOpen` (intraday call auction), `AuctionClose`, `VolatilityAuction`, `PostClose` (no closing auction), `PreOpen` only before the book's first session |
| `AuctionClose` | `PostClose` (closing cross) |
| `VolatilityAuction` | `Continuous` (reopening cross), `AuctionClose`, `PostClose` |
| `Halted` | the state it was halted from (resume), `PostClose` (the session ends while halted) |
| `PostClose` | `PreOpen` only for a later trading date |

`PreOpen` begins a session. Books boot in `Continuous`, so the first `PreOpen` may come from
there. After that, a book reaches `PreOpen` only from `PostClose`, through `beginSession` /
`MatchingEngine::beginTradingSession` with a trading date (days since 1970-01-01) later than
the date of the session it closed. An undated `PreOpen` (`setTradingState(PreOpen)`) never
reopens a closed book. `SessionScheduler`'s pre-open passes its trading date, so a same-date
`resetSession()` re-runs the scheduler's phases while the books stay in `PostClose`. The date is
journaled with the `PreOpen` record and kept by a checkpoint, so a restarted engine still
refuses a second session on that date. Replay applies the journal without checking the table,
because only applied transitions are journaled.

A halt is a pause, not a route. `Halted` resumes only to the state the book was halted from,
so a halted `AuctionClose` resumes as `AuctionClose`, a halted `PostClose` stays closed, and a
halted opening or volatility auction resumes that auction and still ends in its uncross.
`Halted -> Continuous` is legal only for a book halted from `Continuous`. The session may also
end while halted (`Halted -> PostClose`, with no closing cross). Without this rule a halt was a
detour around the table: `Continuous -> AuctionClose -> Halted -> Continuous` abandoned the
closing auction and `PostClose -> Halted -> Continuous` reopened a closed market. The pre-halt
state is rebuilt by journal replay, and a checkpoint writes it just before the `Halted` record.

Off the table: `PostClose -> Continuous` reopened a closed market, `AuctionClose -> Continuous`
abandoned a closing auction, and `PreOpen` / `AuctionOpen -> PostClose` would print a cross at
the close of a session that never opened. `spec/Auction.tla`'s `LegalEdges` is this table,
edge for edge, with its side conditions, and `NoReopenAfterClose` (in `Auction.cfg`) checks that
once a session reaches its closing auction or close nothing trades or auctions until the next
`PreOpen`; `AuctionBrokenHalt.cfg`, which lets a halt resume to `Continuous` from anywhere, must
fail it.

> `OrderBook::transitionAllowed` in `src/OrderBook.cpp`

**Status: Deliberate — commented and pinned** (`tests/SessionTransitionTest.cpp`; decision 7,
roadmap 1.8-H4).

---

## 8. Time in force and expiry

### Rule 8.1 — GTD and DAY expiry is a full linear scan on a 1 Hz background thread

**This is the answer to question 6: it is neither a timer wheel nor a priority queue. It is a
scan.**

The mechanism end to end:

1. `startExpiryTimer(intervalMs)` spawns a thread that sleeps, wakes, and calls
   `expireOrdersFromClock()`. **Default interval: 1000 ms.** `OrderEngine` starts it after
   `startAsync()`. `GatewayServer` does not — its engine is synchronous, so it calls
   `expireOrdersFromClock()` once a second from the gateway's event thread instead, between
   orders. `expiryTime` is **Unix-epoch nanoseconds, wall clock** (`system_clock`).
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

There is a separate session-end sweep, `cancelDayOrders()`, which cancels every `DAY` order
regardless of `expiryTime`. It runs only from `gracefulShutdown(/*cancelDayOrders=*/true)` —
an explicit end-of-day stop — and **no shipped binary makes that call**.

> `src/MatchingEngine.cpp` — `cancelDayOrders()`, and the `cancelDayOrdersNow` branch of
> `gracefulShutdown`

**Status: Decided — a process stop is not a session end.** `gracefulShutdown()` used to run
the sweep unconditionally, and it is what `OrderEngine` calls on SIGTERM, so every deploy,
config reload or crash-restart cancelled every client's `DAY` orders mid-session. It now
defaults to keeping them, and a restart restores them (`GracefulShutdownTest`, "A routine
stop keeps DAY orders").

The session end is a separate, explicit command: `MatchingEngine::endTradingSession(symbols)`
moves those symbols to `PostClose` (running the closing cross) and then cancels and journals
every `DAY` order on them, and only them. `SessionScheduler`'s close calls it (roadmap 1.8-H8,
1.4-D9; pinned by `tests/SessionEndTest.cpp`). `OrderEngine` runs the scheduler only when
started with `--session-schedule PRE,OPEN,CLOSE_AUCTION,CLOSE` (HH:MM UTC, or
`OB_SESSION_SCHEDULE`, or `session_schedule` in the `--config` file), and runs it Monday to
Friday by UTC date; without it, which is the default, nothing ends a session and a `DAY`
order with `expiryTime == 0` behaves exactly like `GTC`, as before. `GatewayServer` never
runs one. Both binaries run the expiry sweep (Rule 8.1), so a `DAY` order that carries an
`expiryTime` expires like `GTD` either way.

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

`modifyOrder`'s `newQty` is the new **leaves**, unlike `cancelReplace` (rule 9.2). The order's
total drops by the same cut, so `cumQty + remainingQty == initialQty` still holds and the cut
is never reported as a fill. Before 1.5-E1 the total stayed put, and a 50 cut to 30 reported
`filledQty` 20 on its cancel with nothing traded (GTDIFF-1).

> `OrderBook::modifyOrder` in `src/OrderBook.cpp`

An iceberg's slice is clamped to the new leaves, and only the *displayed* shrink is published.
A modify-down that consumes hidden reserve without touching the slice emits nothing the market
can see (`src/OrderBook.cpp:1936-1952`).

### Rule 9.2 — `cancelReplace` loses priority on a price change or a same-size re-add, keeps it on a shrink

First, what `newQty` means. It is the order's new **total** (FIX `OrderQty`), shares already
executed included, not its new leaves (roadmap 1.5-E1, MATCH-4). Every order records what it
executed in `cumQty`, and `cumQty + remainingQty == initialQty` holds on every live order.

| `newQty` against `cumQty` | Result |
|---|---|
| `newQty <= cumQty` | Refused with `InvalidQuantity` (FIX OrdRejReason 13, OUCH invalid-quantity reject). Nothing is left to work; the order is untouched |
| `newQty > cumQty` | Accepted. `initialQty = newQty`, `remainingQty = newQty - cumQty`. Growth and shrink both work on a partially filled order |

So an order for 100 with 60 filled, replaced "to 100", has 40 left and executes 100 in all.
Before 1.5-E1, `newQty` became the leaves while the size stayed put: the same replace
executed 160, and growing an unfilled 100 to 120 reported `filledQty` 18446744073709551596 on
its cancel. Roadmap 0.11 had stopped that by refusing any quantity change on a partially
filled order and any growth; that restriction is gone. Risk limits judge the new total, as
they would a new order of that size (rule 9.3).

Every ingress passes the client's quantity through unchanged: FIX `OrderQty` (38) on a `G`,
OUCH `Shares` on a Replace, and the native gateway's `newQty`. Journal replay passes the
recorded `newQty` back through the same contract. A `CancelReplace` record says which meaning
it carries: those written since journal format v3 are flagged as totals, and an older unflagged
one meant the new leaves, so replay restates it as `cumQty + newQty` at that point
(`tests/LegacyReplaceReplayTest.cpp`). An older build refuses a v3 file rather than read totals
as leaves. And a checkpoint's Snapshot records carry
each order's `cumQty`, so a replace after a restore is applied against the fills the order
really had (`tests/ReplaceAfterCheckpointTest.cpp`).
Pinned by `tests/ReplaceQuantityTest.cpp`.

| Change | Priority |
|---|---|
| Price changed | Lost. Removed, repriced, timestamped, matched if it now crosses, then re-rested at the tail. |
| Same price, leaves down | **Kept.** In-place reduction. |
| Same price, leaves the same or up | Lost. Removed and re-added at the tail with a fresh timestamp. |

A repriced order is **off the book while it matches** and acts as an aggressor. Its display
entry is removed before the match so its fills surface through the resting maker's execution
only, as any aggressor's do.

> `src/OrderBook.cpp:2050-2059`

A repriced iceberg that rests afterwards is re-sliced to `min(remainingQty, displayQty)`,
the same way as on every other rest path. Before this, it kept its old slice. When the match
left less than that slice, the book advertised more than the order held, and the next
aggressor's fill wrapped `remainingQty` to ~2^64 (roadmap 0.11, MATCH-3). Pinned by
`tests/ReplaceIcebergSliceTest.cpp`.

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

`minQty` is checked once at admission and the stored field is never read again. Matching
enforces no per-fill minimum.

It is checked against what the sweep would actually **fill**, not against everything at a
crossing price. `sweepFillQty()` dry-runs the sweep over the opposite book, at crossing prices
only, and at each of the participant's own resting orders makes the sweep's own self-trade
decision, through the same `checkSMP` and `SelfTradeProtection::check` calls `match()` makes:
the default mode, `CancelIncoming` and `CancelBoth` end the count there; `CancelResting` steps
past the order; `DecreaseAndCancel` shrinks the incoming order by it with no fill. It follows
the sweep's order: price-time takes an iceberg's slice in queue order and its reserve only
after the rest of the level, so an own order that ends the sweep stops the reserve too;
pro-rata settles a level's own orders before allocating, so one that ends the sweep ends it
before that level fills. An iceberg's whole reserve counts. The screen used to add up every
crossing order, the participant's own included, so an IOC with `minQty` 20 behind 5 of other
liquidity and its own order filled 5 (roadmap 1.6-F5, GTDIFF-2); and it counted only an
iceberg's displayed slice, so an IOC 50 `minQty` 10 against an iceberg of 100 showing 5 was
cancelled unfilled (OTM-11).

If the dry run fills less than `minQty`: an `IOC` or `Market` order is cancelled untraded; a
**crossing** limit order is cancelled (resting it would leave bid ≥ ask, a locked book); a
**non-crossing** limit order rests and waits.

**A `Market` order with `minQty` is an IOC with `minQty`.** It is counted at every level; one
that misses its minimum is cancelled untraded, and one that meets it trades and has its
remainder cancelled. The screen used to read its price (0) as a limit, so a buy saw no
liquidity and was **rested in the book as a bid at 0**, where it later printed at 0; roadmap
0.12 rejected `Market` + `minQty` outright until 1.6-F2 (OTM-2).

**Not in an auction state.** In `PreOpen` and the auction states the screen does not run: the
order rests and takes part in the uncross, **which does not apply `minQty`**. A crossing
`minQty` limit used to be cancelled there, as if it would lock a book that is allowed to cross
(OTM-11).

**FOK uses the same dry run.** A FOK is rejected with `FOKInsufficientLiquidity` unless
`sweepFillQty()` fills its whole size, so a FOK whose own resting order would end the sweep,
or that would lose quantity to a `DecreaseAndCancel` decrement, is rejected rather than filling
part of its size (GTDIFF-2).

> `sweepFillQty()`, `screenFOK()` and `screenMinQty()` in `src/OrderBook.cpp`, with the
> locked-book reasoning in `screenMinQty()`

**Status: Deliberate — commented and pinned.** `tests/FokStpLiquidityTest.cpp` (each STP mode,
the iceberg cases, pro-rata, the auction case), `tests/MarketMinQtyTest.cpp`.

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

### B2 — `DecreaseAndCancel` never decrements the incoming order (fixed in continuous trading)

Fixed by roadmap 1.5-E5 (MATCH-10) on the price-time and pro-rata paths: both orders shrink
by the smaller remaining quantity and whichever reaches zero is cancelled, the CME / Nasdaq
decrement-and-cancel. A decrease is not a fill, so each order's total shrinks with its leaves.
It used to reduce only the resting order, so the sweep met it again and again until it was
gone: a resting 100 met by an incoming 30 ended at 0, not 70, and the incoming order traded on
at full size. Pinned by `DecreaseAndCancelDecrementsBothOrders` in `tests/StpSemanticsTest.cpp`
on both paths.

The auction still cancels the older order outright: its self-cross plan treats
`DecreaseResting` like `CancelResting` ([Rule 4.4](#rule-44--in-an-auction-cross-incoming-means-the-later-arriving-order-by-timestamp)).

### B3 — Auction price discovery runs before auction STP (fixed)

Fixed by roadmap 1.8-H5: self-crossing pairs are resolved before discovery
([Rule 4.4](#rule-44--in-an-auction-cross-incoming-means-the-later-arriving-order-by-timestamp)).
Discovery used to count them and STP removed them only in the execution loop, so a
participant's buy and sell of 1000 at 99 moved an honest 105 cross to 99 and published 1100
paired instead of 100.

### B4 — Pegged orders can rest at a crossing price (fixed)

Fixed by roadmap 1.6-F4 (STP-13); the rules are now [Rule 6.4](#rule-64--every-pegged-order-is-repriced-on-every-order-submission-and-a-repeg-loses-time-priority).
Neither `restPeggedOrder` (placement) nor `updatePeggedOrders` (repeg) checked whether the
computed price crossed the opposite side, and neither calls `match()`, and `pegOffset` was
unvalidated. A buy `MidPeg` with an offset larger than half the spread, or a buy `PrimaryPeg`
with one larger than the spread, rested above the best ask without matching: a locked or
crossed book with no trade. A `PrimaryPeg` that counted its own price as the touch ratcheted
one offset per later submission and walked through the ask, and a `MidPeg` that counted itself
in the mid drifted. Roadmap 0.12 refused `Pegged` at admission in the meantime.

Now the reference ignores pegged orders, a peg at or through the opposite touch at entry (or
with an offset beyond ±(2^32 − 1)) is rejected with `InvalidPrice`, and a reprice that reaches
the opposite touch is clamped one tick behind it. `Pegged` is admitted again. Pinned by
`tests/PegOrderTest.cpp`; `validateIntegrity()` now refuses a crossed book in `Continuous`
(Rule 5.8).

### B5 — Triggered stops and pegs bypass the price band and the circuit breaker

The price band is checked only for `Limit`, `IOC`, `FOK`, `PostOnly`, `Iceberg` and `Hidden`;
the circuit breaker tests the first fill of an order arriving through `addOrder` (Rule 7.4).

> `admitPriceBand` (band) and `tripBreakerOnFirstFill` (breaker) in `src/OrderBook.cpp`

A `Stop` is admitted without either check, because at admission it carries a trigger rather
than a tradeable price — reasonable. But when it elects, `checkStopOrders` rewrites its type
to `Limit`/`Market` and calls `match()` **directly**, never re-entering `addOrder`.

> `checkStopOrders()` in `src/OrderBook.cpp`

So a triggered stop executes with **no band check and no breaker check, ever**. A stop
cascade can therefore print straight through a LULD band that would have rejected the same
order submitted directly — which is the exact scenario price bands exist to contain. The same
holds for `TrailingStop` (`updateTrailingStops()`) and for `Pegged`, which is excluded from the
band list; a peg's only price check is the opposite touch (Rule 6.4).

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

The same read decides the zero-imbalance case: a candidate that pairs its whole volume counts as
a buy surplus, so with no reference price a zero-imbalance tie clears at the highest tied price.
That consequence is kept as the rule (Rule 7.2).

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
  assumes; it does not participate in matching. Its default clock reads the wall clock once
  and then advances on `steady_clock`, and within a trading date it never runs backwards: a
  new session starts only when the trading date changes (`setTradingDateFn`; default the UTC
  day). It used to treat any backward clock reading as midnight, so a 1 ms NTP step in the
  closing auction replayed the day, opening uncross included (roadmap 1.8-H7, AUCT-7; pinned
  by `tests/SessionClockStepTest.cpp`).

## 12. Maintaining this document

If you change a matching rule, change the rule here in the same commit. Two specific traps:

1. **Line numbers rot silently.** Every `file:line` above was verified at `05ea604`. A stale
   reference is worse than no reference, because it teaches the next reader to distrust all of
   them.
2. **Do not promote a defect to a rule.** §10 exists so that fixing a defect is a *correction*
   rather than a *breaking change*. Moving an entry from §10 into the rules because it has
   been there a long time is how a bug becomes a compatibility obligation.
