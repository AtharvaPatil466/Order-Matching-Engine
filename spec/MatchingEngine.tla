---- MODULE MatchingEngine ----
(***************************************************************************)
(* Continuous price-time matching for one symbol, as OrderBook::match,     *)
(* modifyOrder and cancelReplace do it (src/OrderBook.cpp).                *)
(*                                                                         *)
(* An order is matched when it arrives, before it can rest: Submit makes   *)
(* it the in-flight `aggressor`, and while one is in flight only Match,    *)
(* StpCancelIncoming and Rest can fire. That models the engine taking the  *)
(* book lock for the whole of addOrder. The aggressor is not on the book   *)
(* while it matches, so the resting book is never crossed in ANY state.    *)
(*                                                                         *)
(* Actions:                                                                *)
(*   Submit            a Limit order arrives and becomes the aggressor     *)
(*   Match             the aggressor fills against the front order of the  *)
(*                     best opposite level it crosses, at that order's     *)
(*                     price                                               *)
(*   StpCancelIncoming the front order belongs to the aggressor's own      *)
(*                     participant: the aggressor's remainder is cancelled *)
(*                     (STPMode::DefaultCancelIncoming, the only mode      *)
(*                     either binary runs; no binary calls setSTPMode)     *)
(*   Rest              the aggressor crosses nothing: its remainder joins  *)
(*                     the back of its level                               *)
(*   Cancel            a resting order leaves the book                     *)
(*   Modify            modifyOrder: reduce a resting order's quantity in   *)
(*                     place, keeping its queue position (0 < new < old)   *)
(*   Replace           cancelReplace to a new price, quantity unchanged:   *)
(*                     the order leaves the book, takes a new timestamp    *)
(*                     and arrives again as an aggressor. To the same      *)
(*                     price it rests at the back of its level.            *)
(*                                                                         *)
(* Not modelled: Market/IOC/FOK/iceberg/hidden orders, the other three STP *)
(* modes, a replace that changes price and quantity together, auctions,    *)
(* pro-rata, risk and admission checks, the journal and crash recovery,    *)
(* and concurrency between symbols or threads.                             *)
(*                                                                         *)
(* Non-vacuity: CONSTANT BUG injects one defect at a time. Each            *)
(* MatchingEngineBroken*.cfg names one and checks only the property that   *)
(* defect must break; .github/workflows/tla.yml requires each to fail with *)
(* that property's counterexample.                                         *)
(*   "no_fifo"                 Match takes the youngest order at the level *)
(*                             -> FIFOExecution                            *)
(*   "worst_price_first"       Match takes the worst level it crosses      *)
(*                             -> PricePriority                            *)
(*   "aggressor_price"         a fill prints at the aggressor's limit      *)
(*                             -> ExecPriceIsPassive                       *)
(*   "rest_crossed"            Rest may fire while the aggressor crosses   *)
(*                             -> NoCrossedBookAfterStep                   *)
(*   "no_stp"                  same-participant orders trade               *)
(*                             -> NoSelfTrade                              *)
(*   "modify_to_zero"          Modify accepts 0 and leaves the order       *)
(*                             resting (MATCH-2) -> NoEmptyRestingOrder    *)
(*   "replace_keeps_priority"  Replace keeps the old timestamp             *)
(*                             -> FIFO_Preservation                        *)
(***************************************************************************)

EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    MaxOrders,      \* orders submitted over a whole behaviour
    MaxReplaces,    \* Replace steps over a whole behaviour
    Participants,
    Prices,
    MaxQty,
    BUG             \* "none" in MatchingEngine.cfg; see the header

ASSUME BUG \in {"none", "no_fifo", "worst_price_first", "aggressor_price",
                "rest_crossed", "no_stp", "modify_to_zero",
                "replace_keeps_priority"}

VARIABLES
    bids,       \* Prices -> Seq(order id), front = oldest
    asks,
    orders,     \* Seq of order records; an order's id is its index
    aggressor,  \* id of the order being matched, 0 when none
    replaces,   \* Replace steps taken so far
    lastTrade   \* the fill this step made, or NoTrade

vars == <<bids, asks, orders, aggressor, replaces, lastTrade>>

Sides == {"Buy", "Sell"}
NoTrade == [aggressorId |-> 0, restingId |-> 0, price |-> 0, qty |-> 0]

\* Priority time. Strictly increases at every Submit and Replace, the two
\* actions that stamp an order.
Now == Len(orders) + replaces

Book(side) == IF side = "Buy" THEN bids ELSE asks
Opposite(side) == IF side = "Buy" THEN "Sell" ELSE "Buy"

Min(S) == CHOOSE x \in S : \A y \in S : x <= y
Max(S) == CHOOSE x \in S : \A y \in S : x >= y

BidLevels == {p \in Prices : bids[p] /= <<>>}
AskLevels == {p \in Prices : asks[p] /= <<>>}

Resting(o) == \E i \in 1..Len(Book(orders[o].side)[orders[o].price]) :
                  Book(orders[o].side)[orders[o].price][i] = o

\* Opposite levels order o's limit reaches.
CrossLevels(o) ==
    IF orders[o].side = "Buy"
    THEN {p \in AskLevels : orders[o].price >= p}
    ELSE {p \in BidLevels : orders[o].price <= p}

\* The level Match takes: the best one (lowest ask, highest bid).
MatchLevel(o) ==
    IF (orders[o].side = "Buy") = (BUG /= "worst_price_first")
    THEN Min(CrossLevels(o)) ELSE Max(CrossLevels(o))

\* The resting order the aggressor meets next: the front of MatchLevel.
Contra(o) ==
    LET q == Book(Opposite(orders[o].side))[MatchLevel(o)]
    IN  IF BUG = "no_fifo" /\ Len(q) > 1 THEN q[Len(q)] ELSE q[1]

Drop(q, o) == SelectSeq(q, LAMBDA x : x /= o)

\* Book after removing order o from its level.
WithoutBids(o) == IF orders[o].side = "Buy"
                  THEN [bids EXCEPT ![orders[o].price] = Drop(@, o)] ELSE bids
WithoutAsks(o) == IF orders[o].side = "Sell"
                  THEN [asks EXCEPT ![orders[o].price] = Drop(@, o)] ELSE asks

Init ==
    /\ bids = [p \in Prices |-> <<>>]
    /\ asks = [p \in Prices |-> <<>>]
    /\ orders = <<>>
    /\ aggressor = 0
    /\ replaces = 0
    /\ lastTrade = NoTrade

Submit(pt, side, price, qty) ==
    /\ aggressor = 0
    /\ Len(orders) < MaxOrders
    /\ orders' = Append(orders,
           [participant |-> pt, side |-> side, price |-> price, qty |-> qty,
            remainingQty |-> qty, filledQty |-> 0, cancelledQty |-> 0,
            timestamp |-> Now])
    /\ aggressor' = Len(orders) + 1
    /\ lastTrade' = NoTrade
    /\ UNCHANGED <<bids, asks, replaces>>

Match ==
    LET a == aggressor IN
    /\ a /= 0
    /\ CrossLevels(a) /= {}
    /\ LET r     == Contra(a)
           fill  == Min({orders[a].remainingQty, orders[r].remainingQty})
           price == IF BUG = "aggressor_price" THEN orders[a].price
                    ELSE orders[r].price
           aRem  == orders[a].remainingQty - fill
           rRem  == orders[r].remainingQty - fill
       IN /\ orders[r].participant /= orders[a].participant \/ BUG = "no_stp"
          /\ orders' = [orders EXCEPT
                 ![a].remainingQty = aRem,
                 ![a].filledQty    = @ + fill,
                 ![r].remainingQty = rRem,
                 ![r].filledQty    = @ + fill]
          /\ bids' = IF rRem = 0 THEN WithoutBids(r) ELSE bids
          /\ asks' = IF rRem = 0 THEN WithoutAsks(r) ELSE asks
          /\ aggressor' = IF aRem = 0 THEN 0 ELSE a
          /\ lastTrade' = [aggressorId |-> a, restingId |-> r,
                           price |-> price, qty |-> fill]
          /\ UNCHANGED replaces

StpCancelIncoming ==
    LET a == aggressor IN
    /\ a /= 0
    /\ BUG /= "no_stp"
    /\ CrossLevels(a) /= {}
    /\ orders[Contra(a)].participant = orders[a].participant
    /\ orders' = [orders EXCEPT ![a].cancelledQty = @ + orders[a].remainingQty,
                                ![a].remainingQty = 0]
    /\ aggressor' = 0
    /\ lastTrade' = NoTrade
    /\ UNCHANGED <<bids, asks, replaces>>

Rest ==
    LET a == aggressor IN
    /\ a /= 0
    /\ CrossLevels(a) = {} \/ BUG = "rest_crossed"
    /\ IF orders[a].side = "Buy"
       THEN /\ bids' = [bids EXCEPT ![orders[a].price] = Append(@, a)]
            /\ UNCHANGED asks
       ELSE /\ asks' = [asks EXCEPT ![orders[a].price] = Append(@, a)]
            /\ UNCHANGED bids
    /\ aggressor' = 0
    /\ lastTrade' = NoTrade
    /\ UNCHANGED <<orders, replaces>>

Cancel(o) ==
    /\ aggressor = 0
    /\ Resting(o)
    /\ orders' = [orders EXCEPT ![o].cancelledQty = @ + orders[o].remainingQty,
                                ![o].remainingQty = 0]
    /\ bids' = WithoutBids(o)
    /\ asks' = WithoutAsks(o)
    /\ lastTrade' = NoTrade
    /\ UNCHANGED <<aggressor, replaces>>

Modify(o, newQty) ==
    /\ aggressor = 0
    /\ Resting(o)
    /\ newQty < orders[o].remainingQty
    /\ newQty > 0 \/ BUG = "modify_to_zero"
    /\ orders' = [orders EXCEPT
           ![o].cancelledQty = @ + (orders[o].remainingQty - newQty),
           ![o].remainingQty = newQty]
    /\ lastTrade' = NoTrade
    /\ UNCHANGED <<bids, asks, aggressor, replaces>>

Replace(o, newPrice) ==
    /\ aggressor = 0
    /\ replaces < MaxReplaces
    /\ Resting(o)
    /\ orders' = [orders EXCEPT
           ![o].price     = newPrice,
           ![o].timestamp = IF BUG = "replace_keeps_priority" THEN @ ELSE Now]
    /\ bids' = WithoutBids(o)
    /\ asks' = WithoutAsks(o)
    /\ aggressor' = o
    /\ replaces' = replaces + 1
    /\ lastTrade' = NoTrade

Next ==
    \/ \E pt \in Participants, s \in Sides, p \in Prices, q \in 1..MaxQty :
           Submit(pt, s, p, q)
    \/ Match
    \/ StpCancelIncoming
    \/ Rest
    \/ \E o \in DOMAIN orders :
           \/ Cancel(o)
           \/ \E q \in 0..MaxQty : Modify(o, q)
           \/ \E p \in Prices : Replace(o, p)

Spec == Init /\ [][Next]_vars

(* ─── State invariants ───────────────────────────────────────────── *)

NoNegativeQuantity ==
    \A o \in DOMAIN orders :
        /\ orders[o].remainingQty >= 0
        /\ orders[o].filledQty >= 0
        /\ orders[o].cancelledQty >= 0

\* A resting order always has quantity left (MATCH-2 was a resting order
\* modified to zero).
NoEmptyRestingOrder ==
    \A o \in DOMAIN orders : Resting(o) => orders[o].remainingQty > 0

\* Queue order is priority-time order at every level.
FIFO_Preservation ==
    \A p \in Prices, side \in Sides :
        LET q == Book(side)[p]
        IN  \A i \in 1..(Len(q) - 1) :
                orders[q[i]].timestamp < orders[q[i + 1]].timestamp

\* Every unit an order was given is resting, filled or cancelled, and every
\* fill moved the same quantity on both sides.
MatchingConservation ==
    LET SumFilled(side) ==
            LET S[i \in 0..Len(orders)] ==
                    IF i = 0 THEN 0
                    ELSE S[i - 1] + (IF orders[i].side = side
                                     THEN orders[i].filledQty ELSE 0)
            IN  S[Len(orders)]
    IN  /\ \A o \in DOMAIN orders :
               orders[o].qty = orders[o].remainingQty + orders[o].filledQty
                                 + orders[o].cancelledQty
        /\ SumFilled("Buy") = SumFilled("Sell")

\* The book is never crossed. Named for the roadmap item; it holds in every
\* state, not only between submissions, because the aggressor is off the
\* book while it matches.
NoCrossedBookAfterStep ==
    BidLevels /= {} /\ AskLevels /= {} => Max(BidLevels) < Min(AskLevels)

NoSelfTrade ==
    lastTrade /= NoTrade =>
        orders[lastTrade.aggressorId].participant
            /= orders[lastTrade.restingId].participant

(* ─── Properties of each fill (checked on every step) ────────────── *)

TradeStep == lastTrade' /= NoTrade
Filled    == lastTrade'.restingId

\* The resting order filled was on the best level of its side.
PricePriority ==
    [][TradeStep =>
        IF orders[Filled].side = "Sell"
        THEN orders[Filled].price = Min(AskLevels)
        ELSE orders[Filled].price = Max(BidLevels)]_vars

\* The resting order filled was the oldest at its level.
FIFOExecution ==
    [][TradeStep =>
        Head(Book(orders[Filled].side)[orders[Filled].price]) = Filled]_vars

\* A fill prints at the resting (passive) order's price, which is never
\* worse than the aggressor's limit.
ExecPriceIsPassive ==
    [][TradeStep =>
        LET a == lastTrade'.aggressorId
            px == lastTrade'.price
        IN  /\ px = orders[Filled].price
            /\ IF orders[a].side = "Buy" THEN px <= orders[a].price
                                         ELSE px >= orders[a].price]_vars

====
