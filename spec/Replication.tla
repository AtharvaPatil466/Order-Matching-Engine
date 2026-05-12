---- MODULE Replication ----
(***************************************************************************)
(* TLA+ Specification for the Primary-Backup Replication Protocol          *)
(*                                                                         *)
(* Roadmap Phase 2, Week 6: Replication Protocol TLA+ Specification        *)
(*                                                                         *)
(* Models two nodes: Primary and Backup                                    *)
(* Variables: primary_log, backup_log, epochs, heartbeat, partition state  *)
(*                                                                         *)
(* Invariants:                                                             *)
(*   NoCommittedLoss: Acknowledged trades survive failover                 *)
(*   NoDuplicateExecution: No trade appears twice in committed log         *)
(*   NoSplitBrain: At most one node believes it is primary                 *)
(***************************************************************************)

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    MaxEntries,     \* Maximum log entries
    HeartbeatTimeout \* Ticks before declaring peer dead

VARIABLES
    primaryLog,      \* Sequence of committed entries on primary
    backupLog,       \* Sequence of replicated entries on backup
    primaryEpoch,    \* Primary's current epoch
    backupEpoch,     \* Backup's last seen epoch
    primaryRole,     \* TRUE if node 1 thinks it's primary
    backupRole,      \* TRUE if node 2 thinks it's primary (split-brain check)
    heartbeatTimer,  \* Ticks since last heartbeat received by backup
    networkPartition,\* TRUE if network is partitioned
    acked,           \* Set of entry IDs acknowledged to clients
    primaryAlive     \* TRUE if primary process is alive

vars == <<primaryLog, backupLog, primaryEpoch, backupEpoch,
          primaryRole, backupRole, heartbeatTimer,
          networkPartition, acked, primaryAlive>>

(* ─── Init ────────────────────────────────────────────────────────── *)

Init ==
    /\ primaryLog = <<>>
    /\ backupLog = <<>>
    /\ primaryEpoch = 1
    /\ backupEpoch = 0
    /\ primaryRole = TRUE
    /\ backupRole = FALSE
    /\ heartbeatTimer = 0
    /\ networkPartition = FALSE
    /\ acked = {}
    /\ primaryAlive = TRUE

(* ─── PrimaryAppend ───────────────────────────────────────────────── *)
\* Primary appends a new entry to its log

PrimaryAppend ==
    /\ primaryAlive
    /\ primaryRole
    /\ Len(primaryLog) < MaxEntries
    /\ LET entry == Len(primaryLog) + 1
       IN /\ primaryLog' = Append(primaryLog, entry)
          /\ acked' = acked \cup {entry}
          /\ UNCHANGED <<backupLog, primaryEpoch, backupEpoch,
                         primaryRole, backupRole, heartbeatTimer,
                         networkPartition, primaryAlive>>

(* ─── ShipLog ─────────────────────────────────────────────────────── *)
\* Primary ships log entries to backup (if network is up)

ShipLog ==
    /\ primaryAlive
    /\ primaryRole
    /\ ~networkPartition
    /\ Len(backupLog) < Len(primaryLog)
    /\ LET nextIdx == Len(backupLog) + 1
           entry == primaryLog[nextIdx]
       IN /\ backupLog' = Append(backupLog, entry)
          /\ backupEpoch' = primaryEpoch
          /\ UNCHANGED <<primaryLog, primaryEpoch, primaryRole,
                         backupRole, heartbeatTimer, networkPartition,
                         acked, primaryAlive>>

(* ─── Heartbeat ───────────────────────────────────────────────────── *)
\* Primary sends heartbeat (resets backup's timer)

Heartbeat ==
    /\ primaryAlive
    /\ primaryRole
    /\ ~networkPartition
    /\ heartbeatTimer' = 0
    /\ UNCHANGED <<primaryLog, backupLog, primaryEpoch, backupEpoch,
                   primaryRole, backupRole, networkPartition,
                   acked, primaryAlive>>

(* ─── HeartbeatTick ───────────────────────────────────────────────── *)
\* Time passes without heartbeat

HeartbeatTick ==
    /\ heartbeatTimer < HeartbeatTimeout + 1
    /\ heartbeatTimer' = heartbeatTimer + 1
    /\ UNCHANGED <<primaryLog, backupLog, primaryEpoch, backupEpoch,
                   primaryRole, backupRole, networkPartition,
                   acked, primaryAlive>>

(* ─── PrimaryCrash ────────────────────────────────────────────────── *)
\* Primary crashes (unclean failure)

PrimaryCrash ==
    /\ primaryAlive
    /\ primaryAlive' = FALSE
    /\ primaryRole' = FALSE  \* Dead nodes are not primary
    /\ UNCHANGED <<primaryLog, backupLog, primaryEpoch, backupEpoch,
                   backupRole, heartbeatTimer, networkPartition, acked>>

(* ─── BackupPromote ───────────────────────────────────────────────── *)
\* Backup promotes itself to primary after heartbeat timeout

BackupPromote ==
    /\ ~backupRole                    \* Not already promoted
    /\ ~primaryAlive                  \* Primary must actually be down
                                      \* (in reality, detected via timeout)
    /\ heartbeatTimer > HeartbeatTimeout
    /\ backupEpoch < primaryEpoch + 1 \* Fencing: must use higher epoch
    /\ backupRole' = TRUE
    /\ backupEpoch' = primaryEpoch + 1
    /\ UNCHANGED <<primaryLog, backupLog, primaryEpoch, primaryRole,
                   heartbeatTimer, networkPartition, acked, primaryAlive>>

(* ─── NetworkPartition ────────────────────────────────────────────── *)

NetworkPartition ==
    /\ ~networkPartition
    /\ networkPartition' = TRUE
    /\ UNCHANGED <<primaryLog, backupLog, primaryEpoch, backupEpoch,
                   primaryRole, backupRole, heartbeatTimer,
                   acked, primaryAlive>>

NetworkHeal ==
    /\ networkPartition
    /\ networkPartition' = FALSE
    /\ UNCHANGED <<primaryLog, backupLog, primaryEpoch, backupEpoch,
                   primaryRole, backupRole, heartbeatTimer,
                   acked, primaryAlive>>

(* ─── Next ────────────────────────────────────────────────────────── *)

Next ==
    \/ PrimaryAppend
    \/ ShipLog
    \/ Heartbeat
    \/ HeartbeatTick
    \/ PrimaryCrash
    \/ BackupPromote
    \/ NetworkPartition
    \/ NetworkHeal

Spec == Init /\ [][Next]_vars

(* ─── Invariants ──────────────────────────────────────────────────── *)

\* INV1: No committed (acked) trade is lost after failover.
\* If primary acked an entry AND primary crashed AND backup promoted,
\* then the entry must be in backupLog.
NoCommittedLoss ==
    \A entry \in acked :
        (backupRole /\ ~primaryAlive)
        => \E i \in 1..Len(backupLog) : backupLog[i] = entry

\* INV2: No duplicate entries in the committed log.
\* Whichever log is "active" must have unique entries.
NoDuplicateExecution ==
    /\ \A i, j \in 1..Len(primaryLog) :
        i /= j => primaryLog[i] /= primaryLog[j]
    /\ \A i, j \in 1..Len(backupLog) :
        i /= j => backupLog[i] /= backupLog[j]

\* INV3: At most one node believes it is primary at any time.
NoSplitBrain ==
    ~(primaryRole /\ backupRole)

====
