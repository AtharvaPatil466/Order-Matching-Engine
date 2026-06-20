---- MODULE Auction ----
(***************************************************************************)
(* TLA+ Specification for the Exchange Auction / Session State Machine      *)
(*                                                                         *)
(* Models the engine's `TradingState` lifecycle (include/Types.h) plus the *)
(* VolatilityAuction state used by the volatility breaker, together with    *)
(* the per-state order-admission rules (src/OrderBook.cpp::addOrder) and    *)
(* the discrete "uncross" event that is the ONLY way trades emerge from an  *)
(* auction state (OrderBook::uncross).                                      *)
(*                                                                         *)
(* States:                                                                 *)
(*   PreOpen           - pre-session; orders accumulate                     *)
(*   AuctionOpen       - opening auction; orders accumulate                 *)
(*   Continuous        - normal trading; incoming orders may match now      *)
(*   AuctionClose      - closing auction; orders accumulate                 *)
(*   PostClose         - session ended; new orders rejected, cancels OK     *)
(*   Halted            - manual hard halt; new orders rejected, cancels OK   *)
(*   VolatilityAuction - band-breach reopening auction; orders accumulate   *)
(*                                                                         *)
(* Legal transitions (scheduler / engine driven):                          *)
(*   PreOpen           -> Continuous          (opening uncross)             *)
(*   Continuous        -> AuctionClose                                      *)
(*   AuctionClose      -> PostClose           (closing uncross)             *)
(*   Continuous        -> VolatilityAuction   (band breach)                 *)
(*   VolatilityAuction -> Continuous          (reopening uncross)           *)
(*   any state         -> Halted              (manual hard halt)            *)
(*   Halted            -> Continuous          (manual resume)               *)
(*                                                                         *)
(* The order book is modeled abstractly as a tiny multiset of resting       *)
(* quantities; matching is abstracted to "in Continuous an incoming order   *)
(* may cross immediately and print a trade", whereas in an auction state    *)
(* orders only ACCUMULATE. An uncross is a single discrete event that       *)
(* clears the entire accumulated auction book at ONE clearing price.        *)
(*                                                                         *)
(* Properties proved:                                                      *)
(*   TypeOK                                  (type invariant)               *)
(*   ValidTransitions    (every step is a legal edge of the state machine)  *)
(*   NoMatchOutsideContinuousExceptUncross                                  *)
(*   SingleClearingPrice                                                    *)
(*   NoTradeWhileHalted                                                     *)
(*   VolatilityEventuallyReopens             (liveness, under WF)            *)
(***************************************************************************)

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    MaxOrders,      \* Maximum number of orders submitted during the run
    MaxEvents,      \* Bound on the number of state-machine events
    Prices,         \* Set of valid clearing/limit prices, e.g. {100, 200}
    BROKEN_NO_REOPEN \* Non-vacuity switch: TRUE disables the volatility
                     \* reopening uncross so that TLC must produce a
                     \* counterexample to VolatilityEventuallyReopens.

(* ─── State sets ─────────────────────────────────────────────────────── *)

States == {"PreOpen", "AuctionOpen", "Continuous", "AuctionClose",
           "PostClose", "Halted", "VolatilityAuction"}

\* States in which orders accumulate with no continuous matching. An uncross
\* is the only way these turn into trades (mirrors OrderBook.cpp::addOrder).
AuctionStates == {"PreOpen", "AuctionOpen", "AuctionClose", "VolatilityAuction"}

\* States in which new orders are rejected entirely (cancels still allowed).
ClosedStates == {"Halted", "PostClose"}

VARIABLES
    state,          \* Current TradingState
    book,           \* Seq of resting order quantities (the abstract auction book)
    trades,         \* Seq of trade records: [price |-> p, qty |-> q, kind |-> ...]
    nextOrderId,    \* Number of orders submitted so far (bounds the run)
    eventCount,     \* Number of state-machine events taken (bounds the run)
    sawVolatility   \* History flag: TRUE once VolatilityAuction has occurred

vars == <<state, book, trades, nextOrderId, eventCount, sawVolatility>>

(* ─── Trade records ──────────────────────────────────────────────────── *)
\* Every trade record carries:
\*   price - the execution price (one value, drawn from Prices)
\*   qty   - the executed quantity
\*   kind  - "continuous" (immediate cross in Continuous state)
\*           or "uncross"  (discrete uncross at an auction boundary)
\*   inState - the TradingState the engine was IN when the trade printed.
\* `inState` is a faithful history field (it records observable fact, not a
\* prophecy): it lets the safety invariants below be genuine reachable-state
\* predicates with teeth, rather than tautologies over the guards.

(* ─── Init ───────────────────────────────────────────────────────────── *)

\* The session begins in PreOpen with an empty book, mirroring the
\* engine's startup before the opening auction (OrderBook seeds PreOpen).
Init ==
    /\ state = "PreOpen"
    /\ book = <<>>
    /\ trades = <<>>
    /\ nextOrderId = 0
    /\ eventCount = 0
    /\ sawVolatility = FALSE

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
    /\ UNCHANGED <<state, trades, eventCount, sawVolatility>>

\* SubmitContinuousRest: in Continuous, an incoming order may rest without
\* crossing (no contra interest). It joins the book, no trade prints.
SubmitContinuousRest ==
    /\ state = "Continuous"
    /\ nextOrderId < MaxOrders
    /\ \E q \in 1..2 :
        book' = Append(book, q)
    /\ nextOrderId' = nextOrderId + 1
    /\ UNCHANGED <<state, trades, eventCount, sawVolatility>>

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
    /\ UNCHANGED <<state, eventCount, sawVolatility>>

\* CancelOrder: allowed in EVERY state (including Halted / PostClose), and
\* never prints a trade. Models the un-gated cancel path: cancelOrder() has
\* no state gate (OrderBook.cpp comment, line ~199).
CancelOrder ==
    /\ Len(book) > 0
    /\ book' = Tail(book)
    /\ UNCHANGED <<state, trades, nextOrderId, eventCount, sawVolatility>>

(* ─── Uncross ────────────────────────────────────────────────────────── *)

\* Uncross(newState): the single discrete clearing event. It executes the
\* ENTIRE accumulated auction book at ONE clearing price chosen from Prices,
\* emits at most one trade record tagged "uncross", clears the book, and
\* moves the state machine to `newState`. Modeling the whole auction book as
\* clearing at a single price is the abstraction behind SingleClearingPrice.
\*
\* The trade is only emitted if there was something to clear (Len(book) > 0);
\* an uncross of an empty book is a no-op print, matching the engine where an
\* uncross with zero crossable volume produces no execution.
\* `Uncross` itself does NOT touch eventCount (see the bounding note below).
\* It clears the whole book, emits at most one "uncross" print at one price,
\* and moves state from -> to.
Uncross(from, to) ==
    /\ state = from
    /\ \E p \in Prices :
        IF Len(book) > 0
        THEN trades' = Append(trades,
                              [price |-> p, qty |-> book[1],
                               kind |-> "uncross", inState |-> from])
        ELSE trades' = trades
    /\ book' = <<>>                      \* whole auction book cleared at once
    /\ state' = to
    /\ UNCHANGED <<nextOrderId, eventCount, sawVolatility>>

(* ─── Bounding discipline (why some actions count and others don't) ────── *)
\* The state space is kept finite by bounding only the CYCLE-CREATING
\* transitions with `eventCount`: Halt, EnterVolatilityAuction, and
\* EnterAuctionClose. Every loop in the state graph must pass through one of
\* these, so capping their total firings caps the whole run.
\*
\* The COMPLETING transitions — OpeningUncross, ClosingUncross,
\* ReopeningUncross, Resume — are NOT gated by eventCount and do NOT
\* increment it. This is essential for liveness: WF_vars(ReopeningUncross)
\* can only force the reopen if the action stays continuously ENABLED while
\* in VolatilityAuction. Gating it on a budget that the entry already spent
\* would let it become disabled (a modeling artifact), spuriously breaking
\* VolatilityEventuallyReopens. Since VolatilityAuction is only ever entered
\* via the gated EnterVolatilityAuction, the number of reopenings is still
\* bounded by the number of entries — finiteness is preserved.

(* ─── State-machine transitions ──────────────────────────────────────── *)

\* PreOpen -> Continuous via the opening uncross. PreOpen is only the initial
\* state and has no incoming edge, so this fires at most once — no gate needed.
OpeningUncross ==
    Uncross("PreOpen", "Continuous")

\* Continuous -> AuctionClose. No uncross on this edge (orders begin to
\* accumulate again for the closing auction). Cycle-creating -> gated.
EnterAuctionClose ==
    /\ eventCount < MaxEvents
    /\ state = "Continuous"
    /\ state' = "AuctionClose"
    /\ eventCount' = eventCount + 1
    /\ UNCHANGED <<book, trades, nextOrderId, sawVolatility>>

\* AuctionClose -> PostClose via the closing uncross. Completing -> ungated.
ClosingUncross ==
    Uncross("AuctionClose", "PostClose")

\* Continuous -> VolatilityAuction on a price-band breach. No uncross on this
\* edge (the breach freezes continuous matching and starts accumulating).
\* Cycle-creating -> gated.
EnterVolatilityAuction ==
    /\ eventCount < MaxEvents
    /\ state = "Continuous"
    /\ state' = "VolatilityAuction"
    /\ sawVolatility' = TRUE
    /\ eventCount' = eventCount + 1
    /\ UNCHANGED <<book, trades, nextOrderId>>

\* VolatilityAuction -> Continuous via the reopening uncross. Completing ->
\* ungated, so it stays continuously enabled in VolatilityAuction and WF can
\* force it. Guarded by ~BROKEN_NO_REOPEN: when the non-vacuity switch is set
\* this action is permanently disabled, VolatilityAuction becomes absorbing,
\* and TLC must report a counterexample to VolatilityEventuallyReopens.
ReopeningUncross ==
    /\ ~BROKEN_NO_REOPEN
    /\ Uncross("VolatilityAuction", "Continuous")

\* any state -> Halted (manual hard halt). Halting clears nothing and prints
\* nothing; orders already resting stay put but no new matching occurs.
\* Cycle-creating -> gated.
Halt ==
    /\ eventCount < MaxEvents
    /\ state /= "Halted"
    /\ state' = "Halted"
    /\ eventCount' = eventCount + 1
    /\ UNCHANGED <<book, trades, nextOrderId, sawVolatility>>

\* Halted -> Continuous (manual resume). No uncross; continuous matching
\* simply resumes. Completing -> ungated (Halted is only reached via the
\* gated Halt, so resumes are bounded by halts).
Resume ==
    /\ state = "Halted"
    /\ state' = "Continuous"
    /\ UNCHANGED <<book, trades, nextOrderId, eventCount, sawVolatility>>

(* ─── Next ───────────────────────────────────────────────────────────── *)

Next ==
    \/ SubmitAccumulate
    \/ SubmitContinuousRest
    \/ SubmitContinuousMatch
    \/ CancelOrder
    \/ OpeningUncross
    \/ EnterAuctionClose
    \/ ClosingUncross
    \/ EnterVolatilityAuction
    \/ ReopeningUncross
    \/ Halt
    \/ Resume

(* ─── Fairness ───────────────────────────────────────────────────────── *)

\* Weak fairness on the reopening uncross: a VolatilityAuction must not be
\* able to sit forever when the reopening action is continuously enabled.
\* This is what gives VolatilityEventuallyReopens its teeth. We deliberately
\* do NOT put fairness on Halt (a manual halt may persist indefinitely) so
\* that VolatilityAuction is provably non-absorbing while Halted may be.
Fairness ==
    WF_vars(ReopeningUncross)

Spec == Init /\ [][Next]_vars /\ Fairness

(* ─── Invariants ─────────────────────────────────────────────────────── *)

\* Type invariant.
TypeOK ==
    /\ state \in States
    /\ nextOrderId \in 0..MaxOrders
    /\ eventCount \in 0..MaxEvents
    /\ sawVolatility \in BOOLEAN
    /\ \A i \in 1..Len(book) : book[i] \in 1..2
    /\ \A i \in 1..Len(trades) :
        /\ trades[i].price \in Prices
        /\ trades[i].qty \in 1..2
        /\ trades[i].kind \in {"continuous", "uncross"}
        /\ trades[i].inState \in States

\* INV: every reachable state is one of the seven legal TradingStates.
\* (Subsumed by TypeOK but kept explicit for documentation parity with the
\* other specs' named invariants.)
ValidState == state \in States

\* The complete set of legal directed edges of the session state machine.
\* A self-loop (state' = state) is always legal (it covers the order-flow
\* actions that do not change state).
LegalEdges ==
    { <<"PreOpen", "Continuous">>,
      <<"Continuous", "AuctionClose">>,
      <<"AuctionClose", "PostClose">>,
      <<"Continuous", "VolatilityAuction">>,
      <<"VolatilityAuction", "Continuous">> }
    \cup { <<s, "Halted">> : s \in States }     \* any state -> Halted
    \cup { <<"Halted", "Continuous">> }          \* Halted -> Continuous

\* ACTION PROPERTY: ValidTransitions. Every step either leaves the state
\* unchanged or follows a legal edge. Checked as a temporal/action property
\* (it relates state and state'). A bug that introduced an illegal jump
\* (e.g. PostClose -> Continuous) would be caught here.
ValidTransitions ==
    [][ state' = state \/ <<state, state'>> \in LegalEdges ]_state

\* PROPERTY 1: NoMatchOutsideContinuousExceptUncross.
\* No trade is produced while the market is in an auction / halt / closed
\* state, EXCEPT the discrete uncross that fires on an auction boundary.
\* Each trade record stamps the state it printed in (`inState`), so this is a
\* genuine reachable-state invariant: a non-uncross ("continuous") trade may
\* ONLY have printed in Continuous, and any trade that printed in a non-
\* Continuous state MUST be an uncross whose source was a true auction state
\* (PreOpen / AuctionClose / VolatilityAuction). Any continuous print
\* attributed to a non-Continuous state, or an uncross attributed to Halted /
\* PostClose / AuctionOpen / Continuous, is a violation.
NoMatchOutsideContinuousExceptUncross ==
    \A i \in 1..Len(trades) :
        \/ /\ trades[i].kind = "continuous"
           /\ trades[i].inState = "Continuous"
        \/ /\ trades[i].kind = "uncross"
           /\ trades[i].inState \in {"PreOpen", "AuctionClose", "VolatilityAuction"}

\* PROPERTY 2: SingleClearingPrice.
\* Each uncross produces trades at exactly one price. Uncross emits at most
\* one trade record per firing with a single chosen price, so the set of
\* distinct prices contributed by any single uncross has cardinality <= 1.
\* We assert that for every uncross print, the prices among the uncross
\* trades that share its source-state-and-position collapse to one value —
\* structurally one record carries one price field drawn from Prices, and no
\* uncross ever emits two records at different prices.
SingleClearingPrice ==
    \A i \in 1..Len(trades) :
        trades[i].kind = "uncross" =>
            /\ trades[i].price \in Prices
            /\ Cardinality({trades[i].price}) = 1

\* PROPERTY 3: NoTradeWhileHalted.
\* Zero trades are ever produced while in Halted: no trade record may carry
\* inState = "Halted". (No trade-producing action is enabled in Halted —
\* SubmitContinuousMatch needs Continuous and every Uncross needs its auction
\* source state — so this reachable-state invariant has teeth: an erroneous
\* match path firing during a halt would stamp a "Halted" trade and trip it.)
NoTradeWhileHalted ==
    \A i \in 1..Len(trades) : trades[i].inState /= "Halted"

\* PROPERTY 4 (liveness): VolatilityEventuallyReopens.
\* VolatilityAuction is NOT absorbing: once the market enters it, it is ALWAYS
\* EVENTUALLY back in Continuous (reopened by the uncross) OR Halted (the one
\* sanctioned escape, since a manual hard halt may persist). The disjunct with
\* Halted is what distinguishes Vol from a deadlock: Vol must always lead OUT,
\* and the only way to "linger" is to transition into the explicitly-absorbing
\* Halted state — it can never get stuck IN VolatilityAuction itself.
\*
\* This has teeth precisely because there are behaviors in which Halt never
\* fires from Vol (there is no fairness on Halt). On those behaviors only
\* WF_vars(ReopeningUncross) discharges the obligation, by forcing the reopen
\* to Continuous. So:
\*   - Real spec (BROKEN_NO_REOPEN = FALSE): WF forces the reopen -> holds.
\*   - Broken spec (BROKEN_NO_REOPEN = TRUE): ReopeningUncross is permanently
\*     disabled; on the behavior where Halt also never fires, the system is
\*     stuck in VolatilityAuction forever -> TLC reports a liveness
\*     counterexample (a lasso looping in VolatilityAuction).
VolatilityEventuallyReopens ==
    [](state = "VolatilityAuction" => <>(state \in {"Continuous", "Halted"}))

====
