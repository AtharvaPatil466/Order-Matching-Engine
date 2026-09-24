#!/bin/sh
# A resting order must survive a restart.
#
# WHAT WAS WRONG. The engine wrote a write-ahead log and never read it back.
# MatchingEngine::replayJournal() existed, was covered by tests, and was called
# by tools/JournalReplayCLI and by nothing else — grep found no call in
# src/main.cpp at all. So every restart opened an EMPTY BOOK while the resting
# orders sat on disk. src/main.cpp already refused to boot on an UNREADABLE
# journal, with the comment that "an engine with no orders looks exactly like an
# engine at the start of a session" — and then did precisely that on a perfectly
# readable one.
#
# WHY THIS DRIVES THE BINARY. A test that calls replayJournal() directly passes
# against the broken tree, because the function was never the problem. The defect
# was that main did not call it. So this boots the real OrderEngine, puts an order
# in through a real submit path, stops it the way an operator does, starts it
# again on the same file, and asks the book. Nothing else can observe the bug.
#
# The order goes in via /chaos/order (OB_CHAOS_INJECT=1 plus a token), which
# calls engine.submitOrder — the same path a client takes, so it journals
# normally. The book is read back on /book and compared field by field, not just
# counted: a replay that restores an order at the wrong price or quantity is a
# worse outcome than one that restores nothing, and a count would pass it.
#
# Usage: engine_replays_journal.sh <path-to-OrderEngine>

set -u

unset OB_JOURNAL_PATH OB_CONFIG_PATH OB_NODE_ROLE OB_ADMIN_TOKEN OB_ADMIN_NO_AUTH
CHAOS_TOKEN="test-only-chaos-token-not-a-secret"
export OB_CHAOS_INJECT=1
export OB_CHAOS_TOKEN="$CHAOS_TOKEN"

ENGINE="$1"
TMP="${TMPDIR:-/tmp}/ob_replay_$$"
mkdir -p "$TMP" || exit 1
EPID=""
cleanup() {
    [ -n "$EPID" ] && kill -9 "$EPID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

WAL="$TMP/engine.wal"

# The order under test. Synthetic; order 7 is the roadmap's case.
OID=7
PID=7
SYM=0
PRICE=100050
QTY=25
SIDE=0          # 0 = Buy

# $1 port, $2 mode ("inject" | "expect")
# exit 0 pass, 1 real failure, 2 never became ready (caller retries the port)
probe() {
    python3 - "$1" "$2" "$CHAOS_TOKEN" "$OID" "$PID" "$SYM" "$PRICE" "$QTY" "$SIDE" <<'PY'
import json, sys, time, urllib.error, urllib.request

port, mode, token = int(sys.argv[1]), sys.argv[2], sys.argv[3]
oid, pid, sym, price, qty, side = (int(x) for x in sys.argv[4:10])
base = f"http://127.0.0.1:{port}"

def get(path, headers=None):
    req = urllib.request.Request(base + path, headers=headers or {})
    return urllib.request.urlopen(req, timeout=5).read().decode()

deadline = time.time() + 20
while time.time() < deadline:
    try:
        if json.loads(get("/readyz")).get("status") == "ready":
            break
    except (urllib.error.HTTPError, urllib.error.URLError, OSError, ValueError):
        pass
    time.sleep(0.2)
else:
    print("  (engine never reported ready on /readyz)")
    sys.exit(2)

if mode == "inject":
    q = (f"/chaos/order?orderId={oid}&participantId={pid}&symbolId={sym}"
         f"&price={price}&qty={qty}&side={side}")
    r = json.loads(get(q, {"X-Chaos-Token": token}))
    if not r.get("accepted"):
        print(f"FAIL: could not inject the order the test needs: {r}")
        sys.exit(1)

# Read the book back and compare the level field by field.
book = json.loads(get(f"/book?symbolId={sym}"))
levels = book["bids" if side == 0 else "asks"]
want_side = "bid" if side == 0 else "ask"

if mode == "inject":
    label = "after submit"
else:
    label = "AFTER RESTART"

match = [lv for lv in levels if lv.get("price") == price]
if not match:
    print(f"FAIL: {label}: no {want_side} level at {price}.")
    print(f"       the book has {len(levels)} {want_side} level(s): {levels[:3]}")
    if not levels:
        print("       the book is EMPTY — the journal on disk was not replayed")
    sys.exit(1)

lv = match[0]
if lv.get("qty") != qty:
    print(f"FAIL: {label}: {want_side} at {price} has qty {lv.get('qty')}, wanted {qty}")
    print(f"       a replay that restores the wrong size is worse than one that "
          f"restores nothing")
    sys.exit(1)

print(f"  ok  {label}: {want_side} {qty} @ {price} present")
PY
}

# Boot the engine, retrying the admin port if it is taken. Sets EPID and PORT.
PORT=47830
boot() {
    mode="$1"
    tries=0
    while [ "$tries" -lt 8 ]; do
        log="$TMP/engine.$mode.log"
        "$ENGINE" --journal "$WAL" --admin-no-auth --symbols 2 --port "$PORT" \
            > "$log" 2>&1 &
        EPID=$!
        probe "$PORT" "$mode"
        rc=$?
        [ "$rc" -ne 2 ] && return "$rc"

        if kill -0 "$EPID" 2>/dev/null; then
            echo "FAIL: engine running but never ready on port $PORT"
            cat "$log"
            return 1
        fi
        wait "$EPID" 2>/dev/null
        EPID=""
        PORT=$((PORT + 1))
        tries=$((tries + 1))
    done
    echo "FAIL: no usable admin port in 47830-47837"
    return 1
}

# Stop the way an operator does, so the journal is flushed by the real path.
stop_engine() {
    [ -z "$EPID" ] && return 0
    kill -TERM "$EPID" 2>/dev/null
    waited=0
    while [ "$waited" -lt 60 ]; do
        kill -0 "$EPID" 2>/dev/null || break
        sleep 0.25
        waited=$((waited + 1))
    done
    if kill -0 "$EPID" 2>/dev/null; then
        echo "FAIL: engine did not exit on SIGTERM"
        kill -9 "$EPID" 2>/dev/null
        wait "$EPID" 2>/dev/null
        EPID=""
        return 1
    fi
    wait "$EPID" 2>/dev/null
    EPID=""
    return 0
}

# ── Run 1: submit, confirm resting, shut down cleanly ────────────────────────
boot inject || exit 1
stop_engine || exit 1

if [ ! -s "$WAL" ]; then
    echo "FAIL: the journal is empty after a clean shutdown — nothing to replay"
    exit 1
fi

# ── Run 2: same journal, the order must be back ──────────────────────────────
boot expect || exit 1
grep -q "Replayed" "$TMP/engine.expect.log" 2>/dev/null \
    || echo "  note: no 'Replayed' line in the log (stdout is block-buffered; the book is the assertion)"
stop_engine || exit 1

echo "PASS: a resting order survived a restart"
exit 0
