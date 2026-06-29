---- MODULE Risk ----
(***************************************************************************)
(* Safety of MULTI-TIER (hierarchical) pre-trade risk aggregation.         *)
(*                                                                         *)
(* Models the pre-trade RiskManager being built alongside the C++ matching *)
(* engine. Risk limits are arranged in a TWO-TIER hierarchy: a Firm sits   *)
(* above its Traders, and an order must clear EVERY tier of caps on its    *)
(* path to the root before it is admitted. The engine is a deterministic   *)
(* single writer, so checking the caps and booking the fill is one atomic  *)
(* step.                                                                   *)
(*                                                                         *)
(* NOTE ON SCOPE: this spec models a reduced TWO-TIER (Firm over           *)
(* Trader) abstraction, chosen so the state space stays exhaustively       *)
(* checkable. The production C++ HierarchicalRiskManager implements FOUR   *)
(* tiers: Trader -> Strategy -> Account -> Firm (RiskTier, kNumTiers=4).   *)
(* The two-tier model captures the same per-tier cap-check and             *)
(* aggregation safety argument; the extra Strategy/Account tiers are       *)
(* additional instances of the same check and are not modelled here.       *)
(*                                                                         *)
(* Hierarchy modelled (kept tiny for exhaustive checking):                 *)
(*   - One Firm with two Traders, T1 and T2, beneath it.                   *)
(*   - pos[e] is the net (long) position currently booked to entity e, for *)
(*     every leaf trader AND the firm aggregate node.                      *)
(*   - Buys only (sizes drawn from Qtys, e.g. {1,2}); positions are        *)
(*     monotone non-decreasing, which keeps the reachable space finite     *)
(*     without an explicit event counter (every pos[t] is bounded by the   *)
(*     trader cap, so pos[Firm] is bounded by the sum of trader caps).     *)
(*                                                                         *)
(* Admission rule modelled (src RiskManager::checkOrder, the multi-tier    *)
(* walk):                                                                   *)
(*   Submit(t, q) is admitted ONLY when BOTH                               *)
(*     (a) the trader's projected position stays within TraderCap[t], AND  *)
(*     (b) the FIRM's projected AGGREGATE position stays within FirmCap.   *)
(*   On admit, the fill is booked to BOTH tiers atomically: pos[t] += q    *)
(*   and pos[Firm] += q. A single order that would breach EITHER tier is   *)
(*   rejected outright (no partial booking).                               *)
(*                                                                         *)
(* Invariants proved:                                                      *)
(*   - TypeOK                                                               *)
(*   - FirmWithinCap        : pos[Firm] <= FirmCap in every reachable state *)
(*   - TradersWithinCap     : pos[t]    <= TraderCap[t] for every trader t  *)
(*   - FirmIsSumOfTraders   : pos[Firm] = pos[T1] + pos[T2] (the aggregate  *)
(*                            node is always the exact sum of its leaves —  *)
(*                            tier-aggregation consistency).                *)
(*                                                                         *)
(* Non-vacuity: with CONSTANT BROKEN_NO_FIRM_AGG = TRUE, Submit's guard     *)
(* consults ONLY the trader cap and SKIPS the firm-aggregate check (tier b *)
(* is dropped), while still booking the fill to both tiers. TLC then        *)
(* reaches a state where the two traders, each individually within its own  *)
(* cap, together drive pos[Firm] above FirmCap — a counterexample to        *)
(* FirmWithinCap. This proves the firm-aggregate guard is what carries the  *)
(* safety weight, i.e. the invariant has teeth.                            *)
(***************************************************************************)

EXTENDS Integers, FiniteSets, TLC

CONSTANTS
    Traders,             \* Set of leaf trader IDs beneath the firm, e.g. {T1, T2}
    Firm,                \* The single firm-tier aggregate node ID
    FirmCap,             \* Firm-level net-position cap (the aggregate limit)
    PerTraderCap,        \* Per-trader net-position cap (one tier below the firm)
    Qtys,                \* Set of admissible order sizes (buys), e.g. {1, 2}
    BROKEN_NO_FIRM_AGG   \* Bug toggle: when TRUE, Submit skips the firm-tier check

\* Per-trader cap, indexed by trader. Each leaf trader carries its OWN cap
\* entry (here a uniform PerTraderCap, kept as a function so the lower tier is
\* genuinely per-entity and the spec generalizes to non-uniform caps).
TraderCap == [t \in Traders |-> PerTraderCap]

\* Every entity that carries a tracked position: the leaf traders plus the
\* firm aggregate node. (Firm is assumed distinct from every trader.)
Entities == Traders \cup {Firm}

VARIABLES
    pos                  \* Map: entity -> net position currently booked to it

vars == <<pos>>

(* ─── Init ───────────────────────────────────────────────────────────── *)
(* Flat book: every tier starts at zero net position.                      *)

Init ==
    /\ pos = [e \in Entities |-> 0]

(* ─── Submit ─────────────────────────────────────────────────────────── *)
(* CORRECT RiskManager: a trader t submits a buy of size q. Admission walks *)
(* the hierarchy and requires the projected position to stay within the cap *)
(* at EVERY tier — the trader's own cap AND the firm aggregate cap. Only    *)
(* then is the fill booked, and it is booked to BOTH tiers atomically.      *)
(*                                                                         *)
(* The firm-aggregate guard (pos[Firm] + q <= FirmCap) is THE mechanism    *)
(* that bounds the firm position: each trader stays under its own cap, but  *)
(* only the aggregate check stops their COMBINED flow from breaching the    *)
(* firm cap. We deliberately do not add any redundant guard, so that        *)
(* removing the aggregate check (the bug) genuinely breaks the property.    *)
(*                                                                         *)
(* BUG (BROKEN_NO_FIRM_AGG = TRUE): the firm tier is not consulted — Submit *)
(* checks ONLY the trader cap and books to both tiers anyway. Two traders,  *)
(* each within its own cap, can then drive pos[Firm] past FirmCap.          *)

\* Multi-tier admission predicate. The trader-tier cap is ALWAYS enforced;
\* the firm-tier (aggregate) cap is enforced only in the correct model. Under
\* the bug toggle the firm disjunct is satisfied vacuously, so the aggregate
\* tier is effectively skipped.
CanAdmit(t, q) ==
    /\ pos[t] + q <= TraderCap[t]                        \* trader-tier check (always)
    /\ \/ BROKEN_NO_FIRM_AGG                              \* BUG: firm tier skipped
       \/ pos[Firm] + q <= FirmCap                        \* CORRECT: firm-tier check

Submit(t, q) ==
    /\ t \in Traders
    /\ q \in Qtys
    /\ CanAdmit(t, q)
    /\ pos' = [pos EXCEPT ![t] = @ + q,                  \* book to leaf tier ...
                          ![Firm] = @ + q]               \* ... and to firm aggregate

(* ─── Terminal stutter ───────────────────────────────────────────────── *)
(* Buys only, so positions never decrease. When no trader can admit any     *)
(* further size at every tier the system is saturated — a legal quiescent   *)
(* state, not a stuck one. An explicit self-loop keeps TLC's deadlock check  *)
(* from flagging these terminal states.                                     *)

Done ==
    /\ \A t \in Traders : \A q \in Qtys : ~CanAdmit(t, q)
    /\ UNCHANGED vars

(* ─── Next + Spec ────────────────────────────────────────────────────── *)

Next ==
    \/ \E t \in Traders : \E q \in Qtys : Submit(t, q)
    \/ Done

Spec == Init /\ [][Next]_vars

(* ─── Invariants ─────────────────────────────────────────────────────── *)

\* Sum of pos[.] over a set of trader IDs (recursive fold over the subset).
\* Used to express the firm aggregate as the exact sum of its leaf traders
\* without hard-coding individual trader names.
SumPos(S) == LET f[T \in SUBSET Traders] ==
                   IF T = {} THEN 0
                   ELSE LET x == CHOOSE e \in T : TRUE
                        IN pos[x] + f[T \ {x}]
             IN f[S]

\* The sum of all per-trader caps — the true upper bound on the firm position
\* in either model (the broken model can reach it; the correct one cannot
\* exceed FirmCap). Used only to give TypeOK a finite, model-independent
\* bound on pos[Firm]: each trader is always capped by its own TraderCap, so
\* the firm aggregate can never exceed this sum.
SumTraderCaps == LET f[T \in SUBSET Traders] ==
                       IF T = {} THEN 0
                       ELSE LET x == CHOOSE e \in T : TRUE
                            IN TraderCap[x] + f[T \ {x}]
                 IN f[Traders]

TypeOK ==
    /\ DOMAIN pos = Entities
    /\ \A t \in Traders : pos[t] \in 0..TraderCap[t]
    /\ pos[Firm] \in 0..SumTraderCaps

\* SAFETY (primary): the firm aggregate position never exceeds the firm cap.
\* This is the core multi-tier guarantee — it holds ONLY because Submit
\* enforces the firm-aggregate guard. Dropping that guard (the bug) violates
\* it: this is the invariant the non-vacuity config makes TLC refute.
FirmWithinCap ==
    pos[Firm] <= FirmCap

\* SAFETY: every leaf trader stays within its own per-tier cap. (Holds in
\* both models — the trader-tier guard is never removed — so it documents the
\* lower tier of the hierarchy and stays true even under the injected bug.)
TradersWithinCap ==
    \A t \in Traders : pos[t] <= TraderCap[t]

\* SAFETY: tier-aggregation consistency. The firm aggregate node always
\* equals the exact sum of its leaf traders' positions. Every admit books to
\* both tiers in lock-step, so the aggregate can never drift from its leaves;
\* a bug that updated one tier without the other would break this.
FirmIsSumOfTraders ==
    pos[Firm] = SumPos(Traders)

====
