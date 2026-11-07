#!/bin/sh
# The order-entry port must refuse to serve unauthenticated — including when a
# credentials file WAS configured and turned out to hold nothing.
#
# WHAT WAS WRONG. Only a negative return from ParticipantAuth::loadFromFile was
# fatal. A file of nothing but comments and blank lines parses cleanly and returns
# 0, which passed. enabled() is then false, so
#
#     gateway.setParticipantAuth(participantAuth.enabled() ? &participantAuth : nullptr)
#
# passed nullptr, which the gateway reads as "take each claimed identity on trust".
# The operator saw "Loaded 0 participant credential(s)", the port came up, and any
# client could claim to be any participant. A fail-open on a control that was
# configured is worse than one that was forgotten, because nothing in the log looks
# wrong.
#
# Every case below asserts WHY the process exited, not just that it did: an earlier
# WILL_FAIL test stayed green while the process exited on an argument error before
# it ever reached this check. Same reason refuses_unreadable_journal.sh greps for
# its message.
#
# A REFUSAL IS ALSO A DEADLINE. The failure mode being pinned is "the engine starts
# serving", and a serving engine never exits. So a case that must refuse is run with
# a deadline rather than read through command substitution, which would hang the
# suite instead of reporting the defect. Asserting on the exit code alone cannot see
# this failure at all.
#
# Usage: gateway_requires_credentials.sh <path-to-OrderEngine>

set -u

# Unset rather than blank: a developer who exports these in their shell must not
# silently pass this, and CMake's ENVIRONMENT property can only SET a variable,
# so clearing one there gives it an empty value instead of an absent one.
unset OB_PARTICIPANT_CREDENTIALS OB_NO_PARTICIPANT_AUTH OB_GATEWAY_PORT OB_GATEWAY_BIND \
      OB_ADMIN_TOKEN OB_ADMIN_NO_AUTH OB_ADMIN_BIND OB_JOURNAL_PATH OB_CONFIG_PATH OB_NODE_ROLE

ENGINE="$1"
# Any free admin and order port, so a refusal is the check talking and not a bind.
ARGS="--admin-no-auth --port 0 --threads 1 --symbols 1"
TMP="${TMPDIR:-/tmp}/ob_gw_creds_$$"
mkdir -p "$TMP" || exit 1
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT INT TERM

fails=0
note_fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

# ── Credential files. Synthetic secrets; nothing here is a real credential. ──
: > "$TMP/empty.conf"
printf '# participant credentials\n#\n\n   \n\t\n# firm-a:REPLACE_ME:100\n' > "$TMP/comments.conf"
printf 'test-firm:not-a-real-secret:100,101\n' > "$TMP/valid.conf"
chmod 600 "$TMP/empty.conf" "$TMP/comments.conf" "$TMP/valid.conf"

# Run the engine with a deadline. Sets $rc to its exit status, or to the string
# "running" if it was still alive at the deadline — which is what "it agreed to
# serve" looks like, since a serving engine blocks forever.
run_with_deadline() {
    log="$1"; shift
    # shellcheck disable=SC2086
    "$ENGINE" $ARGS --gateway-port 0 "$@" > "$log" 2>&1 &
    pid=$!
    waited=0
    while [ "$waited" -lt 40 ]; do          # 40 x 0.2s = 8s
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.2
        waited=$((waited + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
        kill -9 "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
        rc="running"
    else
        wait "$pid" 2>/dev/null
        rc=$?
    fi
}

# $1 label, $2 expected substring of the refusal, $3.. extra engine args
must_refuse() {
    label="$1"; expect="$2"; shift 2
    log="$TMP/refuse.log"
    run_with_deadline "$log" "$@"

    if [ "$rc" = "running" ]; then
        note_fail "$label: engine was STILL RUNNING at the deadline — it agreed to serve"
        cat "$log"
        return
    fi
    if [ "$rc" -eq 0 ]; then
        note_fail "$label: engine exited 0"
        cat "$log"
        return
    fi
    if grep -q "Order Gateway listening" "$log" 2>/dev/null; then
        note_fail "$label: engine bound the order port before giving up"
        cat "$log"
        return
    fi
    if ! grep -q "$expect" "$log" 2>/dev/null; then
        note_fail "$label: exited $rc, but not for the expected reason (wanted \"$expect\")"
        cat "$log"
        return
    fi
    echo "  ok  $label — refused: $expect"
}

must_refuse "no credentials configured" "no participant credentials configured"
must_refuse "empty file"                "contains no credentials" \
            --participant-credentials "$TMP/empty.conf"
must_refuse "comments-only file"        "contains no credentials" \
            --participant-credentials "$TMP/comments.conf"
must_refuse "unreadable file"           "cannot open credentials file" \
            --participant-credentials "$TMP/does_not_exist.conf"

# A port that is not a number is refused too, rather than read as something else.
log="$TMP/refuse.log"
"$ENGINE" $ARGS --gateway-port not-a-port --no-participant-auth > "$log" 2>&1
rc=$?
if [ "$rc" -eq 0 ] || ! grep -q "is not a number" "$log"; then
    note_fail "non-numeric order port: exited $rc"
    cat "$log"
else
    echo "  ok  non-numeric order port — refused"
fi

# ── Cases that must still START ─────────────────────────────────────────────
#
# A guard that refuses everything is not a fix. These have to reach the listening
# state; a free port is the kernel's problem.
must_start() {
    label="$1"; shift
    log="$TMP/start.log"
    # Empty it HERE, before the fork. The `>` below truncates in the child, and on
    # a loaded box the poll can run first and read the previous case's "listening"
    # line — then this case's own kill -9 below killed an engine still starting up,
    # and reported an empty log.
    : > "$log"
    # shellcheck disable=SC2086
    "$ENGINE" $ARGS --gateway-port 0 "$@" > "$log" 2>&1 &
    pid=$!
    waited=0
    while [ "$waited" -lt 150 ]; do         # 150 x 0.2s = 30s: a sanitizer boot is slow
        grep -q "Order Gateway listening" "$log" 2>/dev/null && break
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.2
        waited=$((waited + 1))
    done
    if grep -q "Order Gateway listening" "$log" 2>/dev/null; then
        kill -9 "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
        echo "  ok  $label — started and listened"
        return
    fi
    kill -9 "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    st=$?
    # The status says how it ended: a process that died on its own before the
    # deadline reports its own exit code or 128+signal.
    note_fail "$label: did not reach the listening state (wait status $st after $waited x 0.2s)"
    cat "$log"
}

must_start "one valid credential" --participant-credentials "$TMP/valid.conf"
must_start "explicit --no-participant-auth" --no-participant-auth

if [ "$fails" -ne 0 ]; then
    echo "FAIL: $fails case(s) failed"
    exit 1
fi
echo "PASS: refuses an absent, empty, comments-only or unreadable credentials file; still starts when configured or explicitly opted out"
exit 0
