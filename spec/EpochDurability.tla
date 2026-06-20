-------------------------- MODULE EpochDurability --------------------------
\* Durable leader-lease epoch.
\*
\* The replication layer fences a stale primary with a monotonic epoch
\* ("higher epoch wins"). If the epoch lives only in memory, a crashed node
\* resets it to 0 on restart and can REGRESS below an epoch it already
\* committed — letting it act as leader at a stale epoch (split brain). The
\* fix persists the epoch (EpochStore) and reloads it on restart.
\*
\* This spec models persist-on-bump + reload-on-restart and proves the working
\* epoch never regresses below the highest committed epoch while the node is
\* operational. The bug-injected variant (BROKEN_NO_PERSIST = TRUE) resets the
\* epoch to 0 on restart; TLC then finds the regression — so the property is
\* not vacuous.

EXTENDS Integers, TLC

CONSTANTS MaxEpoch, BROKEN_NO_PERSIST

VARIABLES epoch,    \* in-memory fencing epoch (lost on crash)
          durable,  \* persisted epoch (survives a crash)
          maxSeen,  \* history var: highest epoch ever committed by this node
          crashed   \* TRUE while down, between Crash and Restart

vars == <<epoch, durable, maxSeen, crashed>>

TypeOK ==
    /\ epoch   \in 0..MaxEpoch
    /\ durable \in 0..MaxEpoch
    /\ maxSeen \in 0..MaxEpoch
    /\ crashed \in BOOLEAN

Init ==
    /\ epoch = 0
    /\ durable = 0
    /\ maxSeen = 0
    /\ crashed = FALSE

\* Acquire / renew leadership: bump the epoch and persist it durably BEFORE it
\* is acted upon (durable' tracks the new value).
Bump ==
    /\ ~crashed
    /\ epoch < MaxEpoch
    /\ epoch'   = epoch + 1
    /\ durable' = epoch + 1
    /\ maxSeen' = epoch + 1
    /\ crashed' = crashed

\* Process crash: the in-memory epoch is lost; the disk value survives.
Crash ==
    /\ ~crashed
    /\ crashed' = TRUE
    /\ epoch'   = 0
    /\ durable' = durable
    /\ maxSeen' = maxSeen

\* Restart: reload the epoch from disk (correct) or reset to 0 (the bug).
Restart ==
    /\ crashed
    /\ crashed' = FALSE
    /\ epoch'   = IF BROKEN_NO_PERSIST THEN 0 ELSE durable
    /\ durable' = durable
    /\ maxSeen' = maxSeen

Next == Bump \/ Crash \/ Restart \/ UNCHANGED vars

Spec == Init /\ [][Next]_vars

\* Safety: while operational (not crashed), the working epoch never sits below
\* an epoch the node already committed — so "higher epoch wins" fencing stays
\* sound across restarts. The in-memory-only bug violates this.
NoRegression == crashed \/ (epoch >= maxSeen)

\* Sanity: the persisted epoch always equals the highest committed epoch.
DurableTracksMax == durable = maxSeen
=============================================================================
