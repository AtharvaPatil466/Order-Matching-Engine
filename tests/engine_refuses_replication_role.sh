#!/bin/sh
# A replication role refuses to boot: replication was removed (roadmap 1.1 R1).
#
# WHY. OrderEngine started replication whenever --role /
# OB_NODE_ROLE was set, and docker-compose.yml once set it on both of its
# engines. That path had no fencing, a promoted backup kept applying the old
# primary's stream, its send blocked the journal commit path and resync only
# added orders (REPL-1..9), so it was deleted rather than finished. A deployment
# that still sets a role must hear that at boot, not run as a single node it did
# not ask for. The old opt-in, OB_ALLOW_UNSAFE_REPLICATION, opens nothing now.
#
# Each refusing case runs under a deadline: a serving engine never exits, so an
# exit-code check alone could not see an engine that ignored the role.
#
# Usage: engine_refuses_replication_role.sh <path-to-OrderEngine>

set -u

unset OB_NODE_ROLE OB_ALLOW_UNSAFE_REPLICATION OB_JOURNAL_PATH OB_CONFIG_PATH \
      OB_ADMIN_TOKEN OB_ADMIN_NO_AUTH OB_CHAOS_INJECT OB_CHAOS_TOKEN
# The order port is not what this test is about: no credentials, any free port.
export OB_NO_PARTICIPANT_AUTH=1 OB_GATEWAY_PORT=0

ENGINE="$1"
TMP="${TMPDIR:-/tmp}/ob_repl_role_$$"
mkdir -p "$TMP" || exit 1
PID=""
cleanup() { [ -n "$PID" ] && kill -9 "$PID" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT INT TERM

ADMIN_PORT=31640
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
            note_fail "$label: engine STARTED with a role set"
            grep -E "Ready" "$log" | head -3; return
        fi
        if [ "$state" -eq 0 ]; then note_fail "$label: exited 0 instead of refusing"; cat "$log"; return; fi
        if ! grep -q "replication was removed" "$log"; then
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
run_case "OB_NODE_ROLE=backup"            refuse OB_NODE_ROLE=backup "$ENGINE"
run_case "role + the old opt-in"          refuse OB_NODE_ROLE=primary OB_ALLOW_UNSAFE_REPLICATION=1 \
                                                 "$ENGINE"
run_case "no role"                        start  "$ENGINE"

if [ "$fails" -ne 0 ]; then echo "FAIL: $fails case(s) failed"; exit 1; fi
echo "PASS: a replication role refuses to boot; without one the engine starts"
exit 0
