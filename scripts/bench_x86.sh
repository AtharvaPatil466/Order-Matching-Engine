#!/usr/bin/env bash
#
# bench_x86.sh — build and run every benchmark the docs quote; keep the raw output.
#
# .github/workflows/bench-x86.yml runs this on an x86 runner. It writes raw
# logs and nothing else: no number is parsed, summarised or rounded here.
# scripts/import_bench_results.sh lands the uploaded artifact as
# results/<sha>/, and docs/PERF_SOURCES.md names the file each doc number will
# be rewritten from.
#
# Three Release builds, each in its own directory:
#   prod    the Dockerfile's flags: clang-18, Ninja, -DOB_ARCH=x86-64-v3
#           (armv8-a on arm64), and NO io_uring — the image installs no
#           liburing, so the shipped journal is fwrite + fdatasync. Every
#           benchmark runs on this build; it is the headline (roadmap 1.11-K3).
#   native  -march=native, io_uring when liburing is installed: the flags the
#           published d2e688c figures were measured with. HonestBenchmark only.
#   pgo     scripts/pgo_build.sh (native, trained on seed 7). HonestBenchmark only.
#
# HonestBenchmark Path C acks before the journal is durable; Path D
# (SyncPolicy::Immediate) times submit to durable ack, one fsync per order —
# the 0.4 measurement decision 1.4-D7 waits on. Path D runs on prod only: it
# measures the disk, which no compiler flag changes.
#
# usage: scripts/bench_x86.sh <out-dir>
# env (defaults are the workloads the docs quote; shrink them for a smoke run):
#   REPS=5 ORDERS=50000            HonestBenchmark repetitions and timed orders
#   RF_EVENTS=500000 RF_SPARSE_EVENTS=300000
#   CO_ORDERS=200000 CO_RATES="100000 500000 1000000 2000000 3000000 4000000 6000000"
#   CC_ORDERS=20000 SUSTAIN_SEC=20
#   CXX=clang++-18  JOURNAL_DIR=<repo>/bench-journal  BENCH_CPU=1  BENCH_SHA=<git HEAD>

set -euo pipefail

OUT_ARG="${1:?usage: scripts/bench_x86.sh <out-dir>}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"
mkdir -p "$OUT_ARG"
OUT="$(cd "$OUT_ARG" && pwd)"
if [ -n "$(ls -A "$OUT")" ]; then
    echo "FATAL: $OUT is not empty; give each run its own directory" >&2
    exit 2
fi

REPS="${REPS:-5}"
ORDERS="${ORDERS:-50000}"
RF_EVENTS="${RF_EVENTS:-500000}"
RF_SPARSE_EVENTS="${RF_SPARSE_EVENTS:-300000}"
CO_ORDERS="${CO_ORDERS:-200000}"
CO_RATES="${CO_RATES:-100000 500000 1000000 2000000 3000000 4000000 6000000}"
CC_ORDERS="${CC_ORDERS:-20000}"
SUSTAIN_SEC="${SUSTAIN_SEC:-20}"
export CXX="${CXX:-clang++-18}"
JOURNAL_DIR="${JOURNAL_DIR:-$REPO/bench-journal}"
for knob in REPS ORDERS RF_EVENTS RF_SPARSE_EVENTS CO_ORDERS CC_ORDERS SUSTAIN_SEC; do
    case "${!knob}" in
        ''|*[!0-9]*) echo "FATAL: $knob must be a whole number, got '${!knob}'" >&2; exit 2 ;;
    esac
done
JOBS="$(nproc 2>/dev/null || echo 4)"
SHA="${BENCH_SHA:-$(git rev-parse HEAD)}"
# Every build writes compile_commands.json (pgo_build.sh too), so the exact
# compile line of each binary is recorded rather than inferred.
export CMAKE_EXPORT_COMPILE_COMMANDS=ON
# clang-18 on Ubuntu keeps llvm-profdata (needed by pgo_build.sh) here.
[ -d /usr/lib/llvm-18/bin ] && export PATH="/usr/lib/llvm-18/bin:$PATH"
case "$(uname -m)" in
    aarch64|arm64) PROD_ARCH=armv8-a ;;   # the Dockerfile's choice per arch
    *)             PROD_ARCH=x86-64-v3 ;;
esac
# ponytail: one fixed core for the single-threaded benchmarks; no isolcpus on a
# hosted runner, so this removes migrations, not interference.
PIN=()
command -v taskset >/dev/null 2>&1 && PIN=(taskset -c "${BENCH_CPU:-1}")
LIMIT=()
command -v timeout >/dev/null 2>&1 && LIMIT=(timeout 1800)
mkdir -p "$JOURNAL_DIR"
FAILED=0

# record <name> <command...>: the command line, then its raw output, in
# $OUT/<name>.txt; its exit code in exit-codes.txt. Returns that exit code.
record() {
    local name="$1" rc=0
    shift
    { echo "\$ $*"; "${LIMIT[@]}" "$@"; } > "$OUT/$name.txt" 2>&1 || rc=$?
    echo "$name $rc" >> "$OUT/exit-codes.txt"
    return "$rc"
}
# run: a benchmark that must succeed (its workload checks exit 1 otherwise).
run() { record "$@" || FAILED=1; }

# ─── Environment ────────────────────────────────────────────────────────────
{
    echo "commit: $SHA"
    echo "date_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    if changes="$(git status --porcelain 2>/dev/null)"; then
        echo "worktree_changes: $(printf '%s' "$changes" | grep -c .) (git status --porcelain lines)"
    else
        echo "worktree_changes: unknown (no git repository here)"
    fi
    echo "runner: ${RUNNER_NAME:-none} image: ${ImageOS:-none} ${ImageVersion:-}"
    echo "--- uname -a"; uname -a
    echo "--- lscpu"; lscpu 2>&1 || echo "lscpu unavailable"
    echo "--- nproc: $JOBS"
    echo "--- free -m"; free -m 2>&1 || true
    echo "--- governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unreadable)"
    echo "--- compiler"; "$CXX" --version || echo "$CXX not found"
    echo "--- cmake"; cmake --version | head -1
    echo "--- liburing: $(ls /usr/include/liburing.h 2>/dev/null || echo 'not installed')"
    echo "--- journal dir: $JOURNAL_DIR"
    echo "journal_fs: $(stat -f -c %T "$JOURNAL_DIR" 2>/dev/null || echo unknown)"
    df -T "$JOURNAL_DIR" 2>&1 || df "$JOURNAL_DIR"
    echo "--- fdatasync probe on the journal dir (1000 x 4 KiB, O_DSYNC)"
    dd if=/dev/zero of="$JOURNAL_DIR/fsync_probe" bs=4k count=1000 oflag=dsync 2>&1 | tail -1 \
        || echo "dd oflag=dsync unavailable"
    rm -f "$JOURNAL_DIR/fsync_probe"
    echo "--- pinning: ${PIN[*]:-none}"
    echo "--- workload: REPS=$REPS ORDERS=$ORDERS RF_EVENTS=$RF_EVENTS RF_SPARSE_EVENTS=$RF_SPARSE_EVENTS CO_ORDERS=$CO_ORDERS CO_RATES=\"$CO_RATES\" CC_ORDERS=$CC_ORDERS SUSTAIN_SEC=$SUSTAIN_SEC"
} > "$OUT/env.txt" 2>&1

# ─── Builds ─────────────────────────────────────────────────────────────────
# compile_line <build dir>: the recorded compile command for HonestBenchmark.cpp.
compile_line() {
    grep -m1 '"command": .*HonestBenchmark.cpp' "$1/compile_commands.json" 2>/dev/null \
        || echo "no compile line recorded for $1"
}

BENCHES=(HonestBenchmark RealisticFlowBenchmark CoordinatedOmissionBenchmark ColdCacheBenchmark
         BinaryCodecBenchmark SustainedLoadTest MemoryProbe)
build() {  # build <name> <dir> <targets (space-separated)> <extra cmake args...>
    local name="$1" dir="$2" targets="$3"
    shift 3
    run "configure-$name" cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_COMPILER="$CXX" -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=ON -DBUILD_TOOLS=OFF "$@"
    # shellcheck disable=SC2086  # targets is a word list
    run "build-$name" cmake --build "$dir" --parallel "$JOBS" --target $targets
    compile_line "$dir" >> "$OUT/build-$name.txt"
}
PROD=build-bench-prod
NATIVE=build-bench-native
build prod "$PROD" "${BENCHES[*]} StressTest" -DOB_ARCH="$PROD_ARCH" -DENABLE_IO_URING=OFF
build native "$NATIVE" HonestBenchmark -DOB_ARCH=native
run build-pgo bash scripts/pgo_build.sh
compile_line build-pgo >> "$OUT/build-pgo.txt"

# ─── HonestBenchmark: builds interleaved per repetition, so drift over the run
# lands on all three alike ─────────────────────────────────────────────────
HB=(--orders "$ORDERS" --seed 42 --journal "$JOURNAL_DIR/hb.journal")
for i in $(seq 1 "$REPS"); do
    run "honest-prod-$i"   "${PIN[@]}" "$PROD/benchmarks/HonestBenchmark" "${HB[@]}"
    run "honest-native-$i" "${PIN[@]}" "$NATIVE/benchmarks/HonestBenchmark" "${HB[@]}" --no-immediate
    run "honest-pgo-$i"    "${PIN[@]}" build-pgo/benchmarks/HonestBenchmark "${HB[@]}" --no-immediate
done

# perf counters, one process per path (a counter over several paths cannot be
# split afterwards). Best effort: a hosted VM usually exposes no PMU.
for p in a l b c d; do
    record "perf-prod-$p" perf stat -e L1-dcache-load-misses,branch-misses,branches,instructions,cycles \
        "${PIN[@]}" "$PROD/benchmarks/HonestBenchmark" "${HB[@]}" --only "$p" || true
done

# ─── The other benchmarks the docs quote, on the prod build ─────────────────
run realistic-flow        "${PIN[@]}" "$PROD/benchmarks/RealisticFlowBenchmark" --events "$RF_EVENTS" --seed 42
run realistic-flow-sparse "${PIN[@]}" "$PROD/benchmarks/RealisticFlowBenchmark" --sparse --events "$RF_SPARSE_EVENTS" --seed 42
for r in $CO_RATES; do
    run "co-$r" "${PIN[@]}" "$PROD/benchmarks/CoordinatedOmissionBenchmark" --orders "$CO_ORDERS" --seed 7 --rate "$r"
done
run cold-cache     "${PIN[@]}" "$PROD/benchmarks/ColdCacheBenchmark" --orders "$CC_ORDERS" --seed 7
run codec          "${PIN[@]}" "$PROD/benchmarks/BinaryCodecBenchmark"
run sustained-400k "$PROD/benchmarks/SustainedLoadTest" --rate 400000 --threads 4 --duration "$SUSTAIN_SEC"
run sustained-50k  "$PROD/benchmarks/SustainedLoadTest" --rate 50000 --threads 4 --duration "$SUSTAIN_SEC"
run stress         "$PROD/tests/StressTest"
run memory-probe   "$PROD/benchmarks/MemoryProbe"

rm -f "$JOURNAL_DIR/hb.journal"
echo "raw output in $OUT; failed: $FAILED (see exit-codes.txt)"
exit "$FAILED"
