#!/usr/bin/env bash
#
# aws_benchmark.sh — full validation + benchmark sequence for AWS c6in.metal.
#
# Runs, in one shot, and tees everything to a timestamped log:
#   1. Full ctest suite (Release build).
#   2. perf stat — L1-dcache-load-misses / branch-misses / IPC on HonestBenchmark.
#   3. Five NUMA-pinned HonestBenchmark runs (P50/P90/P99/throughput, all 3 paths).
#   4. PGO build (scripts/pgo_build.sh) + three runs of the PGO binary.
#
# TARGET: x86_64 Linux bare metal (AWS c6in.metal). Requires perf + numactl —
# these do not exist on macOS, so run this on the AWS box, not the dev machine.

set -uo pipefail   # explicit per-step error handling (not -e: keep going to
                   # collect as much of the sequence as possible; builds abort).

# ─── Paths, run id, logging ─────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)"
LOG="$REPO_ROOT/aws-benchmark-$RUN_ID.log"
# Tee all stdout+stderr to the log AND the terminal for the rest of the script.
exec > >(tee -a "$LOG") 2>&1

NPROC="$(nproc)"
BUILD_DIR="build-aws"
BENCH_REL="benchmarks/HonestBenchmark"
BENCH_ARGS=(--orders 50000 --seed 42)
NUMA=(numactl --cpunodebind=0 --membind=0)

banner() { printf '\n\033[1m======================================================================\n%s\n======================================================================\033[0m\n' "$*"; }
warn()   { printf '\033[33mWARN: %s\033[0m\n' "$*"; }
die()    { printf '\033[31mFATAL: %s\033[0m\n' "$*"; exit 1; }

# ─── Preflight: required tooling (fail early with a clear message) ──────────
banner "aws_benchmark.sh — run $RUN_ID"
echo "repo:  $REPO_ROOT"
echo "log:   $LOG"
echo "host:  $(uname -srm)"
echo "cpus:  $NPROC"
for t in cmake ctest perf numactl; do
    command -v "$t" >/dev/null 2>&1 || die "'$t' not found. This script targets AWS c6in.metal (Linux); perf/numactl are Linux-only. Install: apt-get install -y linux-tools-common linux-tools-\$(uname -r) numactl"
done
if command -v clang++ >/dev/null 2>&1; then
    echo "clang: $(clang++ --version | head -1)"
else
    warn "clang++ not found — step 5 (PGO) will be skipped (PGO needs Clang)."
fi

# Helper: run HonestBenchmark from a given build dir, NUMA-pinned, N times.
run_bench_n() {
    local dir="$1" n="$2" label="$3" i
    local bin="$dir/$BENCH_REL"
    [[ -x "$bin" ]] || { warn "benchmark binary missing: $bin (skipping $label)"; return 1; }
    for ((i = 1; i <= n; i++)); do
        echo "--- $label: run $i/$n ---"
        "${NUMA[@]}" "$bin" "${BENCH_ARGS[@]}" || warn "$label run $i exited non-zero"
    done
}

# ─── Step 1: full ctest suite ───────────────────────────────────────────────
banner "STEP 1/4 — full ctest suite (Release)"
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang \
      -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=ON || die "step 1: configure failed"
cmake --build "$BUILD_DIR" --parallel "$NPROC" || die "step 1: build failed"
if ctest --test-dir "$BUILD_DIR" --output-on-failure -j"$NPROC"; then
    echo "STEP 1 RESULT: all tests passed"
else
    warn "STEP 1: ctest reported failures (see above). Continuing to collect benchmark data."
fi

# ─── Step 2: perf stat (L1 misses / branch misses / IPC) ────────────────────
banner "STEP 2/4 — perf stat on HonestBenchmark (per path)"
#
# ONE perf RUN PER PATH, NOT ONE RUN OVER ALL OF THEM.
#
# This step used to wrap perf stat around a single invocation carrying no path
# filter and no --no-journal, so one process generated the orders and then ran
# warmup plus measured passes of Path A, Path B AND Path C — fdatasync included
# — and the totals were divided by the order count and published as "Path A".
# That put the journal path's syscalls and cache traffic into the core-matching
# figures, and the resulting 12,638 instructions/order at IPC 1.34 implied
# ~3.25us per order against a measured 261ns P50. A counter taken over three
# paths cannot be attributed to one of them afterwards, so the paths are
# separated here at run time.
#
# Counters are still whole-PROCESS totals for the path that ran: order
# generation, the timing loop and teardown are all included. Treat them as an
# upper bound on that path, not as its cost.
for _p in a b c; do
    echo ""
    echo "--- perf stat: path ${_p} (whole-process totals for THIS path only;"
    echo "    divide by the order count for a per-order upper bound) ---"
    "${NUMA[@]}" perf stat \
        -e L1-dcache-load-misses,branch-misses,branches,instructions,cycles \
        "$BUILD_DIR/$BENCH_REL" "${BENCH_ARGS[@]}" --only "${_p}" \
        || warn "perf stat failed for path ${_p} (check /proc/sys/kernel/perf_event_paranoid; needs <=2 or CAP_PERFMON)"
done

# ─── Step 3: five NUMA-pinned runs, all three paths ─────────────────────────
banner "STEP 3/4 — five NUMA-pinned HonestBenchmark runs (P50/P90/P99/throughput, paths A/B/C)"
run_bench_n "$BUILD_DIR" 5 "step3"

# ─── Step 4: PGO build + three runs ─────────────────────────────────────────
banner "STEP 4/4 — PGO build + three runs of the PGO binary"
if command -v clang++ >/dev/null 2>&1; then
    if bash "$SCRIPT_DIR/pgo_build.sh"; then
        run_bench_n "build-pgo" 3 "pgo"
    else
        warn "pgo_build.sh failed — skipping PGO benchmark runs"
    fi
else
    warn "clang++ absent — PGO step skipped"
fi

banner "DONE — full log written to $LOG"
