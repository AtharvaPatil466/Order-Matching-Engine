---------------------------- MODULE FixSession ----------------------------
\* FIX session-layer inbound sequence recovery.
\*
\* The receiver delivers an application message to the engine only when its
\* MsgSeqNum equals the expected number. A higher number is a GAP (emit a
\* ResendRequest, do NOT deliver, do NOT advance); a lower number is a
\* duplicate (ignore). This keeps the delivered stream strictly 1,2,3,...
\* with no gaps and no duplicates.
\*
\* The bug-injected variant (BROKEN_NO_GAP_DETECT = TRUE) delivers a
\* higher-than-expected message anyway (skips gap detection); TLC then finds a
\* delivered stream that is out of order — proving the property has teeth.

EXTENDS Integers, Sequences, TLC

CONSTANTS MaxSeq, BROKEN_NO_GAP_DETECT

VARIABLES expected,   \* next in-order MsgSeqNum the receiver will accept
          delivered   \* sequence of accepted application MsgSeqNums

vars == <<expected, delivered>>

TypeOK ==
    /\ expected \in 1..(MaxSeq + 1)
    /\ delivered \in Seq(1..MaxSeq)

Init ==
    /\ expected = 1
    /\ delivered = <<>>

\* An inbound message with sequence number s arrives. Messages may arrive in
\* any order and repeat (the model picks any s each step).
Receive(s) ==
    \/ /\ s = expected                          \* in order: accept + advance
       /\ delivered' = Append(delivered, s)
       /\ expected'  = expected + 1
    \/ /\ s > expected                           \* gap
       /\ IF BROKEN_NO_GAP_DETECT
          THEN /\ delivered' = Append(delivered, s)  \* BUG: accept out of order
               /\ expected'  = s + 1
          ELSE /\ UNCHANGED vars                      \* correct: resend, hold
    \/ /\ s < expected                           \* duplicate / stale: ignore
       /\ UNCHANGED vars

Next == (\E s \in 1..MaxSeq : Receive(s)) \/ UNCHANGED vars

Spec == Init /\ [][Next]_vars

\* Bound the state space: delivered can never need to exceed MaxSeq entries.
BoundedDelivered == Len(delivered) <= MaxSeq

\* Safety: the delivered application stream is exactly 1, 2, 3, ... — strictly
\* in order, no gaps, no duplicates.
DeliveredInOrder == \A i \in 1..Len(delivered) : delivered[i] = i
=============================================================================
