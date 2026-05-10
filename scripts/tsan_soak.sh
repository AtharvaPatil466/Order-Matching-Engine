#!/usr/bin/env bash
# TSan soak runner.
#
# Builds the project with ThreadSanitizer + fault injection enabled in a
# dedicated build-tsan/ tree, then runs the concurrency-sensitive tests in
# a loop. Fails fast on the first TSan report or test assertion.
#
# Usage:
#   ./scripts/tsan_soak.sh                    # default: 20 iterations
#   ./scripts/tsan_soak.sh 100                # 100 iterations
#   ./scripts/tsan_soak.sh 0                  # run until interrupted
#
# Why these tests:
#   * CombinedChaosTest    full pipeline under all faults armed
#   * QueueChaosTest       MpscQueue producer/consumer under chaos
#   * GatewayChaosTest     concurrent gateway recv path
#   * FaultInjectorTest    explicitly tests scaffold thread safety
#   * FixTcpGatewayTest    real TCP sockets, kqueue/epoll loop
#
# Each iteration runs all tests; the OS scheduler varies thread
# interleavings between iterations, so repeated runs are effective at
# surfacing TSan-detectable races even though the chaos seeds are fixed.

set -euo pipefail

cd "$(dirname "$0")/.."
PROJECT_ROOT=$(pwd)
BUILD_DIR="$PROJECT_ROOT/build-tsan"

ITERATIONS="${1:-20}"

# Tests to run on every iteration. Order is fastest-first so a flake
# surfaces quickly.
TESTS=(
    FaultInjectorTest
    QueueChaosTest
    JournalChaosTest
    JournalCorruptionTest
    GatewayChaosTest
    FixTcpGatewayTest
    SnapshotConsistencyTest
    CombinedChaosTest
)

# --- Build -----------------------------------------------------------------

if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    echo "[tsan_soak] First-run configure of $BUILD_DIR ..."
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    cmake \
        -DCMAKE_BUILD_TYPE=Debug \
        -DENABLE_THREAD_SANITIZER=ON \
        -DENABLE_FAULT_INJECTION=ON \
        -DBUILD_BENCHMARKS=OFF \
        -DBUILD_FUZZERS=OFF \
        "$PROJECT_ROOT"
    cd "$PROJECT_ROOT"
fi

echo "[tsan_soak] Building targets: ${TESTS[*]}"
cmake --build "$BUILD_DIR" -j --target "${TESTS[@]}"

# TSan runtime options:
#   halt_on_error=1     stop the test the moment a race is reported
#   second_deadlock_stack=1  show both stacks for deadlock reports
#   suppressions=...    none yet — add as suppressions/tsan.txt grows
export TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1 abort_on_error=1"

# --- Loop ------------------------------------------------------------------

SUCCESSES=0
FAILURES=0
ITER=0

trap 'echo "[tsan_soak] interrupted at iter=$ITER"; exit 130' INT

while :; do
    ITER=$((ITER + 1))
    if [ "$ITERATIONS" -gt 0 ] && [ "$ITER" -gt "$ITERATIONS" ]; then
        break
    fi
    echo "[tsan_soak] iter=$ITER"
    for t in "${TESTS[@]}"; do
        if ! "$BUILD_DIR/tests/$t" >/dev/null 2>&1; then
            echo "[tsan_soak] FAIL on iter=$ITER test=$t"
            echo "[tsan_soak] re-running with full output:"
            "$BUILD_DIR/tests/$t" || true
            FAILURES=$((FAILURES + 1))
            exit 1
        fi
    done
    SUCCESSES=$((SUCCESSES + 1))
    echo "[tsan_soak] iter=$ITER OK ($SUCCESSES/$ITER passed)"
done

echo "[tsan_soak] DONE. $SUCCESSES iterations clean."
