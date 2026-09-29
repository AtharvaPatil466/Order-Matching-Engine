#!/bin/sh
# The admin port must listen on loopback unless told otherwise.
#
# WHY. AdminServer bound INADDR_ANY, so every interface of the host served it:
# /health and /readyz unauthenticated, everything else behind a bearer token in
# cleartext HTTP, and — under --admin-no-auth, which local runs use — the full
# book, participant risk state and, with OB_CHAOS_INJECT, order injection, to
# anyone who could route to the box. Now it binds 127.0.0.1 unless --admin-bind
# / OB_ADMIN_BIND widens it. The Docker image sets OB_ADMIN_BIND=0.0.0.0: a
# published port or a k8s probe arrives on the container's own interface, never
# on its loopback.
#
# HOW. This connects the way another host would, to one of this machine's
# non-loopback addresses. Asking the socket what it bound would only restate
# the implementation. A host with no such address (no default route) has
# nothing to test, so the test skips (77).
#
# Usage: admin_binds_loopback.sh <path-to-OrderEngine>

set -u

unset OB_ADMIN_BIND OB_NODE_ROLE OB_JOURNAL_PATH OB_CONFIG_PATH OB_ADMIN_TOKEN \
      OB_ADMIN_NO_AUTH OB_CHAOS_INJECT OB_CHAOS_TOKEN

ENGINE="$1"
TMP="${TMPDIR:-/tmp}/ob_admin_bind_$$"
mkdir -p "$TMP" || exit 1
PID=""
cleanup() { [ -n "$PID" ] && kill -9 "$PID" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT INT TERM

# A UDP connect() picks the outbound interface without sending a packet.
HOST_IP=$(python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
try:
    s.connect(('192.0.2.1', 9))
    print(s.getsockname()[0])
except OSError:
    pass" 2>/dev/null)
case "$HOST_IP" in
    ''|127.*) echo "SKIP: this host has no non-loopback IPv4 address to probe from"; exit 77 ;;
esac
echo "  probing from outside loopback via $HOST_IP"

ADMIN_PORT=48210
fails=0
note_fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

# $1 host — exit 0 if GET /health answers 200 there
answers_on() {
    python3 -c "
import sys, urllib.request
try:
    sys.exit(0 if urllib.request.urlopen('http://$1:$ADMIN_PORT/health', timeout=2).status == 200 else 1)
except Exception:
    sys.exit(1)"
}

ready_on_loopback() {
    python3 -c "
import json, sys, urllib.request
try:
    r = urllib.request.urlopen('http://127.0.0.1:$ADMIN_PORT/readyz', timeout=2)
    sys.exit(0 if json.loads(r.read()).get('status') == 'ready' else 1)
except Exception:
    sys.exit(1)"
}

# $1 label, $2 closed|open|refuse, rest: "env" arguments then the engine's flags
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
        ready_on_loopback && break
        sleep 0.25; waited=$((waited + 1))
    done

    if [ "$want" = "refuse" ]; then
        if [ "$state" = "running" ]; then
            note_fail "$label: engine STARTED on an unusable bind address"
        elif [ "$state" -eq 0 ]; then
            note_fail "$label: exited 0 instead of refusing"
        elif ! grep -q "admin bind address" "$log"; then
            note_fail "$label: exited $state, but not for the bind-address reason"; cat "$log"
        else
            echo "  ok  $label — refused, and said why"
        fi
    elif [ "$state" != "running" ]; then
        note_fail "$label: exited ($state) instead of starting"; cat "$log"
    elif ! ready_on_loopback; then
        note_fail "$label: never became ready on 127.0.0.1:$ADMIN_PORT"; cat "$log"
    elif [ "$want" = "closed" ] && answers_on "$HOST_IP"; then
        note_fail "$label: the admin port answered on $HOST_IP — it is listening on every interface"
    elif [ "$want" = "open" ] && ! answers_on "$HOST_IP"; then
        note_fail "$label: the admin port did not answer on $HOST_IP — the override did nothing"
    else
        echo "  ok  $label — $want on $HOST_IP, serving on loopback"
    fi

    [ "$state" = "running" ] && { kill -9 "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; }
    PID=""
}

run_case "default"                     closed "$ENGINE"
run_case "OB_ADMIN_BIND=0.0.0.0"       open   OB_ADMIN_BIND=0.0.0.0 "$ENGINE"
run_case "--admin-bind not-an-address" refuse "$ENGINE" --admin-bind not-an-address

if [ "$fails" -ne 0 ]; then echo "FAIL: $fails case(s) failed"; exit 1; fi
echo "PASS: the admin port listens on loopback unless --admin-bind widens it"
exit 0
