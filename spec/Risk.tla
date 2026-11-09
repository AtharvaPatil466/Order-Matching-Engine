---- MODULE Risk ----
(***************************************************************************)
(* Safety of MULTI-TIER (hierarchical) pre-trade risk with a RESERVATION   *)
(* LEDGER: what src/HierarchicalRiskManager.cpp and the engine's position  *)
(* ledger (MatchingEngine::tryReserveWorking) do since roadmap 1.9 I2.     *)
(*                                                                         *)
(* Each tier entity keeps a net position and, per side, the working        *)
(* quantity of its live orders. An order must clear EVERY tier on its path *)
(* to the root before it is admitted, and admission RESERVES its quantity  *)
(* as working at every tier in the same step (checkAndReserve under        *)
(* riskMutex_; a CAS for the engine-wide limit), so no admission can pass  *)
(* against a figure another one is about to change (THR-3).                *)
(*                                                                         *)
(* What an entity could hold is per side, working orders included (TLA-5):*)
(*   buy reach  = net + working buys                                       *)
(*   sell reach = working sells - net                                      *)
(* Opposite sides do not net: either can fill alone.                       *)
(*                                                                         *)
(* After admission:                                                        *)
(*   Fill    — q of an order trades: working -> net, at every tier.        *)
(*   Release — q leaves without trading (cancel, IOC/FOK remainder, STP,   *)
(*             kill, expiry, DAY close, a replace that shrinks): working   *)
(*             shrinks at every tier (OrderBook::untrackOrder).            *)
(* A replace that grows is a Submit of the growth.                         *)
(*                                                                         *)
(* An earlier version booked position at admission, which the code never   *)
(* did: the code checked net position only and ignored working orders.     *)
(*                                                                         *)
(* NOTE ON SCOPE: TWO tiers (Firm over Traders), kept tiny for exhaustive  *)
(* checking. The C++ manager has four (Trader -> Strategy -> Account ->    *)
(* Firm); the middle two are more instances of the same check.             *)
(*                                                                         *)
(* Invariants proved:                                                      *)
(*   - TypeOK                                                               *)
(*   - FirmWithinCap        : neither side of the firm can reach past       *)
(*                            FirmCap, working orders included              *)
(*   - TradersWithinCap     : the same for every trader and its own cap     *)
(*   - FirmIsSumOfTraders   : the firm's net and working are the exact sums *)
(*                            of its traders' (tier-aggregation consistency)*)
(*                                                                         *)
(* Non-vacuity: with BROKEN_NO_FIRM_AGG = TRUE, Submit consults only the    *)
(* trader cap and still reserves at both tiers. Two traders, each within    *)
(* its own cap, then reach past FirmCap together — a counterexample to      *)
(* FirmWithinCap (RiskBroken.cfg, run by tla.yml).                          *)
(***************************************************************************)

EXTENDS Integers, FiniteSets, TLC

CONSTANTS
    Traders,             \* Set of leaf trader IDs beneath the firm, e.g. {T1, T2}
    Firm,                \* The single firm-tier aggregate node ID
    FirmCap,             \* Firm-level position cap (the aggregate limit)
    PerTraderCap,        \* Per-trader position cap (one tier below the firm)
    Qtys,                \* Set of admissible order sizes, e.g. {1, 2}
    BROKEN_NO_FIRM_AGG   \* Bug toggle: when TRUE, Submit skips the firm-tier check

TraderCap == [t \in Traders |-> PerTraderCap]

Entities == Traders \cup {Firm}
Sides == {"Buy", "Sell"}

VARIABLES
    net,                 \* entity -> net filled position (buys - sells)
    work                 \* entity -> side -> working quantity reserved

vars == <<net, work>>

\* What entity e could hold on side s if everything working there filled.
Reach(e, s) == IF s = "Buy" THEN net[e] + work[e][s] ELSE work[e][s] - net[e]

Cap(e) == IF e = Firm THEN FirmCap ELSE TraderCap[e]

Init ==
    /\ net = [e \in Entities |-> 0]
    /\ work = [e \in Entities |-> [s \in Sides |-> 0]]

(* ─── Submit: check every tier and reserve, in one step ─────────────── *)
CanAdmit(t, s, q) ==
    /\ Reach(t, s) + q <= TraderCap[t]                   \* trader tier (always)
    /\ \/ BROKEN_NO_FIRM_AGG                             \* BUG: firm tier skipped
       \/ Reach(Firm, s) + q <= FirmCap                  \* CORRECT: firm tier

Submit(t, s, q) ==
    /\ CanAdmit(t, s, q)
    /\ work' = [work EXCEPT ![t][s] = @ + q, ![Firm][s] = @ + q]
    /\ UNCHANGED net

(* ─── Fill: q of t's working quantity on s trades ───────────────────── *)
Signed(s, q) == IF s = "Buy" THEN q ELSE -q

Fill(t, s, q) ==
    /\ q <= work[t][s]
    /\ net' = [net EXCEPT ![t] = @ + Signed(s, q), ![Firm] = @ + Signed(s, q)]
    /\ work' = [work EXCEPT ![t][s] = @ - q, ![Firm][s] = @ - q]

(* ─── Release: q of t's working quantity leaves without trading ─────── *)
Release(t, s, q) ==
    /\ q <= work[t][s]
    /\ work' = [work EXCEPT ![t][s] = @ - q, ![Firm][s] = @ - q]
    /\ UNCHANGED net

Next ==
    \E t \in Traders, s \in Sides, q \in Qtys :
        \/ Submit(t, s, q)
        \/ Fill(t, s, q)
        \/ Release(t, s, q)

Spec == Init /\ [][Next]_vars

(* ─── Invariants ─────────────────────────────────────────────────────── *)

SumOver(f(_), S) == LET g[T \in SUBSET S] ==
                          IF T = {} THEN 0
                          ELSE LET x == CHOOSE e \in T : TRUE
                               IN f(x) + g[T \ {x}]
                    IN g[S]

SumTraderCaps == SumOver(LAMBDA t : TraderCap[t], Traders)

\* Working on a side is bounded by the cap plus a position held the other way:
\* long 2 under a cap of 2 may work sells of 4.
TypeOK ==
    /\ DOMAIN net = Entities
    /\ DOMAIN work = Entities
    /\ \A t \in Traders :
          /\ net[t] \in -TraderCap[t]..TraderCap[t]
          /\ \A s \in Sides : work[t][s] \in 0..(2 * TraderCap[t])
    /\ net[Firm] \in -SumTraderCaps..SumTraderCaps
    /\ \A s \in Sides : work[Firm][s] \in 0..(2 * SumTraderCaps)

\* SAFETY (primary): the firm cannot reach past its cap on either side, with
\* every working order counted. Holds only because Submit checks and reserves
\* at the firm tier; RiskBroken.cfg refutes it.
FirmWithinCap ==
    \A s \in Sides : Reach(Firm, s) <= FirmCap

\* SAFETY: the same for every trader against its own cap.
TradersWithinCap ==
    \A t \in Traders : \A s \in Sides : Reach(t, s) <= TraderCap[t]

\* SAFETY: every step moves both tiers in lock-step, so the firm's net and
\* working are always the sums of its traders'.
FirmIsSumOfTraders ==
    /\ net[Firm] = SumOver(LAMBDA t : net[t], Traders)
    /\ \A s \in Sides : work[Firm][s] = SumOver(LAMBDA t : work[t][s], Traders)

====
