------------------------------- MODULE MpscQueue -------------------------------
\* TLA+ specification of the Vyukov-style array-MPSC queue used in
\* include/MpscQueue.h.
\*
\* The model is faithful to the actual algorithm at a granularity fine
\* enough to catch real concurrency bugs: each producer's push is
\* decomposed into the same atomic steps as the C++ code (read writePos,
\* read slot seq, CAS, write data, publish slot seq); the consumer's pop
\* is decomposed similarly. Any interleaving of these atomic steps across
\* producers and the consumer is allowed.
\*
\* Properties verified by TLC:
\*   - TypeOK            : type invariant on every reachable state
\*   - NoSpuriousItems   : every item popped came from some producer
\*   - NoDuplicates      : the consumer log has no repeats
\*   - PerProducerFIFO   : items from the same producer appear in the
\*                         consumer log in submission order
\*   - EventuallyDrained : under fairness, every submitted item is
\*                         eventually popped (liveness)

EXTENDS Naturals, Sequences, FiniteSets, TLC

\* --- Bounded model parameters ----------------------------------------------
\* Inlined here rather than passed via the cfg because TLC's cfg syntax
\* doesn't support record literals (needed for Inbox). Editing the model
\* size is one-line changes here.
\* Default model: small enough for sub-second TLC runs. Bump these for a
\* stronger check; the spec scales naturally (3 producers × 4 slots was
\* validated at ~15 s, ~515k distinct states, no violations).
Producers == {"p1", "p2"}
Inbox     == [p1 |-> <<"a", "b">>, p2 |-> <<"c", "d">>]
SlotCount == 2
MaxItems  == 4
NULL      == "NULL"

\* Set of all items that any producer will push (used by NoSpuriousItems).
RangeOf(s) == { s[i] : i \in 1..Len(s) }
AllItems == UNION { RangeOf(Inbox[p]) : p \in Producers }

\* ---------------- VARIABLES -------------------------------------------------

VARIABLES
    writePos,        \* shared atomic counter
    readPos,         \* counter; only the consumer touches it
    slotSeq,         \* slotSeq[i]  : current sequence number on slot i
    slotData,        \* slotData[i] : current data value on slot i (or NULL)
    producerPC,      \* per-producer program counter
    producerLocal,   \* per-producer locals: pos (current attempt) + item
    producerLog,     \* producerLog[p]: items p has successfully published
    inbox,           \* inbox[p]: items p still has to push
    consumerPC,      \* consumer program counter
    consumerLocal,   \* consumer locals: pos + data
    consumerLog      \* sequence of items popped, in pop order

vars == <<writePos, readPos, slotSeq, slotData,
          producerPC, producerLocal, producerLog, inbox,
          consumerPC, consumerLocal, consumerLog>>

\* ---------------- INIT ------------------------------------------------------

\* Slot i starts with seq = i (matches MpscQueue.h constructor).
Init ==
    /\ writePos = 0
    /\ readPos = 0
    /\ slotSeq = [i \in 0..(SlotCount - 1) |-> i]
    /\ slotData = [i \in 0..(SlotCount - 1) |-> NULL]
    /\ producerPC = [p \in Producers |-> "idle"]
    /\ producerLocal = [p \in Producers |-> [pos |-> 0, item |-> NULL]]
    /\ producerLog = [p \in Producers |-> << >>]
    /\ inbox = Inbox
    /\ consumerPC = "idle"
    /\ consumerLocal = [pos |-> 0, data |-> NULL]
    /\ consumerLog = << >>

\* ---------------- PRODUCER ACTIONS ------------------------------------------
\* PCs:
\*   "idle"      : nothing in flight; pick next item from inbox if any
\*   "attempt"   : read writePos into local pos
\*   "readSeq"   : read slotSeq[pos % SlotCount]; decide claim/full/retry
\*   "cas"       : try CAS writePos pos -> pos+1
\*   "writeData" : write slot data
\*   "publish"   : write slot seq = pos + 1 (release)
\*   "done"      : inbox empty, terminal

ProducerStart(p) ==
    /\ producerPC[p] = "idle"
    /\ Len(inbox[p]) > 0
    /\ producerPC' = [producerPC EXCEPT ![p] = "attempt"]
    /\ producerLocal' = [producerLocal EXCEPT
                            ![p] = [pos |-> 0, item |-> Head(inbox[p])]]
    /\ inbox' = [inbox EXCEPT ![p] = Tail(@)]
    /\ UNCHANGED <<writePos, readPos, slotSeq, slotData,
                   producerLog, consumerPC, consumerLocal, consumerLog>>

ProducerAttempt(p) ==
    /\ producerPC[p] = "attempt"
    /\ producerLocal' = [producerLocal EXCEPT ![p].pos = writePos]
    /\ producerPC' = [producerPC EXCEPT ![p] = "readSeq"]
    /\ UNCHANGED <<writePos, readPos, slotSeq, slotData,
                   producerLog, inbox, consumerPC, consumerLocal, consumerLog>>

\* Read slot seq; branch on three cases. Each is a separate exclusive action.
ProducerReadSeq_Claim(p) ==
    /\ producerPC[p] = "readSeq"
    /\ slotSeq[producerLocal[p].pos % SlotCount] = producerLocal[p].pos
    /\ producerPC' = [producerPC EXCEPT ![p] = "cas"]
    /\ UNCHANGED <<writePos, readPos, slotSeq, slotData,
                   producerLocal, producerLog, inbox,
                   consumerPC, consumerLocal, consumerLog>>

ProducerReadSeq_Full(p) ==
    /\ producerPC[p] = "readSeq"
    /\ slotSeq[producerLocal[p].pos % SlotCount] < producerLocal[p].pos
    \* Real code returns false; we put the item back on the front of the
    \* inbox so it will be retried later when capacity opens up.
    /\ producerPC' = [producerPC EXCEPT ![p] = "idle"]
    /\ inbox' = [inbox EXCEPT ![p] = <<producerLocal[p].item>> \o @]
    /\ UNCHANGED <<writePos, readPos, slotSeq, slotData,
                   producerLocal, producerLog,
                   consumerPC, consumerLocal, consumerLog>>

ProducerReadSeq_Retry(p) ==
    /\ producerPC[p] = "readSeq"
    /\ slotSeq[producerLocal[p].pos % SlotCount] > producerLocal[p].pos
    /\ producerPC' = [producerPC EXCEPT ![p] = "attempt"]
    /\ UNCHANGED <<writePos, readPos, slotSeq, slotData,
                   producerLocal, producerLog, inbox,
                   consumerPC, consumerLocal, consumerLog>>

\* CAS atomic: either writePos still equals our local pos (success) or
\* another producer moved it (failure → retry).
ProducerCAS_Success(p) ==
    /\ producerPC[p] = "cas"
    /\ writePos = producerLocal[p].pos
    /\ writePos' = producerLocal[p].pos + 1
    /\ producerPC' = [producerPC EXCEPT ![p] = "writeData"]
    /\ UNCHANGED <<readPos, slotSeq, slotData, producerLocal,
                   producerLog, inbox, consumerPC, consumerLocal, consumerLog>>

ProducerCAS_Fail(p) ==
    /\ producerPC[p] = "cas"
    /\ writePos # producerLocal[p].pos
    /\ producerPC' = [producerPC EXCEPT ![p] = "attempt"]
    /\ UNCHANGED <<writePos, readPos, slotSeq, slotData, producerLocal,
                   producerLog, inbox, consumerPC, consumerLocal, consumerLog>>

\* Slot is claimed (writePos was advanced) but not yet visible (slotSeq
\* is still the old value pos, so consumer sees seq < pos+1 → empty).
ProducerWriteData(p) ==
    /\ producerPC[p] = "writeData"
    /\ slotData' = [slotData EXCEPT
                       ![producerLocal[p].pos % SlotCount] =
                            producerLocal[p].item]
    /\ producerPC' = [producerPC EXCEPT ![p] = "publish"]
    /\ UNCHANGED <<writePos, readPos, slotSeq, producerLocal, producerLog,
                   inbox, consumerPC, consumerLocal, consumerLog>>

\* Publish: slot seq = pos + 1 (release). Now consumer can see it.
ProducerPublish(p) ==
    /\ producerPC[p] = "publish"
    /\ slotSeq' = [slotSeq EXCEPT
                       ![producerLocal[p].pos % SlotCount] =
                            producerLocal[p].pos + 1]
    /\ producerLog' = [producerLog EXCEPT
                          ![p] = Append(@, producerLocal[p].item)]
    /\ producerPC' = [producerPC EXCEPT
                          ![p] = IF Len(inbox[p]) > 0 THEN "idle" ELSE "done"]
    /\ UNCHANGED <<writePos, readPos, slotData, producerLocal,
                   inbox, consumerPC, consumerLocal, consumerLog>>

Producer(p) ==
    \/ ProducerStart(p)
    \/ ProducerAttempt(p)
    \/ ProducerReadSeq_Claim(p)
    \/ ProducerReadSeq_Full(p)
    \/ ProducerReadSeq_Retry(p)
    \/ ProducerCAS_Success(p)
    \/ ProducerCAS_Fail(p)
    \/ ProducerWriteData(p)
    \/ ProducerPublish(p)

\* ---------------- CONSUMER ACTIONS ------------------------------------------

ConsumerStart ==
    /\ consumerPC = "idle"
    /\ consumerLocal' = [consumerLocal EXCEPT !.pos = readPos]
    /\ consumerPC' = "readSeq"
    /\ UNCHANGED <<writePos, readPos, slotSeq, slotData,
                   producerPC, producerLocal, producerLog, inbox, consumerLog>>

ConsumerReadSeq_Empty ==
    /\ consumerPC = "readSeq"
    /\ slotSeq[consumerLocal.pos % SlotCount] < consumerLocal.pos + 1
    /\ consumerPC' = "idle"
    /\ UNCHANGED <<writePos, readPos, slotSeq, slotData,
                   producerPC, producerLocal, producerLog, inbox,
                   consumerLocal, consumerLog>>

ConsumerReadSeq_Ready ==
    /\ consumerPC = "readSeq"
    /\ slotSeq[consumerLocal.pos % SlotCount] >= consumerLocal.pos + 1
    /\ consumerPC' = "readData"
    /\ UNCHANGED <<writePos, readPos, slotSeq, slotData,
                   producerPC, producerLocal, producerLog, inbox,
                   consumerLocal, consumerLog>>

ConsumerReadData ==
    /\ consumerPC = "readData"
    /\ consumerLocal' = [consumerLocal EXCEPT
                            !.data = slotData[consumerLocal.pos % SlotCount]]
    /\ consumerPC' = "publish"
    /\ UNCHANGED <<writePos, readPos, slotSeq, slotData,
                   producerPC, producerLocal, producerLog, inbox, consumerLog>>

ConsumerPublish ==
    /\ consumerPC = "publish"
    /\ slotSeq' = [slotSeq EXCEPT
                      ![consumerLocal.pos % SlotCount] =
                           consumerLocal.pos + SlotCount]
    /\ readPos' = consumerLocal.pos + 1
    /\ consumerLog' = Append(consumerLog, consumerLocal.data)
    /\ consumerPC' = "idle"
    /\ UNCHANGED <<writePos, slotData, producerPC, producerLocal,
                   producerLog, inbox, consumerLocal>>

Consumer ==
    \/ ConsumerStart
    \/ ConsumerReadSeq_Empty
    \/ ConsumerReadSeq_Ready
    \/ ConsumerReadData
    \/ ConsumerPublish

\* ---------------- NEXT ------------------------------------------------------

Next ==
    \/ \E p \in Producers : Producer(p)
    \/ Consumer

\* ---------------- FAIRNESS --------------------------------------------------

\* Weak fairness on every action so liveness holds: a producer with work
\* eventually publishes; the consumer eventually drains.
Fairness ==
    /\ \A p \in Producers : WF_vars(Producer(p))
    /\ WF_vars(Consumer)

Spec == Init /\ [][Next]_vars /\ Fairness

\* ---------------- INVARIANTS / PROPERTIES -----------------------------------

TypeOK ==
    /\ writePos \in 0..(MaxItems + 1)
    /\ readPos  \in 0..(MaxItems + 1)
    /\ readPos <= writePos
    /\ DOMAIN slotSeq = 0..(SlotCount - 1)
    /\ DOMAIN slotData = 0..(SlotCount - 1)
    /\ \A p \in Producers : producerPC[p] \in
        {"idle", "attempt", "readSeq", "cas", "writeData", "publish", "done"}
    /\ consumerPC \in {"idle", "readSeq", "readData", "publish"}

\* SAFETY: every popped item came from some producer's published log.
NoSpuriousItems ==
    \A i \in 1..Len(consumerLog) :
        \E p \in Producers : \E j \in 1..Len(producerLog[p]) :
            producerLog[p][j] = consumerLog[i]

\* SAFETY: no duplicates in consumer log.
NoDuplicates ==
    \A i, j \in 1..Len(consumerLog) :
        i # j => consumerLog[i] # consumerLog[j]

\* SAFETY: per-producer FIFO. Items each producer published, in order,
\* must appear as a prefix-or-equal of the corresponding subsequence of
\* consumerLog (filtered to that producer's items).
PerProducerFIFO ==
    \A p \in Producers :
        LET pLog == producerLog[p]
            cLogP == SelectSeq(consumerLog,
                                LAMBDA x : \E i \in 1..Len(pLog) : pLog[i] = x)
        IN \A i \in 1..Len(cLogP) : cLogP[i] = pLog[i]

\* LIVENESS: eventually every submitted item appears in consumerLog.
EventuallyDrained ==
    <>(\A p \in Producers :
        /\ Len(inbox[p]) = 0
        /\ \A i \in 1..Len(producerLog[p]) :
            \E j \in 1..Len(consumerLog) :
                consumerLog[j] = producerLog[p][i])

================================================================================
