---- MODULE TraceMatchingEngine ----
(***************************************************************************)
(* Trace validation for MatchingEngine.tla: is what the C++ book did a     *)
(* behaviour of the spec?                                                  *)
(*                                                                         *)
(* tests/TraceValidationTest.cpp drives OrderBook with Limit orders,       *)
(* Cancel, Modify and Replace (default STP, two participants, prices 100  *)
(* and 200) and records, per request, the spec action it corresponds to:   *)
(* the submit, cancel, modify or replace itself, then for an arrival each  *)
(* fill in the order the book printed it and how the arrival ended (rested *)
(* or STP-cancelled). It writes those as TLA+ records in a module          *)
(* TraceData that defines Traces, a sequence of traces.                    *)
(*                                                                         *)
(* TNext lets an action of MatchingEngine.tla fire only when it is the     *)
(* trace's next event, with the same arguments and, for a fill, the same   *)
(* aggressor, resting order, price and quantity. If the spec cannot take   *)
(* the event the code took, the state has no successor and TLC stops with  *)
(* "Deadlock reached": that is a divergence between spec and code. A trace *)
(* followed to its end stutters in TraceDone.                              *)
(*                                                                         *)
(* Order ids are the spec's: the code's ids are 1, 2, ... in submit order, *)
(* which is the spec's index into `orders`. Time is the spec's counter.    *)
(***************************************************************************)

EXTENDS MatchingEngine, TraceData

VARIABLES t, i     \* which trace, and the index of its next event

tvars == <<vars, t, i>>

Ev   == Traces[t][i]
More == i <= Len(Traces[t])

TInit ==
    /\ Init
    /\ t \in 1..Len(Traces)
    /\ i = 1

\* The next event, taken by action A.
Take(A) == More /\ A /\ i' = i + 1 /\ UNCHANGED t

TraceDone ==
    /\ ~More
    /\ aggressor = 0
    /\ UNCHANGED tvars

TNext ==
    \/ Take(Ev.op = "submit" /\ Submit(Ev.pt, Ev.side, Ev.price, Ev.qty))
    \/ Take(/\ Ev.op = "fill"
            /\ Match
            /\ lastTrade' = [aggressorId |-> Ev.agg, restingId |-> Ev.rest,
                             price |-> Ev.price, qty |-> Ev.qty])
    \/ Take(Ev.op = "stp" /\ aggressor = Ev.o /\ StpCancelIncoming)
    \/ Take(Ev.op = "rest" /\ aggressor = Ev.o /\ Rest)
    \/ Take(Ev.op = "cancel" /\ Cancel(Ev.o))
    \/ Take(Ev.op = "modify" /\ Modify(Ev.o, Ev.qty))
    \/ Take(Ev.op = "replace" /\ Replace(Ev.o, Ev.price))
    \/ TraceDone

TSpec == TInit /\ [][TNext]_tvars

====
