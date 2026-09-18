#!/bin/sh
# The engine must refuse to start on a journal it cannot read.
#
# Journal::recoveryFailed() already stops the file being appended to, so the
# data on disk is safe either way. What this pins is the other half: that the
# engine does not go on to serve an EMPTY BOOK while the real resting orders
# sit unreadable on disk. An engine with no orders looks exactly like an engine
# at the start of a session, so nothing about the running system would tell an
# operator what happened — refusing to boot is what makes someone read the
# message the Journal printed.
#
# Run against the real binary rather than the engine API, because the decision
# being pinned lives in src/main.cpp. Same reasoning as GatewayRequiresAuthTest:
# a startup default that only a unit test covers is a default someone can
# quietly delete.
#
# Usage: refuses_unreadable_journal.sh <path-to-OrderEngine>

set -u

# Unset rather than blank. CMake's ENVIRONMENT test property can only SET a
# variable, so clearing one there gives it an empty value — and an empty
# OB_JOURNAL_PATH is not "unconfigured", it is a configured path of "", which
# the config validator rejects before this test's own gate is ever reached.
# The same set-but-empty trap the gateway had with OB_ENGINE_HOST.
unset OB_JOURNAL_PATH OB_CONFIG_PATH OB_NODE_ROLE OB_ADMIN_TOKEN OB_ADMIN_NO_AUTH

ENGINE="$1"
WAL="${TMPDIR:-/tmp}/ob_unreadable_journal_$$.wal"

cleanup() { rm -f "$WAL"; }
trap cleanup EXIT INT TERM

# 1 KiB of bytes that decode as no valid record — several whole records' worth,
# which is what a layout change looks like from the reader's side. Well above
# one record, so this is not mistaken for a torn first write, which is
# legitimate and must still be recoverable.
dd if=/dev/zero bs=1024 count=1 2>/dev/null | tr '\000' 'Z' > "$WAL"

SIZE_BEFORE=$(wc -c < "$WAL" | tr -d ' ')

# --admin-no-auth so the admin port's own required-by-default check is not what
# ends the process; the journal gate has to be the reason for the exit.
OUT=$("$ENGINE" --journal "$WAL" --admin-no-auth --symbols 1 2>&1)
RC=$?

if [ "$RC" -eq 0 ]; then
    echo "FAIL: engine started on an unreadable journal (exit 0)"
    echo "$OUT"
    exit 1
fi

if ! printf '%s' "$OUT" | grep -q "could not be read"; then
    echo "FAIL: exited non-zero, but not for the journal reason:"
    echo "$OUT"
    exit 1
fi

SIZE_AFTER=$(wc -c < "$WAL" | tr -d ' ')
if [ "$SIZE_BEFORE" != "$SIZE_AFTER" ]; then
    echo "FAIL: the unreadable journal was modified ($SIZE_BEFORE -> $SIZE_AFTER)"
    exit 1
fi

echo "PASS: refused to start, and left the journal untouched"
exit 0
