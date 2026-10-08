#!/bin/sh
# The session timetable can come from the config file (roadmap 1.8-H8).
#
# WHY. The only way to switch on OrderEngine's SessionScheduler was the
# --session-schedule flag or OB_SESSION_SCHEDULE. A `session_schedule` key in
# the --config file was read by nothing, so a deployment that kept its venue
# hours in engine.conf ran with no session at all: no open, no close, no DAY
# sweep, and no word about it.
#
# Two cases against the real binary, each under a deadline:
#   * a malformed key refuses to boot and says why, as the flag does
#   * a valid key turns the scheduler on
# At HEAD the key is ignored: the engine starts in both cases, without the
# scheduler.
#
# Usage: engine_session_schedule_config.sh <path-to-OrderEngine>

set -u

unset OB_SESSION_SCHEDULE OB_JOURNAL_PATH OB_CONFIG_PATH OB_NODE_ROLE \
      OB_ADMIN_TOKEN OB_ADMIN_NO_AUTH OB_CHAOS_INJECT OB_CHAOS_TOKEN

ENGINE="$1"
TMP="${TMPDIR:-/tmp}/ob_session_cfg_$$"
mkdir -p "$TMP" || exit 1
PID=""
cleanup() { [ -n "$PID" ] && kill -9 "$PID" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT INT TERM

ADMIN_PORT=49431
fails=0
note_fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

# $1 label, $2 refuse|start, $3 the session_schedule value
run_case() {
    label="$1"; want="$2"
    conf="$TMP/engine.conf"
    log="$TMP/case.log"
    printf 'log_sink = null\nsession_schedule = %s\n' "$3" > "$conf"
    "$ENGINE" --config "$conf" --admin-no-auth --symbols 1 --port "$ADMIN_PORT" > "$log" 2>&1 &
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
    if [ "$state" = "running" ]; then
        # A clean stop flushes stdout, which is a file here and fully buffered.
        kill -TERM "$PID" 2>/dev/null
        i=0
        while kill -0 "$PID" 2>/dev/null && [ "$i" -lt 120 ]; do sleep 0.25; i=$((i + 1)); done
        kill -9 "$PID" 2>/dev/null; wait "$PID" 2>/dev/null
    fi
    PID=""

    if [ "$want" = "refuse" ]; then
        if [ "$state" = "running" ]; then
            note_fail "$label: engine STARTED — the session_schedule key was ignored"; return
        fi
        if ! grep -q "session_schedule\|session-schedule" "$log"; then
            note_fail "$label: exited $state, but not for the schedule"; cat "$log"; return
        fi
        echo "  ok  $label — refused, and said why"
    else
        if ! grep -q "\[Session\] Scheduler on: 13:00,13:30,19:55,20:00" "$log"; then
            note_fail "$label: the scheduler did not start from the config key"; cat "$log"; return
        fi
        echo "  ok  $label — scheduler on"
    fi
}

run_case "malformed session_schedule"  refuse "25:00,13:30,19:55,20:00"
run_case "valid session_schedule"      start  "13:00,13:30,19:55,20:00"

if [ "$fails" -ne 0 ]; then echo "FAIL: $fails case(s) failed"; exit 1; fi
echo "PASS: session_schedule in the config file drives the scheduler"
exit 0
