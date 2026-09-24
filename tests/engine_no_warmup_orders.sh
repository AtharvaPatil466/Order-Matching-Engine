#!/bin/sh
# The engine must not have a book when nobody has sent it an order.
#
# WHAT WAS WRONG. src/main.cpp ran this before announcing readiness:
#
#     std::cout << "[Engine] Warming up books...";
#     for each symbol: for (int i = 0; i < 100; ++i) {
#         engine.processOrder(sym, warmupId++, 1, Side::Buy,  100000 + i % 100, 10, Limit);
#         engine.processOrder(sym, warmupId++, 2, Side::Sell, 100100 + i % 100, 10, Limit);
#     }
#
# Those are not a warmup. They are 200 real Limit orders per symbol — 800 at the
# default --symbols 4 — attributed to participants 1 and 2, resting in the live
# book, and they were visible on /book the moment the log said "System ready":
#
#     "bids":[{"price":100099,"qty":10,"orders":1},{"price":100098,...}, ...]
#
# Real client orders matched against them, so a participant's first fill was
# against a counterparty that had never submitted anything. They consumed client
# order ids 1..800, so a client numbering from 1 collided with a duplicate. They
# were journaled, so the WAL opened with fake state. And nothing in the log said
# any of it — "Warming up books" reads as page-faulting.
#
# WHAT THIS PINS. Not the absence of a string in the log, which would pass
# against a rename. It reads the book the engine actually serves, on every
# symbol, and requires it to be empty. If page-faulting is ever wanted back it
# has to arrive as MemoryPool::warmup, which touches memory without creating
# orders, and this test stays green.
#
# WHY IT WAITS ON /readyz AND NOT ON THE LOG. Two reasons, both of which produced
# a wrong answer first. The engine's stdout is block-buffered into a file, so
# "Ready for traffic" can sit unflushed in the buffer long after it was printed —
# polling for it timed out against an engine that was already up. And the
# ordering matters for correctness, not just convenience: setReady(true) is called
# after warmup's waitForDrain(), so /readyz is the only signal that cannot be
# observed BEFORE the orders this test looks for have landed. Checking the book
# any earlier could find it empty at HEAD and pass while the defect was present.
#
# Usage: engine_no_warmup_orders.sh <path-to-OrderEngine>

set -u

# Set-but-empty is not unconfigured: an empty OB_JOURNAL_PATH is a configured
# path of "", which the config validator rejects before this test's own subject
# is reached. Unset, per refuses_unreadable_journal.sh.
unset OB_JOURNAL_PATH OB_CONFIG_PATH OB_NODE_ROLE OB_ADMIN_TOKEN OB_ADMIN_NO_AUTH \
      OB_CHAOS_INJECT OB_CHAOS_TOKEN

ENGINE="$1"
TMP="${TMPDIR:-/tmp}/ob_no_warmup_$$"
mkdir -p "$TMP" || exit 1
EPID=""
cleanup() {
    [ -n "$EPID" ] && kill -9 "$EPID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

LOG="$TMP/engine.log"
SYMBOLS=2

# Wait for /readyz, then assert the book is empty on every symbol. python3 rather
# than curl: the Ubuntu image CI builds in has python3 and does not have curl.
# Exit 2 means "never became ready", which the caller treats as a port clash
# worth retrying; exit 1 is a real failure.
check_book() {
    python3 - "$1" "$SYMBOLS" <<'PY'
import json, sys, time, urllib.error, urllib.request

port, symbols = int(sys.argv[1]), int(sys.argv[2])

def get(path):
    return urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=5).read().decode()

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

bad = []
for sym in range(symbols):
    book = json.loads(get(f"/book?symbolId={sym}"))
    bids, asks = book.get("bids", []), book.get("asks", [])
    if bids or asks:
        bad.append((sym, bids, asks))

if bad:
    print("FAIL: the engine is serving a book nobody submitted to")
    for sym, bids, asks in bad:
        print(f"  symbol {sym}: {len(bids)} bid level(s), {len(asks)} ask level(s)")
        print(f"    first bids: {bids[:2]}")
        print(f"    first asks: {asks[:2]}")
    sys.exit(1)

print(f"  ok  book empty on all {symbols} symbol(s) once /readyz reports ready")
PY
}

# A free admin port. A single hardcoded one makes a flaky test on a busy box.
PORT=47810
tries=0
rc=2
while [ "$tries" -lt 8 ]; do
    "$ENGINE" --journal "$TMP/engine.wal" --admin-no-auth --symbols "$SYMBOLS" \
              --port "$PORT" > "$LOG" 2>&1 &
    EPID=$!
    check_book "$PORT"
    rc=$?
    [ "$rc" -ne 2 ] && break

    # Not ready. A port clash is the one retryable cause; the engine exits for it.
    if kill -0 "$EPID" 2>/dev/null; then
        echo "FAIL: engine is running but never reported ready on port $PORT"
        cat "$LOG"
        exit 1
    fi
    wait "$EPID" 2>/dev/null
    EPID=""
    PORT=$((PORT + 1))
    tries=$((tries + 1))
done

if [ "$rc" -eq 2 ]; then
    echo "FAIL: no usable admin port in 47810-47817"
    cat "$LOG"
    exit 1
fi

# Shut down the way an operator does, so a failure here is not mistaken for the
# book assertion above.
kill -TERM "$EPID" 2>/dev/null
waited=0
while [ "$waited" -lt 40 ]; do
    kill -0 "$EPID" 2>/dev/null || break
    sleep 0.25
    waited=$((waited + 1))
done
kill -9 "$EPID" 2>/dev/null
wait "$EPID" 2>/dev/null
EPID=""

[ "$rc" -ne 0 ] && exit 1
echo "PASS: no orders exist until a client sends one"
exit 0
