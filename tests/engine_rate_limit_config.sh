#!/bin/sh
# OrderEngine applies rate_limit.default_rate / default_burst at startup.
#
# WHAT WAS WRONG (SURF-10). Only a SIGHUP reload read the keys, so every
# restart came up with no rate limit until someone sent SIGHUP, whatever the
# config file said.
#
# WHAT IS PINNED. With rate 1/s and burst 1 in the file, three orders from one
# participant sent back to back through /chaos/order (which calls submitOrder)
# leave /metrics rateLimited at 2 or more, with no SIGHUP. A rate without a
# burst refuses to boot.
#
# Usage: engine_rate_limit_config.sh <path-to-OrderEngine>

set -u

unset OB_JOURNAL_PATH OB_CONFIG_PATH OB_NODE_ROLE OB_ADMIN_TOKEN OB_ADMIN_NO_AUTH \
      OB_ADMIN_BIND OB_SESSION_SCHEDULE OB_RATE_LIMIT_DEFAULT_RATE OB_RATE_LIMIT_DEFAULT_BURST
export OB_NO_PARTICIPANT_AUTH=1 OB_GATEWAY_PORT=0
# Not what this test is about (engine_requires_controls.sh).
export OB_NO_PRICE_BAND=1 OB_NO_JOURNAL=1 OB_NO_RISK_LIMITS=1
CHAOS_TOKEN="test-only-chaos-token-not-a-secret"
export OB_CHAOS_INJECT=1 OB_CHAOS_TOKEN="$CHAOS_TOKEN"

ENGINE="$1"
ADMIN_PORT=29551
TMP="${TMPDIR:-/tmp}/ob_rate_limit_$$"
mkdir -p "$TMP" || exit 1
EPID=""
cleanup() { [ -n "$EPID" ] && kill -9 "$EPID" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT INT TERM

fails=0

printf 'rate_limit.default_rate = 1\n' > "$TMP/noburst.conf"
"$ENGINE" --config "$TMP/noburst.conf" --admin-no-auth --symbols 1 --threads 1 --port 0 \
    > "$TMP/noburst.log" 2>&1 &
EPID=$!
i=0
while kill -0 "$EPID" 2>/dev/null && [ "$i" -lt 60 ]; do sleep 0.2; i=$((i + 1)); done
if kill -0 "$EPID" 2>/dev/null; then
    echo "FAIL: started with a rate and no burst"; fails=$((fails + 1)); kill -9 "$EPID"
elif ! grep -q "rate_limit.default_burst" "$TMP/noburst.log"; then
    echo "FAIL: not refused for the missing burst:"; cat "$TMP/noburst.log"; fails=$((fails + 1))
fi
wait "$EPID" 2>/dev/null
EPID=""

printf 'rate_limit.default_rate = 1\nrate_limit.default_burst = 1\n' > "$TMP/rate.conf"
"$ENGINE" --config "$TMP/rate.conf" --admin-no-auth --symbols 1 --threads 1 \
    --port "$ADMIN_PORT" > "$TMP/rate.log" 2>&1 &
EPID=$!
python3 - "$ADMIN_PORT" "$CHAOS_TOKEN" <<'PY' || fails=$((fails + 1))
import json, sys, time, urllib.error, urllib.request
port, token = int(sys.argv[1]), sys.argv[2]
def get(path, headers=None):
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", headers=headers or {})
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
    print("FAIL: the engine never reported ready"); sys.exit(1)
for oid in (1, 2, 3):
    get(f"/chaos/order?orderId={oid}&participantId=1&symbolId=0&price=10000&qty=1&side=0",
        {"X-Chaos-Token": token})
limited = json.loads(get("/metrics")).get("rateLimited")
if not isinstance(limited, int) or limited < 2:
    print(f"FAIL: rateLimited is {limited} after 3 orders at 1/s burst 1; want >= 2")
    sys.exit(1)
PY
if [ "$fails" -ne 0 ]; then
    echo "--- engine log ---"; cat "$TMP/rate.log"
    exit 1
fi
echo "PASS: the rate limit in the config file applies from startup"
