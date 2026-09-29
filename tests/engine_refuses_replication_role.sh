#!/bin/sh
# Replication must not switch on just because a role is set.
#
# WHY. OrderEngine started a ReplicationCoordinator whenever --role /
# OB_NODE_ROLE was set, and docker-compose.yml set it on both of its engines —
# so the default deployment ran primary/backup replication. That path has known
# CRITICAL defects (REPL-1..6), among them:
#   REPL-1  nothing fences a deposed primary; the lease only delays promotion
#   REPL-2  a promoted backup keeps applying the old primary's stream into its
#           now-live books, so orders execute twice
#   REPL-3  a replication send blocks the journal commit path with no timeout,
#           so a slow or paused backup freezes client acks indefinitely
#   REPL-4  entries lost across a disconnect are never re-sent, so the backup
#           silently diverges and a failover trades against orders that are gone
#
# So a role now refuses to boot unless it is explicitly acknowledged with
# --allow-unsafe-replication (OB_ALLOW_UNSAFE_REPLICATION=1) — the same shape as
# --no-participant-auth and --replay-legacy-journal. The chaos suite
# (deploy/chaos), which exists to exercise this path, opts in; nothing else does.
#
# Each refusing case runs under a deadline: at HEAD the engine STARTS with a
# role and serves, and a serving engine never exits, so an exit-code check alone
# could not see the failure this test is for.
#
# Usage: engine_refuses_replication_role.sh <path-to-OrderEngine>

set -u

unset OB_NODE_ROLE OB_ALLOW_UNSAFE_REPLICATION OB_JOURNAL_PATH OB_CONFIG_PATH \
      OB_ADMIN_TOKEN OB_ADMIN_NO_AUTH OB_PRIMARY_HOST OB_PRIMARY_REPLICATION_PORT \
      OB_REPLICATION_PORT OB_NODE_ID OB_CHAOS_INJECT OB_CHAOS_TOKEN

ENGINE="$1"
TMP="${TMPDIR:-/tmp}/ob_repl_role_$$"
mkdir -p "$TMP" || exit 1
PID=""
cleanup() { [ -n "$PID" ] && kill -9 "$PID" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT INT TERM

ADMIN_PORT=48170
fails=0
note_fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

# $1 label, $2 refuse|start, rest: "env" arguments then the engine's own flags
run_case() {
    label="$1"; want="$2"; shift 2
    log="$TMP/case.log"
    env "$@" --admin-no-auth --symbols 1 --port "$ADMIN_PORT" > "$log" 2>&1 &
    PID=$!
    state="running"
    waited=0
    while [ "$waited" -lt 120 ]; do            # 120 x 0.25s = 30s
        if ! kill -0 "$PID" 2>/dev/null; then
            wait "$PID" 2>/dev/null; state=$?; break
        fi
        if python3 -c "
import json,sys,urllib.request
try: sys.exit(0 if json.loads(urllib.request.urlopen('http://127.0.0.1:$ADMIN_PORT/readyz',timeout=2).read()).get('status')=='ready' else 1)
except Exception: sys.exit(1)" 2>/dev/null; then
            break
        fi
        sleep 0.25; waited=$((waited + 1))
    done
    if [ "$state" = "running" ]; then kill -9 "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; fi
    PID=""

    if [ "$want" = "refuse" ]; then
        if [ "$state" = "running" ]; then
            note_fail "$label: engine STARTED — a role switched replication on"
            grep -E "Replication|Ready" "$log" | head -3; return
        fi
        if [ "$state" -eq 0 ]; then note_fail "$label: exited 0 instead of refusing"; cat "$log"; return; fi
        if ! grep -q "replication is disabled" "$log"; then
            note_fail "$label: exited $state, but not for the replication reason"; cat "$log"; return
        fi
        echo "  ok  $label — refused, and said why"
    else
        if [ "$state" != "running" ]; then
            note_fail "$label: exited ($state) instead of starting"; cat "$log"; return
        fi
        echo "  ok  $label — started"
    fi
}

run_case "--role primary"                 refuse "$ENGINE" --role primary
run_case "OB_NODE_ROLE=backup"            refuse OB_NODE_ROLE=backup OB_PRIMARY_HOST=127.0.0.1 "$ENGINE"
run_case "role + explicit opt-in"         start  OB_NODE_ROLE=primary OB_REPLICATION_PORT=48190 \
                                                 "$ENGINE" --allow-unsafe-replication

if [ "$fails" -ne 0 ]; then echo "FAIL: $fails case(s) failed"; exit 1; fi
echo "PASS: a role refuses to boot unless replication is explicitly acknowledged"
exit 0
