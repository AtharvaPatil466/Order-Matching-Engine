----------------------------- MODULE SnapshotLocked -----------------------------
\* Locked variant of Snapshot.tla — models the bookLock_ shared_mutex
\* added to OrderBook to close the torn-read window. Where the unlocked
\* spec produced counterexamples for SnapshotIsPointInTime and
\* NoDoubleSidedSnapshot, this spec verifies that BOTH properties hold
\* once the reader takes a shared lock for the duration of its 2-step
\* read and the writer takes an exclusive lock for the duration of any
\* mutation.
\*
\* Closes the formal-verification cycle:
\*   1. spec/Snapshot.tla found the bug (counterexample trace shipping
\*      an order on both sides of the snapshot)
\*   2. include/OrderBook.h adds shared_mutex bookLock_; mutators take
\*      unique_lock; getSnapshot takes shared_lock
\*   3. tests/SnapshotConsistencyTest.cpp empirically validates 32k+
\*      concurrent snapshots are torn-free (also clean under TSan)
\*   4. THIS SPEC verifies the lock makes the strong consistency
\*      property hold for every reachable state.

EXTENDS Naturals, Sequences, FiniteSets, TLC

OrderIds == {"o1", "o2", "o3"}
MaxWriterOps == 6
NULL == "NULL"

VARIABLES
    bids,
    asks,
    history,
    writerOpsLeft,
    readerPC,
    readerBids,
    readerAsks,
    readerHoldsLock  \* TRUE while the reader is mid-snapshot

vars == <<bids, asks, history, writerOpsLeft, readerPC, readerBids,
          readerAsks, readerHoldsLock>>

Init ==
    /\ bids = {}
    /\ asks = {}
    /\ history = << <<{}, {}>> >>
    /\ writerOpsLeft = MaxWriterOps
    /\ readerPC = "idle"
    /\ readerBids = {}
    /\ readerAsks = {}
    /\ readerHoldsLock = FALSE

\* ---------------- Writer actions (exclusive lock) -------------------------
\* All writer actions are guarded with ~readerHoldsLock — they cannot run
\* while the reader is mid-snapshot. This models the unique_lock /
\* shared_lock exclusion.

WriterAddBid(o) ==
    /\ ~readerHoldsLock
    /\ writerOpsLeft > 0
    /\ o \in OrderIds
    /\ o \notin bids /\ o \notin asks
    /\ bids' = bids \cup {o}
    /\ history' = Append(history, <<bids \cup {o}, asks>>)
    /\ writerOpsLeft' = writerOpsLeft - 1
    /\ UNCHANGED <<asks, readerPC, readerBids, readerAsks, readerHoldsLock>>

WriterAddAsk(o) ==
    /\ ~readerHoldsLock
    /\ writerOpsLeft > 0
    /\ o \in OrderIds
    /\ o \notin bids /\ o \notin asks
    /\ asks' = asks \cup {o}
    /\ history' = Append(history, <<bids, asks \cup {o}>>)
    /\ writerOpsLeft' = writerOpsLeft - 1
    /\ UNCHANGED <<bids, readerPC, readerBids, readerAsks, readerHoldsLock>>

WriterRemove(o) ==
    /\ ~readerHoldsLock
    /\ writerOpsLeft > 0
    /\ o \in OrderIds
    /\ (o \in bids \/ o \in asks)
    /\ bids' = bids \ {o}
    /\ asks' = asks \ {o}
    /\ history' = Append(history, <<bids \ {o}, asks \ {o}>>)
    /\ writerOpsLeft' = writerOpsLeft - 1
    /\ UNCHANGED <<readerPC, readerBids, readerAsks, readerHoldsLock>>

WriterMoveBidToAsk(o) ==
    /\ ~readerHoldsLock
    /\ writerOpsLeft > 0
    /\ o \in bids
    /\ bids' = bids \ {o}
    /\ asks' = asks \cup {o}
    /\ history' = Append(history, <<bids \ {o}, asks \cup {o}>>)
    /\ writerOpsLeft' = writerOpsLeft - 1
    /\ UNCHANGED <<readerPC, readerBids, readerAsks, readerHoldsLock>>

WriterStep ==
    \E o \in OrderIds :
        \/ WriterAddBid(o)
        \/ WriterAddAsk(o)
        \/ WriterRemove(o)
        \/ WriterMoveBidToAsk(o)

\* ---------------- Reader actions (shared lock) ----------------------------
\* The reader holds the lock for the ENTIRE 2-step snapshot. Acquired in
\* ReaderStart, released in ReaderRestart (or when transitioning to
\* "done"). With the lock held, no writer action is enabled, so the
\* reader's bids and asks reads are guaranteed to come from the same
\* point-in-time view.

ReaderStart ==
    /\ readerPC = "idle"
    /\ readerPC' = "readBids"
    /\ readerBids' = {}
    /\ readerAsks' = {}
    /\ readerHoldsLock' = TRUE       \* acquire shared lock
    /\ UNCHANGED <<bids, asks, history, writerOpsLeft>>

ReaderReadBids ==
    /\ readerPC = "readBids"
    /\ readerBids' = bids
    /\ readerPC' = "readAsks"
    /\ UNCHANGED <<bids, asks, history, writerOpsLeft, readerAsks,
                   readerHoldsLock>>

ReaderReadAsks ==
    /\ readerPC = "readAsks"
    /\ readerAsks' = asks
    /\ readerPC' = "done"
    /\ UNCHANGED <<bids, asks, history, writerOpsLeft, readerBids,
                   readerHoldsLock>>

ReaderRestart ==
    /\ readerPC = "done"
    /\ readerPC' = "idle"
    /\ readerHoldsLock' = FALSE      \* release shared lock
    /\ UNCHANGED <<bids, asks, history, writerOpsLeft, readerBids,
                   readerAsks>>

ReaderStep ==
    \/ ReaderStart
    \/ ReaderReadBids
    \/ ReaderReadAsks
    \/ ReaderRestart

Next == WriterStep \/ ReaderStep
Spec == Init /\ [][Next]_vars

\* ---------------- Properties ---------------------------------------------

TypeOK ==
    /\ bids \subseteq OrderIds
    /\ asks \subseteq OrderIds
    /\ readerBids \subseteq OrderIds
    /\ readerAsks \subseteq OrderIds
    /\ readerPC \in {"idle", "readBids", "readAsks", "done"}
    /\ readerHoldsLock \in BOOLEAN
    /\ bids \cap asks = {}

\* The strong properties that FAILED in the unlocked spec must now HOLD.
\* These were the counterexamples the original Snapshot.tla produced;
\* the bookLock_ in the live code closes that window, and TLC verifies
\* the model now never reaches a state that violates either.

SnapshotIsPointInTime ==
    readerPC = "done" =>
        \E i \in 1..Len(history) :
            history[i] = <<readerBids, readerAsks>>

NoDoubleSidedSnapshot ==
    readerPC = "done" =>
        readerBids \cap readerAsks = {}

\* Sanity: the reader can only complete a snapshot while holding the
\* lock OR after releasing it (in the "idle" state holding nothing
\* between scans). Encodes the invariant that every sample we use was
\* taken inside a lock-held critical section.
ReaderConsistency ==
    (readerPC \in {"readAsks", "done"}) => readerHoldsLock

================================================================================
