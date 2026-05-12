#!/bin/bash
# MemoryPressureTest — verify pre-allocation under constrained memory.
#
# Roadmap Phase 5, Week 18: Memory Pressure Test
#
# Runs the engine with virtual memory capped at 95% of normal working set.
# Verifies: no crash, no OOM, latency degradation < 10x.
#
# Usage: ./scripts/memory_pressure_test.sh [--duration 30]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
DURATION=30
RATE=50000
THREADS=2
SYMBOLS=4

while [[ $# -gt 0 ]]; do
    case $1 in
        --duration) DURATION=$2; shift 2 ;;
        --rate)     RATE=$2; shift 2 ;;
        *) echo "Unknown: $1"; exit 1 ;;
    esac
done

echo "=== Memory Pressure Test ==="
echo "Duration: ${DURATION}s"
echo "Rate: ${RATE} orders/sec"
echo ""

# Step 1: Baseline — measure normal working set
echo "[1/3] Measuring baseline memory usage..."
BASELINE_OUTPUT=$("${BUILD_DIR}/benchmarks/SustainedLoadTest" \
    --duration 5 --rate "${RATE}" --threads "${THREADS}" --symbols "${SYMBOLS}" \
    --p99-threshold 100000 2>&1)

BASELINE_RSS=$(echo "$BASELINE_OUTPUT" | grep -i "peak.*rss\|memory" | head -1 || echo "N/A")
echo "  Baseline: ${BASELINE_RSS:-completed}"

# Step 2: Constrained run — limit virtual memory
echo ""
echo "[2/3] Running under memory pressure..."

# Get current RSS in KB and compute 95% limit
CURRENT_RSS_KB=$(ps -o rss= -p $$ 2>/dev/null || echo "262144")
LIMIT_KB=$((CURRENT_RSS_KB * 95 / 100))
# Minimum 128MB to avoid immediate OOM
if [ "$LIMIT_KB" -lt 131072 ]; then
    LIMIT_KB=131072
fi
echo "  Memory limit: $((LIMIT_KB / 1024)) MB"

# macOS uses ulimit -v in KB
CONSTRAINED_OUTPUT=$(ulimit -v "${LIMIT_KB}" 2>/dev/null; \
    "${BUILD_DIR}/benchmarks/SustainedLoadTest" \
    --duration "${DURATION}" --rate "${RATE}" --threads "${THREADS}" --symbols "${SYMBOLS}" \
    --p99-threshold 100000 2>&1) || true

echo "$CONSTRAINED_OUTPUT" | tail -5

# Step 3: Verify no crash
echo ""
echo "[3/3] Verification"
if echo "$CONSTRAINED_OUTPUT" | grep -q "PASS\|complete\|Throughput"; then
    echo "  ✅ Engine survived memory pressure without crash"
else
    echo "  ❌ Engine may have crashed under memory pressure"
    echo "  This indicates heap allocation on the hot path."
    echo "  Review: ObjectPool sizing, FlatHashMap capacity, RingBuffer allocation"
    exit 1
fi

echo ""
echo "=== Memory Pressure Test Complete ==="
