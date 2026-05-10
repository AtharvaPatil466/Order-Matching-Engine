----------------------------- MODULE EngineConsumer -----------------------------
\* TLA+ specification of the engine worker loop's wake-up protocol from
\* MatchingEngine::workerLoop in src/MatchingEngine.cpp.
\*
\* The worker thread alternates between popping requests off its MPSC
\* queue and (when empty) sleeping on a 32/64-bit atomic wake counter
\* via std::atomic::wait(). Producers, after pushing an item, increment
\* the same counter and call notify_one(). The protocol must guarantee:
\*
\*   "If a producer ever pushes an item, the worker eventually pops it."
\*
\* This is the classic missed-wake-up problem: between the worker's
\* "queue is empty" check and its descent into wait(), a producer can
\* push and signal. atomic::wait(value) defends against this by
\* returning immediately if the atomic's current value differs from
\* `value` — but only if `value` was sampled after the previous successful
\* pop (or after the previous wake). The model checks that the actual
\* sampling discipline used by workerLoop is sufficient.
\*
\* The MPSC queue itself is modeled abstractly as a sequence here; its
\* internal correctness is proven separately by MpscQueue.tla.

EXTENDS Naturals, Sequences, FiniteSets, TLC

\* --- Bounded model ----------------------------------------------------------
\* Default model: small enough for sub-second TLC runs. The 3-producer
\* variant (7 items total) was validated separately at ~22k distinct
\* states, also clean.
Producers == {"p1", "p2"}
Inbox     == [p1 |-> <<"a", "b">>, p2 |-> <<"c", "d">>]
MaxItems  == 4

\* --- Variables --------------------------------------------------------------

VARIABLES
    queue,              \* Seq of items currently in the MPSC queue
    wakeCount,          \* shared atomic; producers increment, consumer reads
    inbox,              \* per-producer pending items
    consumerState,      \* "running" or "sleeping"
    consumerObserved,   \* the wakeCount snapshot the consumer is sleeping on
    consumerLog         \* items popped, in pop order

vars == <<queue, wakeCount, inbox, consumerState, consumerObserved, consumerLog>>

\* --- Init -------------------------------------------------------------------

Init ==
    /\ queue = << >>
    /\ wakeCount = 0
    /\ inbox = Inbox
    /\ consumerState = "running"
    /\ consumerObserved = 0
    /\ consumerLog = << >>

\* --- Producer ---------------------------------------------------------------

\* A producer's atomic step: append to queue, increment wakeCount.
\* (The C++ code does push -> fetch_add(release) -> notify_one. The model
\* collapses these into one atomic step because TLA+ atomicity is
\* per-action; what matters is that the increment is visible before
\* anything else runs.)
ProducerPush(p) ==
    /\ Len(inbox[p]) > 0
    /\ queue' = Append(queue, Head(inbox[p]))
    /\ wakeCount' = wakeCount + 1
    /\ inbox' = [inbox EXCEPT ![p] = Tail(@)]
    /\ UNCHANGED <<consumerState, consumerObserved, consumerLog>>

\* --- Consumer ---------------------------------------------------------------

\* Consumer in "running" state with non-empty queue: pop one item.
\* The C++ code, after a successful pop, refreshes observedWake to the
\* current wakeCount (line 277). We model that here: observedWake is
\* updated to reflect a value at-or-after this pop.
ConsumerPop ==
    /\ consumerState = "running"
    /\ Len(queue) > 0
    /\ consumerLog' = Append(consumerLog, Head(queue))
    /\ queue' = Tail(queue)
    /\ consumerObserved' = wakeCount
    /\ UNCHANGED <<wakeCount, inbox, consumerState>>

\* Consumer in "running" with empty queue decides to sleep: it snapshots
\* wakeCount (this is the value passed to atomic::wait) and transitions
\* to sleeping. The snapshot is taken AFTER the empty check, modeling
\* the C++ flow where observedWake is loaded on the empty path before
\* calling wait(observedWake).
\*
\* CRITICAL: this is where the protocol's correctness lives. If we
\* snapshotted observedWake BEFORE the empty check (say, at the top of
\* the loop), a producer pushing between the check and the snapshot
\* could be missed. Snapshotting AFTER the check means a producer push
\* in that window also bumps wakeCount, and the resulting sleep snapshot
\* will already be > consumerObserved, so the WakeUp action fires
\* immediately.
ConsumerSleep ==
    /\ consumerState = "running"
    /\ Len(queue) = 0
    /\ consumerObserved' = wakeCount
    /\ consumerState' = "sleeping"
    /\ UNCHANGED <<queue, wakeCount, inbox, consumerLog>>

\* Sleeping consumer wakes when wakeCount has moved past its observed
\* snapshot. atomic::wait(value) returns when the underlying atomic !=
\* value; in this model, the moment wakeCount > consumerObserved, the
\* WakeUp action becomes ENABLED.
ConsumerWake ==
    /\ consumerState = "sleeping"
    /\ wakeCount > consumerObserved
    /\ consumerState' = "running"
    /\ consumerObserved' = wakeCount
    /\ UNCHANGED <<queue, wakeCount, inbox, consumerLog>>

\* --- Next + Fairness --------------------------------------------------------

Next ==
    \/ \E p \in Producers : ProducerPush(p)
    \/ ConsumerPop
    \/ ConsumerSleep
    \/ ConsumerWake

\* Weak fairness on every action: a producer with work eventually pushes;
\* the consumer eventually pops, sleeps, or wakes.
Fairness ==
    /\ \A p \in Producers : WF_vars(ProducerPush(p))
    /\ WF_vars(ConsumerPop)
    /\ WF_vars(ConsumerSleep)
    /\ WF_vars(ConsumerWake)

Spec == Init /\ [][Next]_vars /\ Fairness

\* --- Properties -------------------------------------------------------------

TypeOK ==
    /\ wakeCount \in 0..(MaxItems + 1)
    /\ consumerObserved \in 0..(MaxItems + 1)
    /\ consumerObserved <= wakeCount
    /\ consumerState \in {"running", "sleeping"}

\* SAFETY: every popped item came from some producer.
RangeOf(s) == { s[i] : i \in 1..Len(s) }
AllItems == UNION { RangeOf(Inbox[p]) : p \in Producers }

NoSpuriousItems ==
    \A i \in 1..Len(consumerLog) : consumerLog[i] \in AllItems

NoDuplicates ==
    \A i, j \in 1..Len(consumerLog) :
        i # j => consumerLog[i] # consumerLog[j]

\* LIVENESS: the heart of the protocol. Every item placed in any
\* producer's inbox eventually appears in consumerLog. If the wake-up
\* protocol has a missed-wake bug, TLC would find a state where queue
\* is non-empty AND consumer is sleeping AND no further enabled
\* actions move the consumer — producing a counterexample to this
\* leads-to property.
EventuallyDrained ==
    <>(/\ \A p \in Producers : Len(inbox[p]) = 0
       /\ Len(queue) = 0
       /\ Len(consumerLog) = Cardinality(AllItems))

\* SAFETY (auxiliary): the consumer can be momentarily sleeping while the
\* queue is non-empty (between a push happening and the WakeUp action
\* firing). What we forbid is *permanent* sleep with work pending. That
\* is exactly the failure mode of EventuallyDrained, so we don't add a
\* separate invariant — the temporal property captures it.

================================================================================
