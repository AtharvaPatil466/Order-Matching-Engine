---- MODULE Auction ----
(***************************************************************************)
(* The session state machine (TradingState, include/Types.h), the per-state *)
(* order-admission rules (OrderBook::addOrder) and the uncross that is the  *)
(* only way an auction state turns into trades.                            *)
(*                                                                         *)
(* LegalEdges is the table OrderBook::transitionAllowed enforces           *)
(* (docs/MatchingRules.md Rule 7.7), edge for edge, with its three side    *)
(* conditions:                                                             *)
(*   Continuous -> PreOpen  only before the book's first session           *)
(*   PostClose  -> PreOpen  only with a trading date later than the date   *)
(*                          of the session it closed (beginSession)        *)
(*   Halted     -> s        only to the state it was halted from, or to    *)
(*                          PostClose: a halt is a pause, not a route      *)
(* Every state change in this spec is a Transition through that table, as  *)
(* setTradingStateLocked applies it: leaving an auction state for          *)
(* Continuous or PostClose is the uncross; any other change prints nothing. *)
(* Books boot in Continuous, so Init does too.                             *)
(*                                                                         *)
(* The book is abstract: a sequence of resting quantities. In Continuous   *)
(* an incoming order may trade at once; in an auction state it only        *)
(* accumulates; in Halted and PostClose it is rejected. Cancels are        *)
(* allowed in every state.                                                 *)
(*                                                                         *)
(* Non-vacuity: CONSTANT BUG                                               *)
(*   "no_reopen"            the volatility reopening never fires           *)
(*                          -> VolatilityEventuallyReopens (AuctionBroken)  *)
(*   "postclose_continuous" PostClose -> Continuous is allowed             *)
(*                          -> ValidTransitions (AuctionBrokenEdge)         *)
(*   "same_day_reopen"      PostClose -> PreOpen on the closed session's   *)
(*                          own date is allowed                            *)
(*                          -> ValidTransitions (AuctionBrokenDate)         *)
(*   "halt_detour"          Halted -> Continuous whatever it was halted    *)
(*                          from                                           *)
(*                          -> NoReopenAfterClose (AuctionBrokenHalt)       *)
(***************************************************************************)

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    MaxOrders,      \* orders submitted over a whole behaviour
    MaxEvents,      \* state changes over a whole behaviour (reopenings excluded)
    MaxDate,        \* trading dates are 1..MaxDate; 0 = undated
    Prices,         \* clearing / trade prices, e.g. {100, 200}
    BUG             \* "none" in Auction.cfg; see the header

ASSUME BUG \in {"none", "no_reopen", "postclose_continuous", "same_day_reopen", "halt_detour"}

States == {"PreOpen", "AuctionOpen", "Continuous", "AuctionClose",
           "PostClose", "Halted", "VolatilityAuction"}

\* OrderBook::inAuctionState: orders accumulate, nothing matches on arrival.
AuctionStates == {"PreOpen", "AuctionOpen", "AuctionClose", "VolatilityAuction"}

\* New orders rejected (MarketHalted / MarketClosed); cancels still allowed.
ClosedStates == {"Halted", "PostClose"}

\* OrderBook::transitionAllowed, without its side conditions (Allowed).
LegalEdges ==
    { <<s, "Halted">> : s \in States \ {"Halted"} }          \* any state may halt
    \cup { <<"Halted", s>> : s \in States \ {"Halted"} }     \* resume (pre-halt state) or close
    \cup { <<"PreOpen", "Continuous">>, <<"PreOpen", "AuctionOpen">>,
           <<"AuctionOpen", "Continuous">>, <<"AuctionOpen", "AuctionClose">>,
           <<"Continuous", "AuctionOpen">>, <<"Continuous", "AuctionClose">>,
           <<"Continuous", "VolatilityAuction">>, <<"Continuous", "PostClose">>,
           <<"Continuous", "PreOpen">>,
           <<"AuctionClose", "PostClose">>,
           <<"VolatilityAuction", "Continuous">>,
           <<"VolatilityAuction", "AuctionClose">>,
           <<"VolatilityAuction", "PostClose">>,
           <<"PostClose", "PreOpen">> }

VARIABLES
    state,          \* current TradingState
    book,           \* Seq of resting order quantities
    trades,         \* Seq of [price, qty, kind, inState]
    nextOrderId,    \* orders submitted so far
    eventCount,     \* state changes taken so far (reopenings excluded)
    sessionBegun,   \* OrderBook::sessionBegun_: a PreOpen has happened
    date,           \* OrderBook::sessionDate_: date of the current session, 0 = undated
    closing,        \* history: AuctionClose or PostClose entered since the last PreOpen
    preHalt         \* OrderBook::preHaltState_: the state Halted resumes to

vars == <<state, book, trades, nextOrderId, eventCount, sessionBegun, date, closing, preHalt>>

Init ==
    /\ state = "Continuous"
    /\ book = <<>>
    /\ trades = <<>>
    /\ nextOrderId = 0
    /\ eventCount = 0
    /\ sessionBegun = FALSE
    /\ date = 0
    /\ closing = FALSE
    /\ preHalt = "Continuous"

(* ─── Order admission ────────────────────────────────────────────────── *)

\* SubmitAccumulate: in an auction state, an incoming order is appended to
\* the auction book and produces NO trade. (OrderBook.cpp: PreOpen /
\* AuctionOpen / AuctionClose accumulate; VolatilityAuction behaves the same.)
SubmitAccumulate ==
    /\ state \in AuctionStates
    /\ nextOrderId < MaxOrders
    /\ \E q \in 1..2 :
        book' = Append(book, q)
    /\ nextOrderId' = nextOrderId + 1
    /\ UNCHANGED <<state, trades, eventCount, sessionBegun, date, closing, preHalt>>

\* SubmitContinuousRest: in Continuous, an incoming order may rest without
\* crossing (no contra interest). It joins the book, no trade prints.
SubmitContinuousRest ==
    /\ state = "Continuous"
    /\ nextOrderId < MaxOrders
    /\ \E q \in 1..2 :
        book' = Append(book, q)
    /\ nextOrderId' = nextOrderId + 1
    /\ UNCHANGED <<state, trades, eventCount, sessionBegun, date, closing, preHalt>>

\* SubmitContinuousMatch: in Continuous, an incoming order crosses an existing
\* resting order and prints a trade IMMEDIATELY at one of the valid prices.
\* This is the only NON-uncross way a trade is created, and it is gated on
\* state = "Continuous" — which is exactly what
\* NoMatchOutsideContinuousExceptUncross checks.
SubmitContinuousMatch ==
    /\ state = "Continuous"
    /\ nextOrderId < MaxOrders
    /\ Len(book) > 0
    /\ \E p \in Prices :
        trades' = Append(trades,
                         [price |-> p, qty |-> book[1],
                          kind |-> "continuous", inState |-> "Continuous"])
    /\ book' = Tail(book)               \* the resting order is consumed
    /\ nextOrderId' = nextOrderId + 1
    /\ UNCHANGED <<state, eventCount, sessionBegun, date, closing, preHalt>>

\* CancelOrder: allowed in EVERY state (including Halted / PostClose), and
\* never prints a trade. Models the un-gated cancel path: cancelOrder() has
\* no state gate (OrderBook.cpp comment, line ~199).
CancelOrder ==
    /\ Len(book) > 0
    /\ book' = Tail(book)
    /\ UNCHANGED <<state, trades, nextOrderId, eventCount, sessionBegun, date, closing, preHalt>>

(* ─── State changes ──────────────────────────────────────────────────── *)

\* The table plus its side conditions, for a move from `state` to `to`
\* carrying trading date d (only a PreOpen carries one). BUG widens it.
Allowed(to, d) ==
    \/ /\ <<state, to>> \in LegalEdges
       /\ (state = "Continuous" /\ to = "PreOpen") => ~sessionBegun
       /\ (state = "PostClose" /\ to = "PreOpen") =>
              /\ d /= 0
              /\ \/ date = 0
                 \/ d > date
                 \/ BUG = "same_day_reopen" /\ d = date
       /\ state = "Halted" => to \in {preHalt, "PostClose"}
    \/ BUG = "postclose_continuous" /\ state = "PostClose" /\ to = "Continuous"
    \/ BUG = "halt_detour" /\ state = "Halted" /\ to = "Continuous"

\* One applied change, as setTradingStateLocked does it.
Step(to, d) ==
    /\ to /= state
    /\ Allowed(to, d)
    /\ IF state \in AuctionStates /\ to \in {"Continuous", "PostClose"}
       THEN \* the uncross: the whole book clears at one price
            /\ \E p \in Prices :
                   trades' = IF Len(book) > 0
                             THEN Append(trades, [price |-> p, qty |-> book[1],
                                                  kind |-> "uncross",
                                                  inState |-> state])
                             ELSE trades
            /\ book' = <<>>
       ELSE UNCHANGED <<book, trades>>
    /\ state' = to
    /\ sessionBegun' = (sessionBegun \/ to = "PreOpen")
    /\ date' = IF to = "PreOpen" /\ d /= 0 THEN d ELSE date
    /\ closing' = IF to = "PreOpen" THEN FALSE
                  ELSE closing \/ to \in {"AuctionClose", "PostClose"}
    /\ preHalt' = IF to = "Halted" THEN state ELSE preHalt
    /\ UNCHANGED nextOrderId

\* Every change but the volatility reopening spends the event budget, which
\* bounds every cycle. The reopening stays unbudgeted so weak fairness can
\* force it (VolatilityEventuallyReopens); it can fire at most once per
\* budgeted entry into VolatilityAuction.
Transition(to, d) ==
    /\ eventCount < MaxEvents
    /\ ~(state = "VolatilityAuction" /\ to = "Continuous")
    /\ Step(to, d)
    /\ eventCount' = eventCount + 1

ReopeningUncross ==
    /\ BUG /= "no_reopen"
    /\ state = "VolatilityAuction"
    /\ Step("Continuous", 0)
    /\ UNCHANGED eventCount

Next ==
    \/ SubmitAccumulate
    \/ SubmitContinuousRest
    \/ SubmitContinuousMatch
    \/ CancelOrder
    \/ \E to \in States :
           \E d \in (IF to = "PreOpen" THEN 0..MaxDate ELSE {0}) : Transition(to, d)
    \/ ReopeningUncross

\* No fairness on anything else: a halt, or a closed book, may persist.
Spec == Init /\ [][Next]_vars /\ WF_vars(ReopeningUncross)

(* ─── Invariants ─────────────────────────────────────────────────────── *)

TypeOK ==
    /\ state \in States
    /\ nextOrderId \in 0..MaxOrders
    /\ eventCount \in 0..MaxEvents
    /\ sessionBegun \in BOOLEAN
    /\ date \in 0..MaxDate
    /\ closing \in BOOLEAN
    /\ preHalt \in States \ {"Halted"}
    /\ \A i \in 1..Len(book) : book[i] \in 1..2
    /\ \A i \in 1..Len(trades) :
        /\ trades[i].price \in Prices
        /\ trades[i].qty \in 1..2
        /\ trades[i].kind \in {"continuous", "uncross"}
        /\ trades[i].inState \in States

\* Every step either leaves the state alone or follows the enforced table,
\* side conditions included. Written against LegalEdges directly, not
\* through Allowed, so a BUG that widens Allowed is caught here.
ValidTransitions ==
    [][ \/ state' = state
        \/ /\ <<state, state'>> \in LegalEdges
           /\ (state = "Continuous" /\ state' = "PreOpen") => ~sessionBegun
           /\ (state = "PostClose" /\ state' = "PreOpen") => date' > date
           /\ state = "Halted" => state' \in {preHalt, "PostClose"}
      ]_<<state, date, sessionBegun, preHalt>>

\* A trade prints in Continuous, or is the uncross of an auction state.
NoMatchOutsideContinuousExceptUncross ==
    \A i \in 1..Len(trades) :
        \/ trades[i].kind = "continuous" /\ trades[i].inState = "Continuous"
        \/ trades[i].kind = "uncross" /\ trades[i].inState \in AuctionStates

\* Each uncross prints at most one record, at one price from Prices.
SingleClearingPrice ==
    \A i \in 1..Len(trades) :
        trades[i].kind = "uncross" => trades[i].price \in Prices

NoTradeWhileHalted ==
    \A i \in 1..Len(trades) : trades[i].inState \notin ClosedStates

\* Once a session reaches its closing auction or its close, nothing trades
\* continuously, or auctions, until the next PreOpen. A halt resumes only to
\* the state it paused, so it is no detour around the table; under
\* BUG = "halt_detour" Continuous -> AuctionClose -> Halted -> Continuous
\* abandons the closing auction (AuctionBrokenHalt.cfg).
NoReopenAfterClose ==
    closing => state \in {"AuctionClose", "PostClose", "Halted"}

(* ─── Liveness ───────────────────────────────────────────────────────── *)

\* A volatility auction always ends. Under BUG = "no_reopen" it can last
\* forever, and AuctionBroken.cfg must produce that counterexample.
VolatilityEventuallyReopens ==
    [](state = "VolatilityAuction" => <>(state /= "VolatilityAuction"))

====
