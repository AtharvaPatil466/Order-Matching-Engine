#!/bin/sh
# A GTD order must be gone once its expiry time has passed.
#
# WHAT WAS WRONG. MatchingEngine has an expiry sweep and a timer to run it
# (startExpiryTimer), and nothing outside the tests started it. So OrderEngine
# booked GTD orders and let them rest, display and trade forever. The timer is
# safe here: in async mode each sweep is posted to every worker as an in-band
# control message, so it never touches a book from another thread.
#
# HOW. Drives the real binary. Three resting bids go in through /chaos/order —
# a GTD due in a second, a GTD due in an hour, a GTC — each at its own price, so
# /book shows each as its own level. The first must disappear; the other two
# must stay, or a sweep that cleared every GTD, or everything, would pass.
#
# Usage: engine_expires_gtd.sh <path-to-OrderEngine>

set -u

unset OB_JOURNAL_PATH OB_CONFIG_PATH OB_NODE_ROLE OB_ADMIN_TOKEN OB_ADMIN_NO_AUTH OB_ADMIN_BIND
CHAOS_TOKEN="test-only-chaos-token-not-a-secret"
export OB_CHAOS_INJECT=1
export OB_CHAOS_TOKEN="$CHAOS_TOKEN"

ENGINE="$1"
TMP="${TMPDIR:-/tmp}/ob_expires_gtd_$$"
mkdir -p "$TMP" || exit 1
EPID=""
cleanup() { [ -n "$EPID" ] && kill -9 "$EPID" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT INT TERM

# $1 port — exit 0 pass, 1 real failure, 2 never became ready (retry the port)
probe() {
    python3 - "$1" "$CHAOS_TOKEN" <<'PY'
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
    print("  (engine never reported ready on /readyz)")
    sys.exit(2)

def wall_ns_in(seconds):
    return time.time_ns() + int(seconds * 1e9)   # Unix-epoch ns, as on the wire

GTC, GTD = 0, 1
orders = {                      # price -> (label, orderId, tif, expiryTime)
    100300: ("GTD due in 1 s", 11, GTD, wall_ns_in(1)),
    100200: ("GTD due in 1 h", 12, GTD, wall_ns_in(3600)),
    100100: ("GTC",            13, GTC, 0),
}
for price, (label, oid, tif, expiry) in orders.items():
    r = json.loads(get(f"/chaos/order?orderId={oid}&participantId=7&symbolId=0"
                       f"&price={price}&qty=10&side=0&tif={tif}&expiryTime={expiry}",
                       {"X-Chaos-Token": token}))
    if not r.get("accepted"):
        print(f"FAIL: could not submit the {label} order the test needs: {r}")
        sys.exit(1)

def resting():
    bids = json.loads(get("/book?symbolId=0"))["bids"]
    return {lv["price"] for lv in bids if lv.get("price") in orders}

# Submission is queued, so wait for all three to rest before judging anything.
deadline = time.time() + 10
while resting() != set(orders) and time.time() < deadline:
    time.sleep(0.05)
if resting() != set(orders):
    print(f"FAIL: not all three orders came to rest: {sorted(resting())}")
    sys.exit(1)
print("  ok  all three resting")

# Expiry is 1 s out and the sweep runs every 1 s; allow generous slack.
deadline = time.time() + 10
while 100300 in resting() and time.time() < deadline:
    time.sleep(0.1)

now = resting()
failed = False
for price, (label, *_rest) in orders.items():
    want = price != 100300
    have = price in now
    print(f"  {'ok ' if want == have else 'BAD'} {label}: {'resting' if have else 'gone'}")
    failed |= want != have
if 100300 in now:
    print("FAIL: a GTD order is still resting 10 s after its expiry time")
sys.exit(1 if failed else 0)
PY
}

PORT=48250
tries=0
while [ "$tries" -lt 8 ]; do
    "$ENGINE" --admin-no-auth --symbols 1 --port "$PORT" > "$TMP/engine.log" 2>&1 &
    EPID=$!
    probe "$PORT"
    rc=$?
    [ "$rc" -ne 2 ] && break
    if kill -0 "$EPID" 2>/dev/null; then
        echo "FAIL: engine running but never ready on port $PORT"; cat "$TMP/engine.log"; exit 1
    fi
    wait "$EPID" 2>/dev/null
    EPID=""
    PORT=$((PORT + 1))
    tries=$((tries + 1))
done
[ "$tries" -ge 8 ] && { echo "FAIL: no usable admin port in 48250-48257"; exit 1; }
[ "$rc" -ne 0 ] && exit 1

echo "PASS: a GTD order expired on time; a later GTD and a GTC stayed"
exit 0
