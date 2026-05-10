-------------------------------- MODULE Snapshot --------------------------------
\* Snapshot-read consistency under concurrent writes.
\*
\* Models OrderBook::getSnapshot in src/OrderBook.cpp, which walks bids
\* and then asks without holding any lock. Writers (addOrder /
\* cancelOrder) mutate the same data structures concurrently. The
\* question this spec answers:
\*
\*   Q1. Are the orders in the returned snapshot all "real" — i.e. was
\*       each one actually in the book at some point during the read?
\*       (Property: SnapshotEntriesAreReal)
\*
\*   Q2. Is the snapshot a single point-in-time view of the book?
\*       (Property: SnapshotIsPointInTime — INTENTIONALLY fails for the
\*       lockless implementation, demonstrating the torn-read window.)
\*
\* The model uses bags-of-orders rather than the full price-level
\* structure because all the interesting concurrency lives in the
\* bid-then-ask sequencing of the reader, not in the per-level layout.

EXTENDS Naturals, Sequences, FiniteSets, TLC

\* ---------------- Bounded model -------------------------------------------

\* Set of order IDs that may exist in the book during this run.
OrderIds == {"o1", "o2", "o3"}

\* Maximum number of writer operations to run before the model halts.
MaxWriterOps == 6

\* ---------------- Variables -----------------------------------------------

VARIABLES
    bids,              \* Set of orderIds currently on the bid side
    asks,              \* Set of orderIds currently on the ask side
    history,           \* Seq of (bids, asks) pairs after each writer step
                       \* (used to check Q1 — "was this order ever there?")
    writerOpsLeft,     \* Decrements until 0; bounds the model
    readerPC,          \* "idle" | "readBids" | "readAsks" | "done"
    readerBids,        \* The bid copy the reader has captured so far
    readerAsks         \* The ask copy

vars == <<bids, asks, history, writerOpsLeft, readerPC, readerBids, readerAsks>>

\* ---------------- Init ----------------------------------------------------

Init ==
    /\ bids = {}
    /\ asks = {}
    /\ history = << <<{}, {}>> >>  \* one initial snapshot pair
    /\ writerOpsLeft = MaxWriterOps
    /\ readerPC = "idle"
    /\ readerBids = {}
    /\ readerAsks = {}

\* ---------------- Writer actions ------------------------------------------
\* Each writer action is atomic and updates `history` so we can verify
\* "was this order ever in the book on this side" after the run.

WriterAddBid(o) ==
    /\ writerOpsLeft > 0
    /\ o \in OrderIds
    /\ o \notin bids /\ o \notin asks
    /\ bids' = bids \cup {o}
    /\ history' = Append(history, <<bids \cup {o}, asks>>)
    /\ writerOpsLeft' = writerOpsLeft - 1
    /\ UNCHANGED <<asks, readerPC, readerBids, readerAsks>>

WriterAddAsk(o) ==
    /\ writerOpsLeft > 0
    /\ o \in OrderIds
    /\ o \notin bids /\ o \notin asks
    /\ asks' = asks \cup {o}
    /\ history' = Append(history, <<bids, asks \cup {o}>>)
    /\ writerOpsLeft' = writerOpsLeft - 1
    /\ UNCHANGED <<bids, readerPC, readerBids, readerAsks>>

WriterRemove(o) ==
    /\ writerOpsLeft > 0
    /\ o \in OrderIds
    /\ (o \in bids \/ o \in asks)
    /\ bids' = bids \ {o}
    /\ asks' = asks \ {o}
    /\ history' = Append(history, <<bids \ {o}, asks \ {o}>>)
    /\ writerOpsLeft' = writerOpsLeft - 1
    /\ UNCHANGED <<readerPC, readerBids, readerAsks>>

\* Move-side: cancel from one side and add to the other in a single
\* atomic step. Approximates a CancelReplace that flips side. The
\* interesting effect is that an order leaves bids and joins asks at
\* the same instant; a reader straddling the two reads can see the
\* order on neither side or on both.
WriterMoveBidToAsk(o) ==
    /\ writerOpsLeft > 0
    /\ o \in bids
    /\ bids' = bids \ {o}
    /\ asks' = asks \cup {o}
    /\ history' = Append(history, <<bids \ {o}, asks \cup {o}>>)
    /\ writerOpsLeft' = writerOpsLeft - 1
    /\ UNCHANGED <<readerPC, readerBids, readerAsks>>

WriterStep ==
    \E o \in OrderIds :
        \/ WriterAddBid(o)
        \/ WriterAddAsk(o)
        \/ WriterRemove(o)
        \/ WriterMoveBidToAsk(o)

\* ---------------- Reader actions ------------------------------------------
\* Two-step snapshot: read bids, then read asks. The two reads are NOT
\* atomic with each other — writers can run between them.

ReaderStart ==
    /\ readerPC = "idle"
    /\ readerPC' = "readBids"
    /\ readerBids' = {}
    /\ readerAsks' = {}
    /\ UNCHANGED <<bids, asks, history, writerOpsLeft>>

ReaderReadBids ==
    /\ readerPC = "readBids"
    /\ readerBids' = bids
    /\ readerPC' = "readAsks"
    /\ UNCHANGED <<bids, asks, history, writerOpsLeft, readerAsks>>

ReaderReadAsks ==
    /\ readerPC = "readAsks"
    /\ readerAsks' = asks
    /\ readerPC' = "done"
    /\ UNCHANGED <<bids, asks, history, writerOpsLeft, readerBids>>

ReaderRestart ==
    /\ readerPC = "done"
    /\ readerPC' = "idle"
    /\ UNCHANGED <<bids, asks, history, writerOpsLeft, readerBids, readerAsks>>

ReaderStep ==
    \/ ReaderStart
    \/ ReaderReadBids
    \/ ReaderReadAsks
    \/ ReaderRestart

\* ---------------- Next + Spec --------------------------------------------

Next == WriterStep \/ ReaderStep

Spec == Init /\ [][Next]_vars

\* ---------------- Properties ---------------------------------------------

TypeOK ==
    /\ bids \subseteq OrderIds
    /\ asks \subseteq OrderIds
    /\ readerBids \subseteq OrderIds
    /\ readerAsks \subseteq OrderIds
    /\ readerPC \in {"idle", "readBids", "readAsks", "done"}
    \* Mutual exclusion of sides at any instant: an order can be on at
    \* most one side at a time. (This is a writer-only property; the
    \* reader's snapshot can violate it — see SnapshotIsPointInTime.)
    /\ bids \cap asks = {}

\* Q1: every entry in a completed snapshot was on its respective side
\* at some point during the run. The reader returns a SUBSET of orders
\* that have existed; it never invents orders.
SnapshotEntriesAreReal ==
    readerPC = "done" =>
        /\ \A o \in readerBids :
              \E i \in 1..Len(history) : o \in history[i][1]
        /\ \A o \in readerAsks :
              \E i \in 1..Len(history) : o \in history[i][2]

\* Q2 (anti-property — INTENTIONALLY not invariant): the snapshot
\* is a point-in-time view, i.e. (readerBids, readerAsks) equals
\* history[i] for some i. This is the property a *locked* snapshot
\* would satisfy. Without locking, TLC will find a counterexample —
\* a state where the snapshot mixes bids from time T1 with asks from
\* a different time T2, and no single history entry matches.
\*
\* Run TLC with INVARIANT SnapshotIsPointInTime to see the violation;
\* run with INVARIANT SnapshotEntriesAreReal to verify the weaker
\* (still useful!) property holds.
SnapshotIsPointInTime ==
    readerPC = "done" =>
        \E i \in 1..Len(history) :
            history[i] = <<readerBids, readerAsks>>

\* Q3: an order shows up on at most one side of the snapshot. This
\* is a property of the *source* book (TypeOK enforces it for the
\* live state) but the snapshot can violate it under a side-flip
\* that straddles the read window.
NoDoubleSidedSnapshot ==
    readerPC = "done" =>
        readerBids \cap readerAsks = {}

================================================================================
