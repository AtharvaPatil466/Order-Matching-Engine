#!/bin/sh
# The engine must refuse to replay a journal that may hold orders nobody sent.
#
# WHAT THIS GUARDS, AND WHY IT ONLY BECAME A BUG WHEN THE OTHER TWO WERE FIXED.
#
# Until recently this binary submitted 200 synthetic Limit orders per symbol at
# boot as participants 1 and 2, and enableJournal ran BEFORE that loop — so every
# journal an older build wrote holds them. Deleting the loop stopped new ones
# appearing. Wiring replay into boot then made the old ones come back, and the
# combination is worse than either bug alone: recovery restores fabricated
# liquidity, clients trade against it, and the log reports
#
#     [Engine] Replayed 400 of 400 recoverable journal entries
#
# which reads as a clean recovery. Measured on a journal written by the 1bfa4fc
# binary: 10 bid and 10 ask levels on both symbols after restart.
#
# The signal is JournalFileHeader::contentEpoch, which was the `reserved` field —
# written as 0 and never read. That is what makes it usable: every pre-existing
# file already says LEGACY without anything having to migrate it.
#
# This is deliberately NOT a formatVersion bump. The record layout did not
# change: a legacy journal frames perfectly and is entirely readable. A version
# mismatch sets recoveryFailed_, whose message says reading "would misframe every
# record" — untrue here — and which also blocks appending and offers no override.
# Content trust and layout compatibility are different questions.
#
# The file used below is not a mock: it is a header with contentEpoch=0 followed
# by records the engine itself wrote, so it is byte-identical to what an older
# build leaves behind apart from the entries' contents.
#
# Usage: engine_refuses_legacy_journal.sh <path-to-OrderEngine>

set -u

unset OB_JOURNAL_PATH OB_CONFIG_PATH OB_NODE_ROLE OB_ADMIN_TOKEN OB_ADMIN_NO_AUTH \
      OB_REPLAY_LEGACY_JOURNAL OB_CHAOS_INJECT OB_CHAOS_TOKEN

ENGINE="$1"
TMP="${TMPDIR:-/tmp}/ob_legacy_jrnl_$$"
mkdir -p "$TMP" || exit 1
EPID=""
cleanup() {
    [ -n "$EPID" ] && kill -9 "$EPID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

fails=0
note_fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

CHAOS_TOKEN="test-only-chaos-token-not-a-secret"
MODERN="$TMP/modern.wal"
LEGACY="$TMP/legacy.wal"

# ── Step 1: let the engine write a journal with a real order in it ───────────
#
# Done through /chaos/order, which calls submitOrder, so the entries are the same
# shape a client produces. This also doubles as the positive control: a journal
# this build wrote must replay without complaint.
PORT=47870
tries=0
while [ "$tries" -lt 8 ]; do
    OB_CHAOS_INJECT=1 OB_CHAOS_TOKEN="$CHAOS_TOKEN" \
        "$ENGINE" --journal "$MODERN" --admin-no-auth --symbols 2 --port "$PORT" \
        > "$TMP/seed.log" 2>&1 &
    EPID=$!
    if python3 - "$PORT" "$CHAOS_TOKEN" <<'PY'
import json, sys, time, urllib.error, urllib.request
port, token = int(sys.argv[1]), sys.argv[2]
base = f"http://127.0.0.1:{port}"
deadline = time.time() + 20
while time.time() < deadline:
    try:
        if json.loads(urllib.request.urlopen(base + "/readyz", timeout=5).read()).get("status") == "ready":
            break
    except (urllib.error.HTTPError, urllib.error.URLError, OSError, ValueError):
        pass
    time.sleep(0.2)
else:
    sys.exit(2)
req = urllib.request.Request(
    base + "/chaos/order?orderId=11&participantId=9&symbolId=0&price=100200&qty=30&side=0",
    headers={"X-Chaos-Token": token})
r = json.loads(urllib.request.urlopen(req, timeout=5).read())
if not r.get("accepted"):
    print(f"could not seed the journal: {r}")
    sys.exit(1)
PY
    then break; fi
    rc=$?
    kill -9 "$EPID" 2>/dev/null; wait "$EPID" 2>/dev/null; EPID=""
    [ "$rc" -eq 1 ] && { note_fail "seeding the journal failed"; cat "$TMP/seed.log"; exit 1; }
    PORT=$((PORT + 1)); tries=$((tries + 1))
done
[ -z "$EPID" ] && { echo "FAIL: no usable admin port in 47870-47877"; exit 1; }

kill -TERM "$EPID" 2>/dev/null
waited=0
while [ "$waited" -lt 60 ]; do kill -0 "$EPID" 2>/dev/null || break; sleep 0.25; waited=$((waited + 1)); done
kill -9 "$EPID" 2>/dev/null; wait "$EPID" 2>/dev/null; EPID=""

[ -s "$MODERN" ] || { echo "FAIL: nothing was journaled, so there is nothing to test with"; exit 1; }

# ── Step 2: same bytes, contentEpoch stamped back to 0 ──────────────────────
python3 - "$MODERN" "$LEGACY" <<'PY'
import struct, sys
src, dst = sys.argv[1], sys.argv[2]
raw = open(src, "rb").read()
# JournalFileHeader: char magic[8]; uint32 formatVersion; uint32 recordSize;
#                    uint64 contentEpoch   (packed, 24 bytes)
magic, fmt, rec, epoch = struct.unpack_from("<8sIIQ", raw, 0)
assert magic == b"OBJRNL\0\0", f"unexpected magic {magic!r}"
assert epoch == 1, f"expected a modern journal to be stamped 1, got {epoch}"
open(dst, "wb").write(struct.pack("<8sIIQ", magic, fmt, rec, 0) + raw[24:])
print(f"  built a legacy journal: epoch 1 -> 0, {len(raw)} bytes, records untouched")
PY
[ $? -eq 0 ] || { echo "FAIL: could not build the legacy journal"; exit 1; }

# ── The cases ───────────────────────────────────────────────────────────────
#
# A refusal is immediate, but an engine that decides to SERVE never exits — so
# every case runs under a deadline. Asserting on the exit code alone cannot see
# the failure this test exists for.
# $1 label, $2 "refuse"|"start", $3 journal, $4.. extra args
run_case() {
    label="$1"; want="$2"; wal="$3"; shift 3
    log="$TMP/case.log"
    P=47880
    "$ENGINE" --journal "$wal" --admin-no-auth --symbols 2 --port "$P" "$@" > "$log" 2>&1 &
    pid=$!
    # Stop as soon as the outcome is known, either way: the process exited
    # (refused) or it reported ready (started). Waiting out a fixed deadline for
    # the starting cases would burn it in full every run, since a serving engine
    # never exits -- and "ready" is the stronger claim anyway, because "has not
    # exited yet" is also true of an engine wedged before it ever bound a port.
    waited=0
    state="running"
    while [ "$waited" -lt 120 ]; do
        if ! kill -0 "$pid" 2>/dev/null; then
            wait "$pid" 2>/dev/null
            state=$?
            break
        fi
        if python3 -c "
import json,sys,urllib.request
try:
    sys.exit(0 if json.loads(urllib.request.urlopen('http://127.0.0.1:$P/readyz',timeout=2).read()).get('status')=='ready' else 1)
except Exception:
    sys.exit(1)
" 2>/dev/null; then
            break
        fi
        sleep 0.25
        waited=$((waited + 1))
    done
    if [ "$state" = "running" ]; then
        kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
    fi

    if [ "$want" = "refuse" ]; then
        if [ "$state" = "running" ]; then
            note_fail "$label: engine STARTED and is serving — it replayed the legacy journal"
            grep -E "Replayed|listening|Ready" "$log" | head -3
            return
        fi
        if [ "$state" -eq 0 ]; then
            note_fail "$label: engine exited 0 instead of refusing"; cat "$log"; return
        fi
        if ! grep -q "seeded synthetic orders at startup" "$log"; then
            note_fail "$label: exited $state, but not for the legacy-journal reason"
            cat "$log"; return
        fi
        echo "  ok  $label — refused, and said why"
    else
        if [ "$state" != "running" ]; then
            note_fail "$label: engine exited ($state) instead of starting"; cat "$log"; return
        fi
        echo "  ok  $label — started"
    fi
}

run_case "legacy journal"                      refuse "$LEGACY"
run_case "legacy journal, explicit override"   start  "$LEGACY" --replay-legacy-journal
run_case "journal this build wrote"            start  "$MODERN"

# ── Laundering: an override must not expire at the next routine restart ─────
#
# gracefulShutdown checkpoints on every clean stop, and the checkpoint rewrites
# the journal through a fresh Journal whose header this build stamps. If that
# stamp is "no seeding", one override boot plus one ordinary restart converts a
# legacy journal into a TRUSTED one: the synthetic orders are now in a file the
# next boot replays without refusal, without the flag, and without a warning.
# Reproduced on a 1bfa4fc journal before this case existed: epoch 0 -> 1 across
# a SIGTERM, then the fake orders back on /book with no flag passed.
#
# So: boot with the override, stop the way an operator does, and require that
# the epoch survived the checkpoint AND that a flagless boot is still refused.
LAUNDER="$TMP/launder.wal"
cp "$LEGACY" "$LAUNDER"
LP=47890
"$ENGINE" --journal "$LAUNDER" --admin-no-auth --symbols 2 --port "$LP" \
          --replay-legacy-journal > "$TMP/launder.log" 2>&1 &
EPID=$!
if python3 - "$LP" <<'PY'
import json, sys, time, urllib.error, urllib.request
port = int(sys.argv[1])
deadline = time.time() + 30
while time.time() < deadline:
    try:
        if json.loads(urllib.request.urlopen(f"http://127.0.0.1:{port}/readyz", timeout=2).read()).get("status") == "ready":
            sys.exit(0)
    except (urllib.error.HTTPError, urllib.error.URLError, OSError, ValueError):
        pass
    time.sleep(0.2)
sys.exit(1)
PY
then
    # A clean stop, so gracefulShutdown's checkpoint runs — that is the path
    # under test. kill -9 would skip it and prove nothing.
    kill -TERM "$EPID" 2>/dev/null
    waited=0
    while [ "$waited" -lt 120 ]; do kill -0 "$EPID" 2>/dev/null || break; sleep 0.25; waited=$((waited + 1)); done
    kill -9 "$EPID" 2>/dev/null; wait "$EPID" 2>/dev/null; EPID=""

    epoch=$(python3 -c "import struct,sys; print(struct.unpack_from('<8sIIQ', open(sys.argv[1],'rb').read(), 0)[3])" "$LAUNDER")
    if [ "$epoch" != "0" ]; then
        note_fail "clean restart after an override re-stamped the legacy journal as trusted (contentEpoch $epoch)"
    else
        echo "  ok  override boot + clean stop — checkpoint kept contentEpoch 0"
    fi
    run_case "same journal, next boot without the flag" refuse "$LAUNDER"
else
    kill -9 "$EPID" 2>/dev/null; wait "$EPID" 2>/dev/null; EPID=""
    note_fail "override boot never became ready, so the laundering case could not run"
    cat "$TMP/launder.log"
fi

if [ "$fails" -ne 0 ]; then
    echo "FAIL: $fails case(s) failed"
    exit 1
fi
echo "PASS: refuses a legacy journal, honours the override, and replays its own"
exit 0
