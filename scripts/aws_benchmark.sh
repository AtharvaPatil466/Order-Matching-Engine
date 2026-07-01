#!/usr/bin/env bash
#
# aws_benchmark.sh — full validation + benchmark sequence for AWS c6in.metal.
#
# Runs, in one shot, and tees everything to a timestamped log:
#   1. Full ctest suite (Release build).
#   2. perf stat — L1-dcache-load-misses / branch-misses / IPC on HonestBenchmark.
#   3. Five NUMA-pinned HonestBenchmark runs (P50/P90/P99/throughput, all 3 paths).
#   4. SLAB_SIZE sweep — rebuild + benchmark at K=32, 64, 128.
#   5. PGO build (scripts/pgo_build.sh) + three runs of the PGO binary.
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
banner "STEP 1/5 — full ctest suite (Release)"
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=ON || die "step 1: configure failed"
cmake --build "$BUILD_DIR" --parallel "$NPROC" || die "step 1: build failed"
if ctest --test-dir "$BUILD_DIR" --output-on-failure -j"$NPROC"; then
    echo "STEP 1 RESULT: all tests passed"
else
    warn "STEP 1: ctest reported failures (see above). Continuing to collect benchmark data."
fi

# ─── Step 2: perf stat (L1 misses / branch misses / IPC) ────────────────────
banner "STEP 2/5 — perf stat on HonestBenchmark"
echo "(counters are whole-run totals over ${BENCH_ARGS[*]}; divide by 50000 for per-order)"
"${NUMA[@]}" perf stat \
    -e L1-dcache-load-misses,branch-misses,branches,instructions,cycles \
    "$BUILD_DIR/$BENCH_REL" "${BENCH_ARGS[@]}" \
    || warn "perf stat failed (check /proc/sys/kernel/perf_event_paranoid; needs <=2 or CAP_PERFMON)"

# ─── Step 3: five NUMA-pinned runs, all three paths ─────────────────────────
banner "STEP 3/5 — five NUMA-pinned HonestBenchmark runs (P50/P90/P99/throughput, paths A/B/C)"
run_bench_n "$BUILD_DIR" 5 "step3"

# ─── Step 4: SLAB_SIZE sweep (rebuild each) ──────────────────────────────────
banner "STEP 4/5 — SLAB_SIZE sweep (K = 32, 64, 128)"
for K in 32 64 128; do
    banner "SLAB_SIZE = $K"
    slab_dir="build-slab-$K"
    cmake -S . -B "$slab_dir" -DCMAKE_BUILD_TYPE=Release \
          -DBUILD_TESTS=OFF -DBUILD_BENCHMARKS=ON \
          -DCMAKE_CXX_FLAGS="-DOB_SLAB_SIZE=$K" \
        || { warn "SLAB_SIZE=$K: configure failed (skipping)"; continue; }
    cmake --build "$slab_dir" --parallel "$NPROC" --target HonestBenchmark \
        || { warn "SLAB_SIZE=$K: build failed (skipping)"; continue; }
    run_bench_n "$slab_dir" 3 "slab-$K"
done

# ─── Step 5: PGO build + three runs ─────────────────────────────────────────
banner "STEP 5/5 — PGO build + three runs of the PGO binary"
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
