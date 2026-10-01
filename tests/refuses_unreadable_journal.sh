#!/bin/sh
# The engine must refuse to start on a journal it cannot safely append to, and
# must NOT refuse one whose only damage is a torn final write.
#
# Journal::recoveryFailed() already stops the file being appended to, so the
# data on disk is safe either way. What this pins is the other half: that the
# engine does not go on to serve an empty or partial book while the real
# resting orders sit on disk. A book missing orders looks like any other book,
# so nothing about the running system would tell an operator what happened —
# refusing to boot is what makes someone read the message the Journal printed.
#
# Two cases:
#   1. Bytes that decode as no record at all — several records' worth, which is
#      what a layout change looks like from the reader's side. Refused.
#   2. A good journal followed by a torn final write (less than one record),
#      what a crash partway through a write leaves. Appending behind it lost
#      every later order at the next restart (JRN-2); refusing it (0.3) stopped
#      every restart after a crash. The engine must cut it at boot, keep the
#      bytes in <journal>.torn, and an order taken after the restart must
#      survive the next one (roadmap D2).
#
# Run against the real binary rather than the engine API, because the decisions
# being pinned live in bootJournal(), called from main.
#
# Usage: refuses_unreadable_journal.sh <path-to-OrderEngine>

set -u

# Unset rather than blank. CMake's ENVIRONMENT test property can only SET a
# variable, so clearing one there gives it an empty value — and an empty
# OB_JOURNAL_PATH is not "unconfigured", it is a configured path of "", which
# the config validator rejects before this test's own gate is ever reached.
unset OB_JOURNAL_PATH OB_CONFIG_PATH OB_NODE_ROLE OB_ADMIN_TOKEN OB_ADMIN_NO_AUTH OB_ADMIN_BIND

ENGINE="$1"
TMP="${TMPDIR:-/tmp}/ob_unreadable_journal_$$"
mkdir -p "$TMP" || exit 1
EPID=""
cleanup() { [ -n "$EPID" ] && kill -9 "$EPID" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT INT TERM

size_of() { wc -c < "$1" | tr -d ' '; }

# ── Case 1: no readable record at all ────────────────────────────────────────
WAL="$TMP/unreadable.wal"
dd if=/dev/zero bs=1024 count=1 2>/dev/null | tr '\000' 'Z' > "$WAL"
SIZE_BEFORE=$(size_of "$WAL")

# --admin-no-auth so the admin port's own required-by-default check is not what
# ends the process; the journal gate has to be the reason for the exit.
OUT=$("$ENGINE" --journal "$WAL" --admin-no-auth --symbols 1 --port 48650 2>&1)
RC=$?
if [ "$RC" -eq 0 ]; then
    echo "FAIL: engine started on an unreadable journal (exit 0)"; echo "$OUT"; exit 1
fi
if ! printf '%s' "$OUT" | grep -q "could not be opened safely"; then
    echo "FAIL: exited non-zero, but not for the journal reason:"; echo "$OUT"; exit 1
fi
if [ "$SIZE_BEFORE" != "$(size_of "$WAL")" ]; then
    echo "FAIL: the unreadable journal was modified"; exit 1
fi
echo "  ok  unreadable journal: refused, file untouched"

# ── Case 2: a good journal, then a torn final write ─────────────────────────
WAL="$TMP/torn.wal"
CHAOS_TOKEN="test-only-chaos-token-not-a-secret"

# Start the engine on $WAL with chaos order entry, wait until it is ready, and
# rest order $1 through /chaos/order. Leaves EPID set; output goes to $2.
start_and_order() {
    OB_CHAOS_INJECT=1 OB_CHAOS_TOKEN="$CHAOS_TOKEN" \
        "$ENGINE" --journal "$WAL" --admin-no-auth --symbols 1 --port 48650 > "$2" 2>&1 &
    EPID=$!
    python3 - "$CHAOS_TOKEN" "$1" <<'PY'
import json, sys, time, urllib.request
base = "http://127.0.0.1:48650"
def get(path, headers=None):
    return json.loads(urllib.request.urlopen(
        urllib.request.Request(base + path, headers=headers or {}), timeout=5).read())
deadline = time.time() + 20
while time.time() < deadline:
    try:
        if get("/readyz").get("status") == "ready":
            break
    except Exception:
        pass
    time.sleep(0.2)
else:
    sys.exit(1)
r = get("/chaos/order?orderId=%s&participantId=7&symbolId=0&price=100050&qty=25&side=0"
        % sys.argv[2], {"X-Chaos-Token": sys.argv[1]})
sys.exit(0 if r.get("accepted") else 1)
PY
}
stop_engine() { kill -TERM "$EPID"; wait "$EPID" 2>/dev/null; EPID=""; }

# A real journal, written by the engine itself: one resting order, then a clean
# stop (the header goes down with the first record, so an idle engine leaves an
# empty file).
if ! start_and_order 7 "$TMP/first.log"; then
    echo "FAIL: could not create the journal this case starts from"; cat "$TMP/first.log"; exit 1
fi
stop_engine

GOOD_END=$(size_of "$WAL")
if [ "$GOOD_END" -eq 0 ]; then
    echo "FAIL: the engine left an empty journal; there is no good data to tear after"; exit 1
fi
printf 'torn-final-write-30-bytes-xxxx' >> "$WAL"      # 30 bytes: less than one record

if ! start_and_order 8 "$TMP/second.log"; then
    echo "FAIL: the engine did not start and take an order on a journal with a torn"
    echo "      final write"; cat "$TMP/second.log"; exit 1
fi
if ! grep -q "Cut a torn final write" "$TMP/second.log"; then
    echo "FAIL: the boot log does not report the cut"; cat "$TMP/second.log"; exit 1
fi
if [ "$(cat "$WAL.torn" 2>/dev/null)" != "torn-final-write-30-bytes-xxxx" ]; then
    echo "FAIL: the torn bytes were not kept in $WAL.torn"; exit 1
fi
stop_engine

# The order taken after the cut is on disk where replay reaches it.
"$ENGINE" --journal "$WAL" --admin-no-auth --symbols 1 --port 48650 > "$TMP/third.log" 2>&1 &
EPID=$!
waited=0
while [ "$waited" -lt 60 ] && ! grep -q "Replayed" "$TMP/third.log"; do  # 60 x 0.25s = 15s
    sleep 0.25; waited=$((waited + 1))
done
stop_engine
if ! grep -q "Replayed 2 of 2 " "$TMP/third.log"; then
    echo "FAIL: the order accepted after the cut did not replay:"
    grep -E "Replayed|FATAL|REFUSING" "$TMP/third.log" || cat "$TMP/third.log"
    exit 1
fi
echo "  ok  torn final write: cut at boot, bytes kept in .torn, later order replays"

echo "PASS: refused the unreadable journal, recovered past the torn one"
exit 0
