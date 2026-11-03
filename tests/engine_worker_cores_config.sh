#!/bin/sh
# `worker_cores` in the config file names the CPU each async worker pins to,
# and a list the engine cannot honour refuses to boot (roadmap 2.12: config is
# validated or rejected, never ignored).
#
# Against the real binary, each case under a deadline:
#   * a CPU outside the process's mask refuses, naming the key (every platform:
#     macOS has no mask, so any list refuses there)
#   * Linux: a list shorter than --threads refuses
#   * Linux: a valid list starts, and engine_start reports the worker on it
# Before the key existed it was ignored: the engine started in every case with
# worker 0 on the first allowed CPU.
#
# Usage: engine_worker_cores_config.sh <path-to-OrderEngine>

set -u

unset OB_WORKER_CORES OB_SESSION_SCHEDULE OB_JOURNAL_PATH OB_CONFIG_PATH OB_NODE_ROLE \
      OB_ADMIN_TOKEN OB_ADMIN_NO_AUTH OB_CHAOS_INJECT OB_CHAOS_TOKEN OB_LOG_SINK

ENGINE="$1"
TMP="${TMPDIR:-/tmp}/ob_worker_cores_$$"
mkdir -p "$TMP" || exit 1
PID=""
cleanup() { [ -n "$PID" ] && kill -9 "$PID" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT INT TERM

ADMIN_PORT=31201
fails=0
note_fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

# $1 label, $2 --threads, $3 the worker_cores value, $4 refuse|start,
# $5 text the log must contain
run_case() {
    label="$1"; want="$4"
    conf="$TMP/engine.conf"
    log="$TMP/case.log"
    printf 'log_sink = stderr\ninstrument.0.min_price = 1\ninstrument.0.max_price = 200000\nworker_cores = %s\n' \
        "$3" > "$conf"
    "$ENGINE" --config "$conf" --admin-no-auth --symbols 1 --threads "$2" \
        --port "$ADMIN_PORT" > "$log" 2>&1 &
    PID=$!
    state="running"
    waited=0
    while [ "$waited" -lt 120 ]; do            # 120 x 0.25s = 30s
        if ! kill -0 "$PID" 2>/dev/null; then
            wait "$PID" 2>/dev/null; state=$?; break
        fi
        grep -q '"event":"engine_start"' "$log" && break
        sleep 0.25; waited=$((waited + 1))
    done
    if [ "$state" = "running" ]; then
        kill -TERM "$PID" 2>/dev/null
        i=0
        while kill -0 "$PID" 2>/dev/null && [ "$i" -lt 120 ]; do sleep 0.25; i=$((i + 1)); done
        kill -9 "$PID" 2>/dev/null; wait "$PID" 2>/dev/null
    fi
    PID=""

    if [ "$want" = "refuse" ] && [ "$state" = "running" ]; then
        note_fail "$label: engine STARTED — worker_cores = $3 was ignored"; cat "$log"; return
    fi
    if [ "$want" = "start" ] && [ "$state" != "running" ]; then
        note_fail "$label: engine exited $state"; cat "$log"; return
    fi
    if ! grep -qF -- "$5" "$log"; then
        note_fail "$label: log does not contain: $5"; cat "$log"; return
    fi
    echo "  ok  $label — $want, log has: $5"
}

run_case "CPU outside the mask" 1 4096 refuse "worker_cores '4096'"

# The CPUs this process may run on; empty where there is no mask (macOS).
cpus=$(python3 -c 'import os; c = sorted(os.sched_getaffinity(0)); print(c[0], c[-1])' 2>/dev/null)
if [ -n "$cpus" ]; then
    first=${cpus% *}; last=${cpus#* }
    run_case "shorter than --threads" 2 "$first" refuse "names 1 CPU(s) for 2 worker threads"
    run_case "valid list" 1 "$last" start "\"worker_cores\":\"$last\""
else
    echo "  --  no CPU affinity mask here: the Linux-only cases are skipped"
fi

if [ "$fails" -ne 0 ]; then echo "FAIL: $fails case(s) failed"; exit 1; fi
echo "PASS: worker_cores is applied or refused, never ignored"
exit 0
