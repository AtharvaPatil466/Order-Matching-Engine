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
\* `value` — but only if `value` was sampled BEFORE the empty check: after
\* the previous successful pop, or after the previous wake, as workerLoop
\* does (observedWake is loaded at the top of the loop, after each pop and
\* after each wait; the failed pop and the wait are separate steps). The
\* model checks that this sampling discipline is sufficient.
\*
\* Not modelled: the Shutdown request and the workersShouldStop_ backstop,
\* the checkpoint cut (holdForCut), the spin/yield phase before parking
\* (it only delays the wait), and memory ordering: every step is
\* sequentially consistent, while the C++ loads and waits are relaxed.
\*
\* Termination: once every item is consumed the worker parks for good.
\* That idle state is the only accepted end (Idle below); TLC's deadlock
\* check fails any other state with nothing enabled, which is what a lost
\* wake-up looks like when the producers are done.
\*
\* Non-vacuity: CONSTANT BUG
\*   "snapshot_at_sleep"  the worker samples wakeCount after its empty
\*                        check, just before wait(): a push in between is
\*                        lost -> "Deadlock reached" (EngineConsumerBroken)
\*
\* The MPSC queue itself is modeled abstractly as a sequence here; its
\* internal correctness is checked separately by MpscQueue.tla.

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANT BUG    \* "none" in EngineConsumer.cfg; see the header

ASSUME BUG \in {"none", "snapshot_at_sleep"}

\* --- Bounded model ----------------------------------------------------------
\* 2 producers x 2 items: sub-second TLC runs.
Producers == {"p1", "p2"}
Inbox     == [p1 |-> <<"a", "b">>, p2 |-> <<"c", "d">>]
MaxItems  == 4

\* --- Variables --------------------------------------------------------------

VARIABLES
    queue,              \* Seq of items currently in the MPSC queue
    wakeCount,          \* shared atomic; producers increment, consumer reads
    inbox,              \* per-producer pending items
    consumerState,      \* "running", "idle" (pop failed) or "sleeping"
    consumerObserved,   \* observedWake: the value wait() will be passed
    consumerLog         \* items popped, in pop order

vars == <<queue, wakeCount, inbox, consumerState, consumerObserved, consumerLog>>

\* --- Init -------------------------------------------------------------------

\* observedWake is loaded before the loop starts.
Init ==
    /\ queue = << >>
    /\ wakeCount = 0
    /\ inbox = Inbox
    /\ consumerState = "running"
    /\ consumerObserved = 0
    /\ consumerLog = << >>

\* --- Producer ---------------------------------------------------------------

\* A producer's atomic step: append to queue, increment wakeCount.
\* (The C++ code does push -> fetch_add -> notify_one. The model collapses
\* these into one step; the increment follows the publish, which is the
\* order that matters.)
ProducerPush(p) ==
    /\ Len(inbox[p]) > 0
    /\ queue' = Append(queue, Head(inbox[p]))
    /\ wakeCount' = wakeCount + 1
    /\ inbox' = [inbox EXCEPT ![p] = Tail(@)]
    /\ UNCHANGED <<consumerState, consumerObserved, consumerLog>>

\* --- Consumer ---------------------------------------------------------------

\* A successful pop, then observedWake = load(wakeCount).
ConsumerPop ==
    /\ consumerState = "running"
    /\ Len(queue) > 0
    /\ consumerLog' = Append(consumerLog, Head(queue))
    /\ queue' = Tail(queue)
    /\ consumerObserved' = wakeCount
    /\ UNCHANGED <<wakeCount, inbox, consumerState>>

\* The pop fails: the queue is empty. Nothing is sampled here.
ConsumerFindEmpty ==
    /\ consumerState = "running"
    /\ Len(queue) = 0
    /\ consumerState' = "idle"
    /\ UNCHANGED <<queue, wakeCount, inbox, consumerObserved, consumerLog>>

\* wait(observedWake) with the value sampled before the empty check. A push
\* since then has moved wakeCount past it, so ConsumerWake fires at once.
\* Under BUG the value is sampled here instead, after the check.
ConsumerSleep ==
    /\ consumerState = "idle"
    /\ consumerState' = "sleeping"
    /\ consumerObserved' = IF BUG = "snapshot_at_sleep" THEN wakeCount
                                                       ELSE consumerObserved
    /\ UNCHANGED <<queue, wakeCount, inbox, consumerLog>>

\* atomic::wait(value) returns once the atomic != value; then
\* observedWake = load(wakeCount).
ConsumerWake ==
    /\ consumerState = "sleeping"
    /\ wakeCount > consumerObserved
    /\ consumerState' = "running"
    /\ consumerObserved' = wakeCount
    /\ UNCHANGED <<queue, wakeCount, inbox, consumerLog>>

\* Everything produced has been consumed and the worker is parked with no
\* wake pending: the behaviour ends here.
Idle ==
    /\ \A p \in Producers : Len(inbox[p]) = 0
    /\ Len(queue) = 0
    /\ consumerState = "sleeping"
    /\ wakeCount = consumerObserved
    /\ UNCHANGED vars

\* --- Next + Fairness --------------------------------------------------------

Next ==
    \/ \E p \in Producers : ProducerPush(p)
    \/ ConsumerPop
    \/ ConsumerFindEmpty
    \/ ConsumerSleep
    \/ ConsumerWake
    \/ Idle

\* Weak fairness on every action: a producer with work eventually pushes;
\* the consumer eventually pops, gives up, sleeps, or wakes.
Fairness ==
    /\ \A p \in Producers : WF_vars(ProducerPush(p))
    /\ WF_vars(ConsumerPop)
    /\ WF_vars(ConsumerFindEmpty)
    /\ WF_vars(ConsumerSleep)
    /\ WF_vars(ConsumerWake)

Spec == Init /\ [][Next]_vars /\ Fairness

\* --- Properties -------------------------------------------------------------

TypeOK ==
    /\ wakeCount \in 0..(MaxItems + 1)
    /\ consumerObserved \in 0..(MaxItems + 1)
    /\ consumerObserved <= wakeCount
    /\ consumerState \in {"running", "idle", "sleeping"}

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
