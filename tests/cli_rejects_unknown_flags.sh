#!/bin/sh
# Both binaries must refuse an argument they do not understand.
#
# WHAT WAS WRONG (DOC-3). OrderEngine and GatewayServer looked each flag up by
# name and skipped everything else, so a misspelt or retired flag was dropped
# without a word and the process started without it. The Runbook told operators
# to pass `--lean` and `--replay-only`, neither of which exists: `--replay-only`
# was meant to inspect a journal and exit, and instead started a live engine on
# it. An ignored flag looks exactly like one that took effect.
#
# A refusal is checked with a deadline, because the failure being pinned is
# "it started anyway", and a started binary never exits.
#
# Usage: cli_rejects_unknown_flags.sh <OrderEngine> <GatewayServer>

set -u
unset OB_ADMIN_TOKEN OB_ADMIN_NO_AUTH OB_ADMIN_BIND OB_JOURNAL_PATH OB_CONFIG_PATH \
      OB_NODE_ROLE OB_PARTICIPANT_CREDENTIALS OB_NO_PARTICIPANT_AUTH \
      OB_ENGINE_HOST OB_ENGINE_PORT OB_SESSION_SCHEDULE

ENGINE="$1"
GATEWAY="$2"
TMP="${TMPDIR:-/tmp}/ob_cli_flags_$$"
mkdir -p "$TMP" || exit 1
trap 'rm -rf "$TMP"' EXIT INT TERM
LOG="$TMP/run.log"

fails=0
fail() { echo "FAIL: $*"; cat "$LOG"; fails=$((fails + 1)); }

# Runs "$@" for up to 8 s. Sets $rc to the exit status, or "running".
run() {
    "$@" > "$LOG" 2>&1 &
    pid=$!
    n=0
    while [ "$n" -lt 40 ] && kill -0 "$pid" 2>/dev/null; do
        sleep 0.2
        n=$((n + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
        kill -9 "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
        rc=running
    else
        wait "$pid"
        rc=$?
    fi
}

# $1 label, $2 expected message, $3.. command
must_refuse() {
    label="$1"; expect="$2"; shift 2
    run "$@"
    if [ "$rc" = running ]; then fail "$label: still running at the deadline"; return; fi
    if [ "$rc" -eq 0 ]; then fail "$label: exited 0"; return; fi
    if ! grep -qF -- "$expect" "$LOG"; then fail "$label: exited $rc without \"$expect\""; return; fi
    echo "  ok  $label"
}

# $1 label, $2 readiness probe (a command), $3.. command under test
must_start() {
    label="$1"; probe="$2"; shift 2
    "$@" > "$LOG" 2>&1 &
    pid=$!
    n=0
    ok=no
    while [ "$n" -lt 40 ] && kill -0 "$pid" 2>/dev/null; do
        if $probe; then ok=yes; break; fi
        sleep 0.2
        n=$((n + 1))
    done
    if [ "$ok" = yes ]; then echo "  ok  $label"; else fail "$label: never became ready"; fi
    kill -9 "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
}

# OrderEngine's stdout is block-buffered into a file, so its readiness is read
# from /readyz, not the log. python3, not curl: CI's Ubuntu image has no curl.
engine_ready() {
    python3 -c "import urllib.request as u; assert b'ready' in u.urlopen('http://127.0.0.1:49332/readyz', timeout=1).read()" 2>/dev/null
}
gateway_ready() { grep -qF "Order Gateway listening" "$LOG"; }

must_refuse "OrderEngine --lean" "unknown argument '--lean'" \
    "$ENGINE" --admin-no-auth --port 49330 --lean
must_refuse "OrderEngine --replay-only" "unknown argument '--replay-only'" \
    "$ENGINE" --admin-no-auth --port 49330 --replay-only
must_refuse "OrderEngine stray positional" "unknown argument '8080'" \
    "$ENGINE" --admin-no-auth 8080
must_refuse "OrderEngine --journal with no value" "--journal needs a value" \
    "$ENGINE" --admin-no-auth --port 49330 --journal
must_refuse "GatewayServer --bogus" "unknown argument '--bogus'" \
    "$GATEWAY" 49331 --no-participant-auth --bogus
must_refuse "GatewayServer PORT after a flag" "unknown argument '49331'" \
    "$GATEWAY" --no-participant-auth 49331

# A check that refuses everything is not a fix: every documented flag still works.
must_start "OrderEngine, known flags" engine_ready \
    "$ENGINE" --threads 1 --symbols 1 --port 49332 --admin-bind 127.0.0.1 --admin-no-auth
must_start "GatewayServer, known flags" gateway_ready \
    "$GATEWAY" 49333 --no-participant-auth
run "$ENGINE" --help
[ "$rc" = 0 ] && grep -qF -- "--journal PATH" "$LOG" && echo "  ok  OrderEngine --help" \
    || fail "OrderEngine --help: rc=$rc, or --journal missing from it"
run "$GATEWAY" --help
[ "$rc" = 0 ] && echo "  ok  GatewayServer --help" || fail "GatewayServer --help: rc=$rc"

if [ "$fails" -ne 0 ]; then
    echo "FAIL: $fails case(s)"
    exit 1
fi
echo "PASS: unknown arguments are fatal in both binaries; known flags still start them"
