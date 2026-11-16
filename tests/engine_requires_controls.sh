#!/bin/sh
# OrderEngine refuses to open its order port with a control off, unless that
# control is waived by name.
#
# WHAT WAS WRONG (review v2 row 2, SURF-1, SURF-4). Credentials were the only
# control the binary insisted on. Without --config every book centred its price
# band on the first order to rest, so one participant's 1-tick bid picked the
# band for the session; without --journal every acknowledged order lived only
# in memory (a WARNING line); no participant had a position limit, and the
# validator demanded limits in the file that nothing then applied; a --config
# that could not be read was a WARNING and the engine started on defaults.
#
# WHAT IS PINNED. Each case asserts why the process exited, under a deadline
# (a serving engine never exits, so "it started" is the failure being pinned):
#   - a config file that does not exist, or has a line that is not key = value
#   - a symbol with no band          (waiver --no-price-band / OB_NO_PRICE_BAND=1)
#   - no journal                     (waiver --no-journal / OB_NO_JOURNAL=1)
#   - no per-participant limits      (waiver --no-risk-limits / OB_NO_RISK_LIMITS=1)
#   - a credentialed participant with no limit
# and it still starts with every control waived, and with every control set, in
# which case the position limit is applied: an order over it does not rest.
#
# Usage: engine_requires_controls.sh <path-to-OrderEngine>

set -u

unset OB_JOURNAL_PATH OB_CONFIG_PATH OB_NODE_ROLE OB_ADMIN_TOKEN OB_ADMIN_NO_AUTH \
      OB_ADMIN_BIND OB_SESSION_SCHEDULE OB_GATEWAY_PORT OB_GATEWAY_BIND \
      OB_PARTICIPANT_CREDENTIALS OB_NO_PARTICIPANT_AUTH OB_REPLAY_LEGACY_JOURNAL \
      OB_NO_PRICE_BAND OB_NO_JOURNAL OB_NO_RISK_LIMITS OB_PARTICIPANTS \
      OB_RATE_LIMIT_DEFAULT_RATE OB_RATE_LIMIT_DEFAULT_BURST
CHAOS_TOKEN="test-only-chaos-token-not-a-secret"
export OB_CHAOS_INJECT=1 OB_CHAOS_TOKEN="$CHAOS_TOKEN"

ENGINE="$1"
ADMIN_PORT=29541
TMP="${TMPDIR:-/tmp}/ob_controls_$$"
mkdir -p "$TMP" || exit 1
EPID=""
cleanup() { [ -n "$EPID" ] && kill -9 "$EPID" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT INT TERM

fails=0
note_fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

BASE="--admin-no-auth --threads 1 --symbols 1 --gateway-port 0"
printf 'log_sink = null\n' > "$TMP/empty.conf"
printf 'log_sink = null\nthis line has no equals sign\n' > "$TMP/malformed.conf"
printf 'instrument.0.min_price = 9000\ninstrument.0.max_price = 11000\n' > "$TMP/band.conf"
printf 'participants = 100\nparticipant.100.position_limit = 10\n' > "$TMP/risk.conf"
cat "$TMP/band.conf" "$TMP/risk.conf" > "$TMP/all.conf"
printf 'test-firm:not-a-real-secret:100,101\n' > "$TMP/creds"
chmod 600 "$TMP/creds"

# $1 label, $2 expected substring of the refusal, $3.. engine args.
must_refuse() {
    label="$1"; expect="$2"; shift 2
    log="$TMP/refuse.log"
    : > "$log"
    # shellcheck disable=SC2086
    "$ENGINE" $BASE "$@" --port 0 > "$log" 2>&1 &
    pid=$!
    i=0
    while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 60 ]; do sleep 0.2; i=$((i + 1)); done
    if kill -0 "$pid" 2>/dev/null; then
        kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
        note_fail "$label: the engine STARTED"; cat "$log"; return
    fi
    wait "$pid"; rc=$?
    if [ "$rc" -eq 0 ] || grep -q "Order Gateway listening" "$log" || ! grep -q -- "$expect" "$log"; then
        note_fail "$label: exit $rc, not refused for \"$expect\""; cat "$log"; return
    fi
    echo "  ok  $label — refused: $expect"
}

must_refuse "config file missing" "could not load" \
    --config "$TMP/absent.conf" --no-price-band --no-journal --no-risk-limits --no-participant-auth
must_refuse "config line without =" "line 2" \
    --config "$TMP/malformed.conf" --no-price-band --no-journal --no-risk-limits --no-participant-auth
must_refuse "no price band" "--no-price-band" \
    --config "$TMP/risk.conf" --no-journal --no-participant-auth
must_refuse "no journal" "--no-journal" \
    --config "$TMP/all.conf" --no-participant-auth
must_refuse "no risk limits" "--no-risk-limits" \
    --config "$TMP/band.conf" --no-journal --no-participant-auth
must_refuse "credentialed participant without a limit" "participant 101" \
    --config "$TMP/all.conf" --no-journal --participant-credentials "$TMP/creds"

# $1 label, $2.. engine args. Leaves the engine running as $EPID.
must_start() {
    label="$1"; shift
    log="$TMP/start.log"
    : > "$log"
    # shellcheck disable=SC2086
    "$ENGINE" $BASE "$@" > "$log" 2>&1 &
    EPID=$!
    i=0
    while [ "$i" -lt 150 ]; do
        grep -q "Order Gateway listening" "$log" && break
        kill -0 "$EPID" 2>/dev/null || break
        sleep 0.2; i=$((i + 1))
    done
    if grep -q "Order Gateway listening" "$log"; then
        echo "  ok  $label — started"; return 0
    fi
    note_fail "$label: did not start"; cat "$log"
    kill -9 "$EPID" 2>/dev/null; wait "$EPID" 2>/dev/null; EPID=""
    return 1
}
stop_engine() { [ -n "$EPID" ] && { kill -9 "$EPID" 2>/dev/null; wait "$EPID" 2>/dev/null; }; EPID=""; }

must_start "every control waived" --port 0 --config "$TMP/empty.conf" \
    --no-price-band --no-journal --no-risk-limits --no-participant-auth
stop_engine
# The waivers by environment, with no --config at all.
OB_NO_PRICE_BAND=1 OB_NO_JOURNAL=1 OB_NO_RISK_LIMITS=1 OB_NO_PARTICIPANT_AUTH=1 \
    must_start "every control waived by environment" --port 0
stop_engine

if must_start "every control set" --port "$ADMIN_PORT" --config "$TMP/all.conf" \
       --journal "$TMP/j.wal" --no-participant-auth; then
    python3 - "$ADMIN_PORT" "$CHAOS_TOKEN" <<'PY' || note_fail "the position limit was not applied"
import json, sys, time, urllib.request
port, token = int(sys.argv[1]), sys.argv[2]
def get(path, headers=None):
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", headers=headers or {})
    return urllib.request.urlopen(req, timeout=5).read().decode()
def order(oid, qty):
    q = f"/chaos/order?orderId={oid}&participantId=100&symbolId=0&price=10000&qty={qty}&side=0"
    return get(q, {"X-Chaos-Token": token})
order(1, 50)   # over participant 100's limit of 10: refused
order(2, 5)    # within it: rests
deadline = time.time() + 10
bids = []
while time.time() < deadline:
    bids = json.loads(get("/book?symbolId=0"))["bids"]
    if bids:
        break
    time.sleep(0.05)
qty = [lv.get("quantity", lv.get("qty")) for lv in bids]
if qty != [5]:
    print(f"FAIL: bid levels {bids}; want one level of 5 (the 50 is over the position limit)")
    sys.exit(1)
PY
fi
stop_engine

if [ "$fails" -ne 0 ]; then
    echo "FAIL: $fails case(s)"
    exit 1
fi
echo "PASS: OrderEngine refuses to serve with a price band, journal or risk limit missing unless waived, and applies the limits it is given"
