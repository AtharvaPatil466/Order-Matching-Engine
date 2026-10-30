#!/bin/sh
# OrderEngine takes each symbol's price band from its config, before any order.
#
# WHAT WAS WRONG (STP-8). ConfigValidator checked instrument.<id>.min_price and
# max_price, and nothing applied them: every book centred its fixed band on the
# first price to rest. One participant's 1-tick bid therefore picked the band
# for the session, and every order at a real price was acked, then rejected as
# out of range, then cancelled.
#
# WHAT IS PINNED. With --config, a symbol the file gives no band refuses to
# boot. With a band [9000, 11000] for symbol 0, a first order at price 1 does
# not rest (it is outside the band, not the anchor of a new one), and an order
# at 10000 does. Orders go in via /chaos/order, which calls submitOrder.
#
# Usage: engine_requires_price_band.sh <path-to-OrderEngine>

set -u

unset OB_JOURNAL_PATH OB_CONFIG_PATH OB_NODE_ROLE OB_ADMIN_TOKEN OB_ADMIN_NO_AUTH \
      OB_SESSION_SCHEDULE OB_ADMIN_BIND
CHAOS_TOKEN="test-only-chaos-token-not-a-secret"
export OB_CHAOS_INJECT=1
export OB_CHAOS_TOKEN="$CHAOS_TOKEN"

ENGINE="$1"
TMP="${TMPDIR:-/tmp}/ob_price_band_$$"
mkdir -p "$TMP" || exit 1
EPID=""
cleanup() { [ -n "$EPID" ] && kill -9 "$EPID" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT INT TERM

ADMIN_PORT=29501
fails=0
note_fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

# ── 1. A config with no band for symbol 0 refuses to boot ────────────────────
printf 'log_sink = null\n' > "$TMP/noband.conf"
"$ENGINE" --config "$TMP/noband.conf" --admin-no-auth --symbols 1 --port "$ADMIN_PORT" \
    > "$TMP/noband.log" 2>&1 &
EPID=$!
i=0
while kill -0 "$EPID" 2>/dev/null && [ "$i" -lt 80 ]; do sleep 0.25; i=$((i + 1)); done
if kill -0 "$EPID" 2>/dev/null; then
    note_fail "the engine STARTED with no price band configured for symbol 0"
    kill -9 "$EPID" 2>/dev/null
else
    wait "$EPID"; rc=$?
    if [ "$rc" -eq 0 ] || ! grep -q "price band" "$TMP/noband.log"; then
        note_fail "exit $rc, and not refused for the missing price band:"; cat "$TMP/noband.log"
    fi
fi
wait "$EPID" 2>/dev/null
EPID=""

# ── 2. The configured band is applied before the first order ─────────────────
printf 'log_sink = null\ninstrument.0.min_price = 9000\ninstrument.0.max_price = 11000\n' \
    > "$TMP/band.conf"
"$ENGINE" --config "$TMP/band.conf" --admin-no-auth --symbols 1 --port "$ADMIN_PORT" \
    > "$TMP/band.log" 2>&1 &
EPID=$!
python3 - "$ADMIN_PORT" "$CHAOS_TOKEN" <<'PY' || fails=$((fails + 1))
import json, sys, time, urllib.error, urllib.request

port, token = int(sys.argv[1]), sys.argv[2]
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
    print("FAIL: the engine never reported ready with a band configured")
    sys.exit(1)

def order(oid, price):
    q = f"/chaos/order?orderId={oid}&participantId=1&symbolId=0&price={price}&qty=5&side=0"
    return json.loads(get(q, {"X-Chaos-Token": token}))

order(1, 1)       # below the band: refused, not the anchor of a new band
order(2, 10000)   # inside it: rests

deadline = time.time() + 10
bids = []
while time.time() < deadline:
    bids = [lv.get("price") for lv in json.loads(get("/book?symbolId=0"))["bids"]]
    if 10000 in bids:
        break
    time.sleep(0.05)
if bids != [10000]:
    print(f"FAIL: bids are {bids}; want only 10000 (a 1-tick bid must not rest "
          f"outside the configured band, and 10000 must rest inside it)")
    sys.exit(1)
PY
kill -TERM "$EPID" 2>/dev/null
i=0
while kill -0 "$EPID" 2>/dev/null && [ "$i" -lt 80 ]; do sleep 0.25; i=$((i + 1)); done
kill -9 "$EPID" 2>/dev/null; wait "$EPID" 2>/dev/null
EPID=""

if [ "$fails" -ne 0 ]; then
    echo "--- engine log (band case) ---"; cat "$TMP/band.log"
    exit 1
fi
echo "PASS: OrderEngine requires and applies each symbol's configured price band"
