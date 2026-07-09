---- MODULE MatchingEngine ----
(***************************************************************************)
(* TLA+ Specification for the Order Matching Engine                        *)
(*                                                                         *)
(* Roadmap Phase 2, Week 5: Matching Engine TLA+ Specification             *)
(*                                                                         *)
(* Models a simplified order book. The Next-state relation has exactly     *)
(* five actions: PlaceLimit, Match, CancelOrder, ExpireGTD, AdvanceTime.   *)
(*   - Two price levels, two participants.                                 *)
(*   - PlaceLimit places LIMIT orders only. The OrderTypes set also        *)
(*     names Market/IOC/FOK/GTD, but no action submits those types.        *)
(*   - Match models the matching/cross layer (P1-6): when the book is      *)
(*     crossed (best bid >= best ask) the FIFO-earliest resting order at   *)
(*     the best bid trades against the FIFO-earliest resting order at the  *)
(*     best ask for min(remainingQty). Fully-filled orders leave the book; *)
(*     partial fills update remainingQty. The spec now proves price-time   *)
(*     execution, not just book-management safety.                         *)
(*   - Per-order accounting fields filledQty and cancelledQty are carried  *)
(*     so that qty = remainingQty + filledQty + cancelledQty always holds  *)
(*     for every order (this is what makes conservation checkable even     *)
(*     after cancellation, which zeroes remainingQty).                     *)
(*                                                                         *)
(* Non-vacuity (P1-7): CONSTANT BROKEN_NO_FIFO toggles a bug-injected      *)
(* variant of Match that consumes a NON-earliest resting order at the best *)
(* price. Under MatchingEngineBroken.cfg (BROKEN_NO_FIFO = TRUE) TLC finds *)
(* a reachable state violating FIFOExecution — proving Match actually      *)
(* fires and the invariant is not vacuously true. See MatchingEngine.cfg   *)
(* (BROKEN_NO_FIFO = FALSE) for the real spec.                             *)
(*                                                                         *)
(* Invariants proved:                                                      *)
(*   - NoNegativeQuantity                                                  *)
(*   - FIFO_Preservation                                                   *)
(*   - MatchingConservation  (P1-6, checked)                               *)
(*   - FIFOExecution         (P1-6/P1-7, checked; non-vacuous)             *)
(*   - Quantity_Conservation (legacy, defined but not in the .cfg)         *)
(*   - GTD_Expiry_Correctness                                             *)
(***************************************************************************)

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    MaxOrders,      \* Maximum number of orders to place
    Participants,   \* Set of participant IDs {1, 2}
    Prices,         \* Set of valid prices {100, 200}
    MaxQty,         \* Maximum order quantity
    MaxTime,        \* Maximum simulation time
    BROKEN_NO_FIFO  \* Non-vacuity flag (P1-7). FALSE = real spec: Match
                    \* consumes the FIFO-earliest resting order at the best
                    \* price. TRUE = bug-injected: Match may consume a
                    \* NON-earliest resting order, so FIFOExecution is
                    \* violated and TLC must report a counterexample. This
                    \* proves the invariant has teeth (Match actually fires).

VARIABLES
    bids,           \* Map: Price -> Sequence of orders (FIFO)
    asks,           \* Map: Price -> Sequence of orders (FIFO)
    orders,         \* Map: OrderId -> Order record
    trades,         \* Sequence of executed trades
    nextOrderId,    \* Next order ID to assign
    nextTradeId,    \* Next trade ID to assign
    seqNum,         \* Global sequence number
    currentTime     \* Simulated clock

vars == <<bids, asks, orders, trades, nextOrderId, nextTradeId, seqNum, currentTime>>

(* ─── Types ──────────────────────────────────────────────────────── *)

OrderTypes == {"Limit", "Market", "IOC", "FOK", "GTD"}
Sides == {"Buy", "Sell"}
Statuses == {"New", "PartialFill", "Filled", "Cancelled"}
Qtys == 1..MaxQty
Times == 0..MaxTime

NullOrder == [id |-> 0, participant |-> 0, side |-> "Buy",
              price |-> 0, qty |-> 0, remainingQty |-> 0,
              filledQty |-> 0, cancelledQty |-> 0,
              type |-> "Limit", status |-> "New",
              timestamp |-> 0, expiryTime |-> 0]

(* ─── Helpers ────────────────────────────────────────────────────── *)

\* Best bid: highest price with non-empty queue
BestBid == IF \E p \in DOMAIN bids : bids[p] /= <<>>
           THEN CHOOSE p \in DOMAIN bids :
                    bids[p] /= <<>> /\
                    \A q \in DOMAIN bids : bids[q] /= <<>> => q <= p
           ELSE 0

\* Best ask: lowest price with non-empty queue
BestAsk == IF \E p \in DOMAIN asks : asks[p] /= <<>>
           THEN CHOOSE p \in DOMAIN asks :
                    asks[p] /= <<>> /\
                    \A q \in DOMAIN asks : asks[q] /= <<>> => q >= p
           ELSE MaxQty * 1000  \* Sentinel "no ask"

\* Total quantity on a side at a price
SideQtyAt(book, price) ==
    IF price \in DOMAIN book /\ book[price] /= <<>>
    THEN LET orderIds == book[price]
         IN  \* Sum remaining quantities
             LET SumQty[i \in 0..Len(orderIds)] ==
                 IF i = 0 THEN 0
                 ELSE SumQty[i-1] + orders[orderIds[i]].remainingQty
             IN SumQty[Len(orderIds)]
    ELSE 0

(* ─── Init ───────────────────────────────────────────────────────── *)

Init ==
    /\ bids = [p \in Prices |-> <<>>]
    /\ asks = [p \in Prices |-> <<>>]
    /\ orders = <<>>
    /\ trades = <<>>
    /\ nextOrderId = 1
    /\ nextTradeId = 1
    /\ seqNum = 1
    /\ currentTime = 0

(* ─── PlaceLimit ─────────────────────────────────────────────────── *)

PlaceLimit(participant, side, price, qty) ==
    /\ nextOrderId <= MaxOrders
    /\ price \in Prices
    /\ qty \in Qtys
    /\ participant \in Participants
    /\ LET oid == nextOrderId
           order == [id |-> oid, participant |-> participant,
                     side |-> side, price |-> price,
                     qty |-> qty, remainingQty |-> qty,
                     filledQty |-> 0, cancelledQty |-> 0,
                     type |-> "Limit", status |-> "New",
                     timestamp |-> seqNum, expiryTime |-> 0]
       IN /\ orders' = orders @@ (oid :> order)
          /\ IF side = "Buy"
             THEN bids' = [bids EXCEPT ![price] = Append(@, oid)]
                  /\ asks' = asks
             ELSE asks' = [asks EXCEPT ![price] = Append(@, oid)]
                  /\ bids' = bids
          /\ nextOrderId' = nextOrderId + 1
          /\ seqNum' = seqNum + 1
          /\ UNCHANGED <<trades, nextTradeId, currentTime>>

(* ─── Match ──────────────────────────────────────────────────────── *)
\* Cross / matching action (P1-6).
\*
\* Fires whenever the book is crossed: the best bid price is >= the best
\* ask price (equivalently, the aggressive buy at best bid crosses the
\* best ask, or the aggressive sell at best ask crosses the best bid).
\*
\* FIFO selection: the resting order chosen at each side is the
\* earliest-timestamped order at the best price level. Because each queue
\* is maintained in FIFO order (front = oldest, see FIFO_Preservation),
\* the earliest order is at index 1. The BROKEN_NO_FIFO variant instead
\* picks the LAST (youngest) order at the best price when more than one
\* rests there — a genuine time-priority violation.
\*
\* Fill quantity is min(incoming.remainingQty, resting.remainingQty), so
\* at least one of the two orders is fully filled and leaves the book;
\* Match therefore fires only finitely often (state space stays bounded).
\* Quantity is conserved: both sides' remainingQty drop by fillQty while
\* their filledQty rise by fillQty, and one trade of size fillQty is
\* recorded (counted once per side => filledQty total = 2 * traded).
Match ==
    /\ \E bp \in DOMAIN bids : bids[bp] /= <<>>
    /\ \E ap \in DOMAIN asks : asks[ap] /= <<>>
    /\ BestBid >= BestAsk                        \* book is crossed
    /\ LET bp   == BestBid
           ap   == BestAsk
           \* FIFO picks the front (oldest); the broken variant picks the
           \* youngest resting order at the best price when one exists.
           buyIdx  == IF BROKEN_NO_FIFO /\ Len(bids[bp]) > 1
                      THEN Len(bids[bp]) ELSE 1
           sellIdx == IF BROKEN_NO_FIFO /\ Len(asks[ap]) > 1
                      THEN Len(asks[ap]) ELSE 1
           buyId   == bids[bp][buyIdx]
           sellId  == asks[ap][sellIdx]
           buyOrd  == orders[buyId]
           sellOrd == orders[sellId]
           fillQty == IF buyOrd.remainingQty <= sellOrd.remainingQty
                      THEN buyOrd.remainingQty ELSE sellOrd.remainingQty
           \* Execution at the passive (earlier-resting) order's price.
           execPrice == IF buyOrd.timestamp <= sellOrd.timestamp
                        THEN buyOrd.price ELSE sellOrd.price
           buyRem  == buyOrd.remainingQty - fillQty
           sellRem == sellOrd.remainingQty - fillQty
           trade   == [id |-> nextTradeId, buyId |-> buyId, sellId |-> sellId,
                       price |-> execPrice, quantity |-> fillQty,
                       buyTs |-> buyOrd.timestamp, sellTs |-> sellOrd.timestamp,
                       timestamp |-> seqNum]
       IN /\ fillQty > 0
          /\ orders' = [orders EXCEPT
                 ![buyId].remainingQty = buyRem,
                 ![buyId].filledQty    = buyOrd.filledQty + fillQty,
                 ![buyId].status       = IF buyRem = 0 THEN "Filled"
                                         ELSE "PartialFill",
                 ![sellId].remainingQty = sellRem,
                 ![sellId].filledQty    = sellOrd.filledQty + fillQty,
                 ![sellId].status       = IF sellRem = 0 THEN "Filled"
                                          ELSE "PartialFill"]
          \* Remove fully-filled orders from their queues (by id, so the
          \* broken variant can drop a non-front order without disturbing
          \* the relative order of the survivors).
          /\ bids' = [bids EXCEPT ![bp] = IF buyRem = 0
                          THEN SelectSeq(@, LAMBDA x : x /= buyId) ELSE @]
          /\ asks' = [asks EXCEPT ![ap] = IF sellRem = 0
                          THEN SelectSeq(@, LAMBDA x : x /= sellId) ELSE @]
          /\ trades' = Append(trades, trade)
          /\ nextTradeId' = nextTradeId + 1
          /\ seqNum' = seqNum + 1
          /\ UNCHANGED <<nextOrderId, currentTime>>

(* ─── CancelOrder ────────────────────────────────────────────────── *)

CancelOrder(oid) ==
    /\ oid \in DOMAIN orders
    /\ orders[oid].status \in {"New", "PartialFill"}
    /\ LET order == orders[oid]
           side == order.side
           price == order.price
       IN /\ orders' = [orders EXCEPT ![oid].status = "Cancelled",
                                       ![oid].cancelledQty = order.remainingQty,
                                       ![oid].remainingQty = 0]
          /\ IF side = "Buy"
             THEN bids' = [bids EXCEPT ![price] =
                      SelectSeq(@, LAMBDA x : x /= oid)]
                  /\ asks' = asks
             ELSE asks' = [asks EXCEPT ![price] =
                      SelectSeq(@, LAMBDA x : x /= oid)]
                  /\ bids' = bids
          /\ seqNum' = seqNum + 1
          /\ UNCHANGED <<nextOrderId, nextTradeId, trades, currentTime>>

(* ─── ExpireGTD ──────────────────────────────────────────────────── *)

ExpireGTD ==
    /\ \E oid \in DOMAIN orders :
        /\ orders[oid].type = "GTD"
        /\ orders[oid].status \in {"New", "PartialFill"}
        /\ orders[oid].expiryTime <= currentTime
        /\ CancelOrder(oid)

(* ─── AdvanceTime ────────────────────────────────────────────────── *)

AdvanceTime ==
    /\ currentTime < MaxTime
    /\ currentTime' = currentTime + 1
    /\ UNCHANGED <<bids, asks, orders, trades, nextOrderId, nextTradeId, seqNum>>

(* ─── Next ───────────────────────────────────────────────────────── *)

Next ==
    \/ \E p \in Participants, s \in Sides, pr \in Prices, q \in Qtys :
        PlaceLimit(p, s, pr, q)
    \/ Match
    \/ \E oid \in DOMAIN orders : CancelOrder(oid)
    \/ ExpireGTD
    \/ AdvanceTime

(* ─── Spec ───────────────────────────────────────────────────────── *)

Spec == Init /\ [][Next]_vars

(* ─── Invariants ─────────────────────────────────────────────────── *)

\* INV1: No order or trade has negative quantity
NoNegativeQuantity ==
    /\ \A oid \in DOMAIN orders : orders[oid].remainingQty >= 0
    /\ \A oid \in DOMAIN orders : orders[oid].qty >= 0

\* INV2: FIFO preservation — for two resting orders at the same price,
\*       if A was placed before B, A appears before B in the queue
FIFO_Preservation ==
    \A p \in Prices :
        /\ \A i, j \in 1..Len(bids[p]) :
            i < j => orders[bids[p][i]].timestamp < orders[bids[p][j]].timestamp
        /\ \A i, j \in 1..Len(asks[p]) :
            i < j => orders[asks[p][i]].timestamp < orders[asks[p][j]].timestamp

\* INV3: Total placed quantity = total resting + total traded + total cancelled
Quantity_Conservation ==
    LET totalPlaced == IF DOMAIN orders = {} THEN 0
                       ELSE LET ids == DOMAIN orders
                                SumPlaced[S \in SUBSET ids] ==
                                    IF S = {} THEN 0
                                    ELSE LET x == CHOOSE x \in S : TRUE
                                         IN orders[x].qty + SumPlaced[S \ {x}]
                            IN SumPlaced[ids]
        totalRemaining == IF DOMAIN orders = {} THEN 0
                          ELSE LET ids == DOMAIN orders
                                   SumRem[S \in SUBSET ids] ==
                                       IF S = {} THEN 0
                                       ELSE LET x == CHOOSE x \in S : TRUE
                                            IN orders[x].remainingQty + SumRem[S \ {x}]
                               IN SumRem[ids]
        totalTraded == IF trades = <<>> THEN 0
                       ELSE LET SumTrd[i \in 0..Len(trades)] ==
                                IF i = 0 THEN 0
                                ELSE SumTrd[i-1] + trades[i].quantity
                            IN SumTrd[Len(trades)]
    IN totalPlaced = totalRemaining + (totalTraded * 2)  \* Each trade counted for both sides

\* INV4: Matching conservation (P1-6). Across the whole lifecycle every
\* unit of placed quantity is accounted for as exactly one of: still
\* resting (remainingQty), filled by a cross (filledQty), or cancelled
\* (cancelledQty). This holds because every action preserves the per-order
\* identity qty = remainingQty + filledQty + cancelledQty, and summing over
\* all orders gives:
\*     totalPlacedQty = totalRestingQty + totalFilledQty + totalCancelledQty
MatchingConservation ==
    LET ids == DOMAIN orders
        SumF(f(_)) == LET S[T \in SUBSET ids] ==
                          IF T = {} THEN 0
                          ELSE LET x == CHOOSE x \in T : TRUE
                               IN f(x) + S[T \ {x}]
                      IN S[ids]
        totalPlacedQty    == SumF(LAMBDA o : orders[o].qty)
        totalRestingQty   == SumF(LAMBDA o : orders[o].remainingQty)
        totalFilledQty    == SumF(LAMBDA o : orders[o].filledQty)
        totalCancelledQty == SumF(LAMBDA o : orders[o].cancelledQty)
    IN totalPlacedQty = totalRestingQty + totalFilledQty + totalCancelledQty

\* INV5: FIFO execution / time priority (P1-6, non-vacuity P1-7).
\* Fills always consume the earliest-timestamped resting order at a price
\* level, never an arbitrary one. Stated as a reachable-state predicate:
\* at the same side and price, an order that has received a fill
\* (filledQty > 0) can never be YOUNGER than an order that is still resting
\* completely untouched (status "New", filledQty = 0). If it were, a later
\* order would have executed ahead of an earlier resting one — a time-
\* priority violation. The correct Match (front-of-queue) preserves this;
\* the BROKEN_NO_FIFO variant breaks it and TLC reports a counterexample.
FIFOExecution ==
    \A a \in DOMAIN orders, b \in DOMAIN orders :
        ( /\ orders[a].side  = orders[b].side
          /\ orders[a].price = orders[b].price
          /\ orders[b].filledQty > 0
          /\ orders[a].filledQty = 0
          /\ orders[a].status = "New"
        ) => orders[a].timestamp > orders[b].timestamp

\* INV6: No GTD order exists past its expiry time
GTD_Expiry_Correctness ==
    \A oid \in DOMAIN orders :
        orders[oid].type = "GTD" /\
        orders[oid].expiryTime < currentTime
        => orders[oid].status = "Cancelled"

====
