---- MODULE Oco ----
(***************************************************************************)
(* Safety of One-Cancels-Other (OCO) contingent-order linkage.            *)
(*                                                                         *)
(* Models the ContingencyManager being built alongside the C++ matching   *)
(* engine. Orders are partitioned into OCO groups (siblings). The engine  *)
(* is a deterministic single writer, so the "execute a leg and cancel its *)
(* siblings" transition is atomic — one step flips the winner to Executed *)
(* and every still-Live sibling to Cancelled.                             *)
(*                                                                         *)
(* OCO semantics modelled:                                                 *)
(*   - Each order has a state: "Live", "Executed", or "Cancelled".         *)
(*   - Execute(o): enabled only when o is Live AND no sibling in o's group *)
(*     is already Executed. Atomic effect: o -> Executed, and every Live   *)
(*     sibling in o's group -> Cancelled.                                  *)
(*   - UserCancel(o): a Live order -> Cancelled at any time (the trader or *)
(*     engine cancelling a leg before it wins).                            *)
(*   - Orders not in any OCO group execute freely (no sibling linkage).    *)
(*                                                                         *)
(* Invariants proved:                                                      *)
(*   - TypeOK / ValidStates                                                *)
(*   - AtMostOneWinnerPerGroup : no group ever has >= 2 Executed members.  *)
(*   - WinnerSilencesSiblings  : a group with an Executed member has zero  *)
(*     Live members (the winner permanently silences its siblings).        *)
(*                                                                         *)
(* Non-vacuity: with CONSTANT BROKEN_NO_OCO_CANCEL = TRUE, Execute does    *)
(* NOT cancel siblings (it only flips o to Executed). TLC then reaches a   *)
(* state with two Executed legs in one group, producing a counterexample  *)
(* to AtMostOneWinnerPerGroup — proving the invariant has teeth.           *)
(***************************************************************************)

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    Orders,                \* Set of all order IDs in the model
    Groups,                \* Set of OCO groups; each group is a set of orderIds
    BROKEN_NO_OCO_CANCEL   \* Bug toggle: when TRUE, Execute skips the cancel

States == {"Live", "Executed", "Cancelled"}

VARIABLES
    state                  \* Map: orderId -> "Live" | "Executed" | "Cancelled"

vars == <<state>>

(* ─── Helpers ────────────────────────────────────────────────────── *)

\* The OCO group containing o, or {o} if o is ungrouped (free order).
\* A free order is its own (singleton) group, so it has no real siblings.
GroupOf(o) ==
    IF \E g \in Groups : o \in g
    THEN CHOOSE g \in Groups : o \in g
    ELSE {o}

\* Siblings of o: the other members of o's OCO group (excludes o itself).
SiblingsOf(o) == GroupOf(o) \ {o}

(* ─── Init ───────────────────────────────────────────────────────── *)

Init ==
    /\ state = [o \in Orders |-> "Live"]

(* ─── Execute ────────────────────────────────────────────────────── *)
(* CORRECT engine: enabled when o is Live. The atomic single step flips o *)
(* to Executed and cancels every still-Live sibling. The sibling-cancel   *)
(* is THE mechanism that enforces One-Cancels-Other: after it, no sibling  *)
(* is Live, so no sibling can ever Execute (UserCancel/Execute both need a *)
(* Live order). We do not add a separate "no sibling Executed" guard — the *)
(* cancel itself is what must carry the safety weight, so that removing it *)
(* (the bug) genuinely breaks the property.                                *)
(*                                                                         *)
(* BUG (BROKEN_NO_OCO_CANCEL = TRUE): the linkage is not enforced — Execute *)
(* only flips o to Executed and leaves siblings Live. A still-Live sibling  *)
(* can then Execute too, yielding two winners in one group.                *)

Execute(o) ==
    /\ o \in Orders
    /\ state[o] = "Live"
    /\ IF BROKEN_NO_OCO_CANCEL
       THEN \* BUG: only the winner flips; Live siblings are left untouched.
            state' = [state EXCEPT ![o] = "Executed"]
       ELSE \* CORRECT: winner -> Executed, every Live sibling -> Cancelled.
            state' = [x \in Orders |->
                        IF x = o THEN "Executed"
                        ELSE IF x \in SiblingsOf(o) /\ state[x] = "Live"
                             THEN "Cancelled"
                             ELSE state[x]]

(* ─── UserCancel ─────────────────────────────────────────────────── *)
(* A Live order may be cancelled at any time, before it wins.             *)

UserCancel(o) ==
    /\ o \in Orders
    /\ state[o] = "Live"
    /\ state' = [state EXCEPT ![o] = "Cancelled"]

(* ─── Terminal stutter ───────────────────────────────────────────── *)
(* When no order is Live, every leg is resolved (Executed or Cancelled)  *)
(* and there is nothing left to do. A fully-resolved OCO set is a legal   *)
(* final state, not a stuck system, so we add an explicit self-loop to    *)
(* keep TLC's deadlock check from flagging these terminal states.         *)

Done ==
    /\ \A o \in Orders : state[o] # "Live"
    /\ UNCHANGED vars

(* ─── Next + Spec ────────────────────────────────────────────────── *)

Next ==
    \/ \E o \in Orders :
            \/ Execute(o)
            \/ UserCancel(o)
    \/ Done

Spec == Init /\ [][Next]_vars

(* ─── Invariants ─────────────────────────────────────────────────── *)

TypeOK ==
    /\ DOMAIN state = Orders
    /\ \A o \in Orders : state[o] \in States

\* ValidStates: a slightly stronger structural sanity check — the model
\* only ever assigns the three legal states, and groups stay disjoint
\* subsets of Orders (a partition assumption the engine relies on).
ValidStates ==
    /\ \A o \in Orders : state[o] \in States
    /\ \A g \in Groups : g \subseteq Orders
    /\ \A g1, g2 \in Groups : (g1 # g2) => (g1 \cap g2 = {})

\* SAFETY (primary): no OCO group ever has two or more Executed members.
\* This is the core One-Cancels-Other guarantee.
AtMostOneWinnerPerGroup ==
    \A g \in Groups :
        Cardinality({o \in g : state[o] = "Executed"}) <= 1

\* SAFETY: once any member of a group is Executed, no sibling remains Live.
\* The winner permanently silences the rest of the group. (Encoded as an
\* invariant: a group containing an Executed member has zero Live members.)
WinnerSilencesSiblings ==
    \A g \in Groups :
        (\E o \in g : state[o] = "Executed")
            => (\A o \in g : state[o] # "Live")

====
