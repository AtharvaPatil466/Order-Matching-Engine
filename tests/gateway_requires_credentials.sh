#!/bin/sh
# The order-entry port must refuse to serve unauthenticated — including when a
# credentials file WAS configured and turned out to hold nothing.
#
# WHAT WAS WRONG. gateway_main treated only a negative return from
# ParticipantAuth::loadFromFile as fatal. A file of nothing but comments and
# blank lines parses cleanly and returns 0, which passed. enabled() is then
# false, so
#
#     gateway.setParticipantAuth(participantAuth.enabled() ? &participantAuth : nullptr)
#
# passed nullptr, which every gateway reads as "take each claimed identity on
# trust". The operator saw "Loaded 0 participant credential(s)", the port came
# up, and any client could claim to be any participant. A fail-open on a control
# that was configured is worse than one that was forgotten, because nothing in
# the log looks wrong.
#
# WHY THIS REPLACED A PURE-CMAKE add_test. The previous coverage was
#
#     add_test(NAME GatewayRequiresAuthTest COMMAND GatewayServer 0)
#     WILL_FAIL TRUE
#
# and it rotted silently. Once gateway_main learned to validate its port
# argument, "0" was rejected with "PORT must be 1-65535" and the process exited
# 1 before it ever reached the credentials check. WILL_FAIL cannot tell those
# apart, so the test stayed green while testing nothing. Every case below
# therefore asserts WHY the process exited, not just that it did — the same
# reason refuses_unreadable_journal.sh greps for its message.
#
# A REFUSAL IS ALSO A DEADLINE. The failure mode being pinned is "the gateway
# starts serving", and a serving gateway never exits: it blocks in its publish
# loop. So a case that must refuse is run with a deadline rather than read
# through command substitution, which would hang the suite instead of reporting
# the defect. Asserting on the exit code alone cannot see this failure at all.
#
# Usage: gateway_requires_credentials.sh <path-to-GatewayServer>

set -u

# Unset rather than blank: a developer who exports these in their shell must not
# silently pass this, and CMake's ENVIRONMENT property can only SET a variable,
# so clearing one there gives it an empty value instead of an absent one. An
# empty OB_ENGINE_HOST is not "unconfigured" — it is a configured host of "",
# which would send the gateway down the forwarding path and past every check
# here. Same set-but-empty trap the journal test documents.
unset OB_PARTICIPANT_CREDENTIALS OB_NO_PARTICIPANT_AUTH OB_ENGINE_HOST OB_ENGINE_PORT

GW="$1"
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

# A real port, so a refusal is the credentials check talking and not argument
# validation. The refusing cases never reach a bind; the starting cases below
# pick their own free port.
DEAD_PORT=47653

# Run the gateway with a deadline. Sets $rc to its exit status, or to the string
# "running" if it was still alive at the deadline — which is what "it agreed to
# serve" looks like, since a serving gateway blocks forever.
run_with_deadline() {
    log="$1"; shift
    "$GW" "$@" > "$log" 2>&1 &
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

# $1 label, $2 expected substring of the refusal, $3.. extra gateway args
must_refuse() {
    label="$1"; expect="$2"; shift 2
    log="$TMP/refuse.log"
    run_with_deadline "$log" "$DEAD_PORT" "$@"

    if [ "$rc" = "running" ]; then
        note_fail "$label: gateway was STILL RUNNING at the deadline — it agreed to serve"
        cat "$log"
        return
    fi
    if [ "$rc" -eq 0 ]; then
        note_fail "$label: gateway exited 0"
        cat "$log"
        return
    fi
    if grep -q "Order Gateway listening" "$log" 2>/dev/null; then
        note_fail "$label: gateway bound a port before giving up"
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

# ── Cases that must still START ─────────────────────────────────────────────
#
# A guard that refuses everything is not a fix. These two have to reach the
# listening state, so they need a port that is actually free; try a few, since a
# single hardcoded port makes a flaky test on a busy box.
must_start() {
    label="$1"; shift
    port=47700
    tries=0
    while [ "$tries" -lt 8 ]; do
        log="$TMP/start.log"
        "$GW" "$port" "$@" > "$log" 2>&1 &
        pid=$!
        waited=0
        while [ "$waited" -lt 40 ]; do
            grep -q "Order Gateway listening" "$log" 2>/dev/null && break
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.2
            waited=$((waited + 1))
        done
        if grep -q "Order Gateway listening" "$log" 2>/dev/null; then
            kill -9 "$pid" 2>/dev/null
            wait "$pid" 2>/dev/null
            echo "  ok  $label — started and listened on $port"
            return
        fi
        kill -9 "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
        # Only a taken port is worth retrying; anything else is a real failure.
        if ! grep -q "Failed to start gateway on port" "$log" 2>/dev/null; then
            note_fail "$label: did not reach the listening state"
            cat "$log"
            return
        fi
        port=$((port + 1))
        tries=$((tries + 1))
    done
    note_fail "$label: no free port found in 47700-47707"
}

must_start "one valid credential" --participant-credentials "$TMP/valid.conf"
must_start "explicit --no-participant-auth" --no-participant-auth

if [ "$fails" -ne 0 ]; then
    echo "FAIL: $fails case(s) failed"
    exit 1
fi
echo "PASS: refuses an absent, empty, comments-only or unreadable credentials file; still starts when configured or explicitly opted out"
exit 0
