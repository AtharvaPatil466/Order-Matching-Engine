#!/bin/sh
# The engine must refuse to start on a journal it cannot safely append to.
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
#      what a layout change looks like from the reader's side.
#   2. A good journal followed by a torn final write (less than one record).
#      This used to be accepted: the engine appended behind the torn bytes,
#      where replay never reaches, so every order after the restart was lost at
#      the next one (JRN-2). It must refuse, keep the bytes, and print a repair
#      that cuts the file exactly where the good data ends.
#
# Run against the real binary rather than the engine API, because the decision
# being pinned lives in bootJournal(), called from main. Case 2 runs under a
# deadline: before the fix the engine STARTED, and a serving engine never exits.
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
OUT=$("$ENGINE" --journal "$WAL" --admin-no-auth --symbols 1 --port 48270 2>&1)
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
LOG="$TMP/first.log"

# A real journal, written by the engine itself: one resting order submitted
# through /chaos/order (the header is written with the first record, so an
# engine that took no orders leaves an empty file), then a clean stop.
CHAOS_TOKEN="test-only-chaos-token-not-a-secret"
OB_CHAOS_INJECT=1 OB_CHAOS_TOKEN="$CHAOS_TOKEN" \
    "$ENGINE" --journal "$WAL" --admin-no-auth --symbols 1 --port 48270 > "$LOG" 2>&1 &
EPID=$!
if ! python3 - "$CHAOS_TOKEN" <<'PY'
import json, sys, time, urllib.request
base = "http://127.0.0.1:48270"
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
r = get("/chaos/order?orderId=7&participantId=7&symbolId=0&price=100050&qty=25&side=0",
        {"X-Chaos-Token": sys.argv[1]})
sys.exit(0 if r.get("accepted") else 1)
PY
then
    echo "FAIL: could not create the journal this case starts from"; cat "$LOG"; exit 1
fi
kill -TERM "$EPID"; wait "$EPID" 2>/dev/null; EPID=""

GOOD_END=$(size_of "$WAL")
if [ "$GOOD_END" -eq 0 ]; then
    echo "FAIL: the engine left an empty journal; there is no good data to tear after"; exit 1
fi
printf 'torn-final-write-30-bytes-xxxx' >> "$WAL"      # 30 bytes: less than one record
SIZE_BEFORE=$(size_of "$WAL")

"$ENGINE" --journal "$WAL" --admin-no-auth --symbols 1 --port 48270 > "$TMP/second.log" 2>&1 &
EPID=$!
state="running"
waited=0
while [ "$waited" -lt 60 ]; do                         # 60 x 0.25s = 15s
    if ! kill -0 "$EPID" 2>/dev/null; then wait "$EPID"; state=$?; EPID=""; break; fi
    sleep 0.25; waited=$((waited + 1))
done
OUT=$(cat "$TMP/second.log")

if [ "$state" = "running" ]; then
    kill -9 "$EPID" 2>/dev/null; wait "$EPID" 2>/dev/null; EPID=""
    echo "FAIL: engine STARTED on a journal with a torn final record — it appends behind"
    echo "      bytes replay never gets past, so those orders are lost at the next restart"
    exit 1
fi
if [ "$state" -eq 0 ]; then echo "FAIL: exited 0 instead of refusing"; echo "$OUT"; exit 1; fi
if ! printf '%s' "$OUT" | grep -q "could not be opened safely"; then
    echo "FAIL: exited $state, but not for the journal reason:"; echo "$OUT"; exit 1
fi
if [ "$SIZE_BEFORE" != "$(size_of "$WAL")" ]; then
    echo "FAIL: the damaged journal was modified"; exit 1
fi
if ! printf '%s' "$OUT" | grep -q "truncate -s $GOOD_END "; then
    echo "FAIL: the printed repair does not cut the file at $GOOD_END, where the good data ends:"
    printf '%s\n' "$OUT" | grep "truncate" || echo "      (no truncate command printed)"
    exit 1
fi
echo "  ok  torn final write: refused, file untouched, repair cuts at byte $GOOD_END"

echo "PASS: refused to start on both, and left the journal untouched"
exit 0
