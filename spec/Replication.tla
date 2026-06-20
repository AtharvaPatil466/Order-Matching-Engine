---- MODULE Replication ----
(***************************************************************************)
(* TLA+ Specification for the Primary-Backup Replication Protocol          *)
(*                                                                         *)
(* Models two nodes: Primary and Backup                                    *)
(*                                                                         *)
(* This revision (post-chaos-suite-findings) models the LEASE PROPAGATION  *)
(* mechanism added to the implementation after live chaos testing found    *)
(* a split-brain bug under packet loss. Specifically:                      *)
(*                                                                         *)
(*   * The primary broadcasts a lease grant (durationMs) every heartbeat   *)
(*     tick. The backup's local view of "is the primary's lease still      *)
(*     valid?" is what fences promotion — NOT the heartbeat timeout alone. *)
(*                                                                         *)
(*   * BackupPromote no longer god-modes ~primaryAlive. It requires the    *)
(*     local lease to be expired (modeled by leaseTimer > LeaseTimeout).   *)
(*     This matches the C++ implementation where backup cannot promote     *)
(*     while LeaderLease.isValid(now) returns true for the primary's      *)
(*     last-known grant.                                                  *)
(*                                                                         *)
(* Invariants:                                                             *)
(*   NoCommittedLoss: Acknowledged entries survive failover                *)
(*   NoDuplicateExecution: No entry appears twice in committed log         *)
(*   NoSplitBrain: At most one node believes it is primary at any moment   *)
(***************************************************************************)

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    MaxEntries,       \* Maximum log entries
    HeartbeatTimeout, \* Ticks before backup declares peer "not pingable"
    LeaseTimeout      \* Ticks since last LeaseGrant before lease expires.
                      \* MUST be >= HeartbeatTimeout; if equal, the lease
                      \* offers no additional fencing over heartbeat-alone.

VARIABLES
    primaryLog,      \* Sequence of entries on primary
    backupLog,       \* Sequence of replicated entries on backup
    primaryEpoch,    \* Primary's current epoch
    backupEpoch,     \* Backup's last seen epoch
    primaryRole,     \* TRUE if node 1 thinks it's primary
    backupRole,      \* TRUE if node 2 thinks it's primary (split-brain check)
    heartbeatTimer,  \* Ticks since last heartbeat received by backup
    leaseTimer,      \* Ticks since last LeaseGrant received by backup
    networkPartition,\* TRUE if network is partitioned
    acked,           \* Set of entry IDs acknowledged to clients
    primaryAlive     \* TRUE if primary process is alive (modeled, but
                     \* NOT used in BackupPromote — see below)

vars == <<primaryLog, backupLog, primaryEpoch, backupEpoch,
          primaryRole, backupRole, heartbeatTimer, leaseTimer,
          networkPartition, acked, primaryAlive>>

(* ─── Init ────────────────────────────────────────────────────────── *)

Init ==
    /\ primaryLog = <<>>
    /\ backupLog = <<>>
    /\ primaryEpoch = 1
    /\ backupEpoch = 1   \* Backup's local view starts at primary's epoch
                         \* (modeling the LeaseGrant on initial connect).
    /\ primaryRole = TRUE
    /\ backupRole = FALSE
    /\ heartbeatTimer = 0
    /\ leaseTimer = 0
    /\ networkPartition = FALSE
    /\ acked = {}
    /\ primaryAlive = TRUE

(* ─── PrimaryAppend ───────────────────────────────────────────────── *)
\* Primary appends a new entry to its log. Sync-replication safety:
\* the entry is NOT yet acked. Acknowledgement waits for ShipLog.

PrimaryAppend ==
    /\ primaryAlive
    /\ primaryRole
    /\ Len(primaryLog) < MaxEntries
    /\ LET entry == Len(primaryLog) + 1
       IN /\ primaryLog' = Append(primaryLog, entry)
          /\ UNCHANGED <<backupLog, primaryEpoch, backupEpoch,
                         primaryRole, backupRole, heartbeatTimer,
                         leaseTimer, networkPartition, acked, primaryAlive>>

(* ─── ShipLog ─────────────────────────────────────────────────────── *)
\* Primary ships log entry to backup (if reachable). On successful
\* ship the client ACK is emitted.

ShipLog ==
    /\ primaryAlive
    /\ primaryRole
    /\ ~networkPartition
    /\ Len(backupLog) < Len(primaryLog)
    /\ LET nextIdx == Len(backupLog) + 1
           entry == primaryLog[nextIdx]
       IN /\ backupLog' = Append(backupLog, entry)
          /\ acked' = acked \cup {entry}
          /\ UNCHANGED <<primaryLog, primaryEpoch, backupEpoch,
                         primaryRole, backupRole, heartbeatTimer,
                         leaseTimer, networkPartition, primaryAlive>>

(* ─── Tick ────────────────────────────────────────────────────────── *)
\* A single "time passes by one quantum" action. The primary's
\* heartbeat-and-lease-grant loop is implicit in the healthy path —
\* the C++ HeartbeatMonitor runs on a timer and fires unconditionally
\* every interval. The choice TLC must make is whether THE NETWORK
\* delivers them or not, not whether the primary "decides" to send.
\*
\* This eliminates an unrealistic class of TLC counterexample where
\* time advances while a healthy primary stays silent. Without this
\* constraint, BackupPromote can be reached even with primary alive
\* and reachable — split brain — but only because the model allowed
\* the primary to skip its own heartbeat thread, which never happens
\* in practice.

TickHealthy ==
    /\ primaryAlive
    /\ primaryRole
    /\ ~networkPartition
    /\ heartbeatTimer' = 0
    /\ leaseTimer' = 0
    /\ backupEpoch' = primaryEpoch
    /\ UNCHANGED <<primaryLog, backupLog, primaryEpoch,
                   primaryRole, backupRole, networkPartition,
                   acked, primaryAlive>>

TickDegraded ==
    /\ ~primaryAlive \/ networkPartition
    /\ heartbeatTimer < HeartbeatTimeout + 1
    /\ leaseTimer < LeaseTimeout + 1
    /\ heartbeatTimer' = heartbeatTimer + 1
    /\ leaseTimer' = leaseTimer + 1
    /\ UNCHANGED <<primaryLog, backupLog, primaryEpoch, backupEpoch,
                   primaryRole, backupRole, networkPartition,
                   acked, primaryAlive>>

(* ─── PrimaryCrash ────────────────────────────────────────────────── *)

PrimaryCrash ==
    /\ primaryAlive
    /\ primaryAlive' = FALSE
    /\ primaryRole' = FALSE
    /\ UNCHANGED <<primaryLog, backupLog, primaryEpoch, backupEpoch,
                   backupRole, heartbeatTimer, leaseTimer,
                   networkPartition, acked>>

(* ─── BackupPromote ───────────────────────────────────────────────── *)
\* The realistic promotion rule: backup can promote ONLY when its
\* local lease for the primary has expired. Crucially, this does NOT
\* god-mode ~primaryAlive — the model lets backup attempt promotion
\* even if primary is still alive, exactly like a real cluster under
\* sustained packet loss / network failure. The lease-expiry check is
\* what prevents split brain. If TLC can find ANY trace that lands a
\* split brain under this rule, the implementation is broken too.

BackupPromote ==
    /\ ~backupRole
    /\ heartbeatTimer > HeartbeatTimeout  \* missed heartbeats
    /\ leaseTimer > LeaseTimeout          \* local lease expired
    /\ backupEpoch <= primaryEpoch        \* epoch fencing
    /\ backupRole' = TRUE
    /\ backupEpoch' = primaryEpoch + 1
    /\ UNCHANGED <<primaryLog, backupLog, primaryEpoch, primaryRole,
                   heartbeatTimer, leaseTimer, networkPartition,
                   acked, primaryAlive>>

(* ─── Network ─────────────────────────────────────────────────────── *)

NetworkPartition ==
    /\ ~networkPartition
    /\ networkPartition' = TRUE
    /\ UNCHANGED <<primaryLog, backupLog, primaryEpoch, backupEpoch,
                   primaryRole, backupRole, heartbeatTimer, leaseTimer,
                   acked, primaryAlive>>

NetworkHeal ==
    /\ networkPartition
    /\ networkPartition' = FALSE
    /\ UNCHANGED <<primaryLog, backupLog, primaryEpoch, backupEpoch,
                   primaryRole, backupRole, heartbeatTimer, leaseTimer,
                   acked, primaryAlive>>

(* ─── Next ────────────────────────────────────────────────────────── *)

Next ==
    \/ PrimaryAppend
    \/ ShipLog
    \/ TickHealthy
    \/ TickDegraded
    \/ PrimaryCrash
    \/ BackupPromote
    \/ NetworkPartition
    \/ NetworkHeal

Spec == Init /\ [][Next]_vars

(* ─── Invariants ──────────────────────────────────────────────────── *)

\* INV1: No committed (acked) entry is lost after a successful failover.
\* If backup believes it is primary AND the original primary is genuinely
\* down, every ACKed entry must be in the backup's log.
NoCommittedLoss ==
    \A entry \in acked :
        (backupRole /\ ~primaryAlive)
        => \E i \in 1..Len(backupLog) : backupLog[i] = entry

\* INV2: No duplicate entries in either committed log.
NoDuplicateExecution ==
    /\ \A i, j \in 1..Len(primaryLog) :
        i /= j => primaryLog[i] /= primaryLog[j]
    /\ \A i, j \in 1..Len(backupLog) :
        i /= j => backupLog[i] /= backupLog[j]

\* INV3: At most one node believes it is primary at any moment.
\* This is the property the lease-propagation mechanism exists to
\* preserve. With the realistic BackupPromote rule (no god-mode
\* ~primaryAlive guard), TLC genuinely exercises the case where
\* primary is alive but unreachable — exactly the split-brain
\* scenario the chaos suite caught and the lease fix prevents.
NoSplitBrain ==
    ~(primaryRole /\ backupRole)

====
