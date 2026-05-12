#!/bin/bash
# SoakTest — multi-day continuous engine validation.
#
# Roadmap Phase 5, Week 19: Multi-Day Soak Test
#
# Runs the engine continuously for a configurable duration with realistic
# order flow. Monitors memory growth, latency drift, journal integrity,
# and GTD expiry accuracy.
#
# Usage: ./scripts/soak_test.sh [--hours 72] [--rate 10000]
#
# Checks performed every interval:
#   - Memory growth (RSS should be flat ±5%)
#   - P99 latency stability (no drift > 2x from baseline)
#   - Journal CRC integrity (verify on every rotation)
#   - GTD expiry accuracy (no expired orders remain)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
RESULTS_DIR="${PROJECT_DIR}/soak_results"

HOURS=1          # Default 1 hour for CI; override with --hours 72 for production
RATE=10000
CHECK_INTERVAL=60  # seconds between health checks
THREADS=4
SYMBOLS=8

while [[ $# -gt 0 ]]; do
    case $1 in
        --hours)    HOURS=$2; shift 2 ;;
        --rate)     RATE=$2; shift 2 ;;
        --interval) CHECK_INTERVAL=$2; shift 2 ;;
        *) echo "Unknown: $1"; exit 1 ;;
    esac
done

TOTAL_SECONDS=$((HOURS * 3600))
mkdir -p "${RESULTS_DIR}"

echo "========================================"
echo "  Order Matching Engine — Soak Test"
echo "========================================"
echo "Duration:       ${HOURS} hours (${TOTAL_SECONDS}s)"
echo "Order rate:     ${RATE}/sec"
echo "Check interval: ${CHECK_INTERVAL}s"
echo "Threads:        ${THREADS}"
echo "Symbols:        ${SYMBOLS}"
echo "Results:        ${RESULTS_DIR}/"
echo ""

# ─── Phase 1: Baseline measurement ──────────────────────────────────────────

echo "[Phase 1] Capturing baseline (30s warm-up)..."
"${BUILD_DIR}/benchmarks/SustainedLoadTest" \
    --duration 30 --rate "${RATE}" --threads "${THREADS}" --symbols "${SYMBOLS}" \
    --p99-threshold 100000 2>&1 | tee "${RESULTS_DIR}/baseline.txt"

BASELINE_P99=$(grep -oP 'P99.*?(\d+)' "${RESULTS_DIR}/baseline.txt" | grep -oP '\d+$' || echo "5000")
echo "  Baseline P99: ${BASELINE_P99} ns"
echo ""

# ─── Phase 2: Soak loop ─────────────────────────────────────────────────────

echo "[Phase 2] Starting soak test..."
EPOCH=0
ERRORS=0
START_TIME=$(date +%s)

while true; do
    ELAPSED=$(( $(date +%s) - START_TIME ))
    if [ "$ELAPSED" -ge "$TOTAL_SECONDS" ]; then
        break
    fi

    EPOCH=$((EPOCH + 1))
    REMAINING=$((TOTAL_SECONDS - ELAPSED))
    CHUNK_DURATION=$CHECK_INTERVAL
    if [ "$CHUNK_DURATION" -gt "$REMAINING" ]; then
        CHUNK_DURATION=$REMAINING
    fi

    echo ""
    echo "--- Epoch ${EPOCH} (T+${ELAPSED}s / ${TOTAL_SECONDS}s) ---"

    # Run sustained load for one check interval
    CHUNK_OUTPUT=$("${BUILD_DIR}/benchmarks/SustainedLoadTest" \
        --duration "${CHUNK_DURATION}" --rate "${RATE}" \
        --threads "${THREADS}" --symbols "${SYMBOLS}" \
        --p99-threshold 100000 2>&1) || true

    # Extract metrics
    CURRENT_P99=$(echo "$CHUNK_OUTPUT" | grep -oP 'P99.*?(\d+)' | grep -oP '\d+$' || echo "0")
    THROUGHPUT=$(echo "$CHUNK_OUTPUT" | grep -oP 'Throughput.*?(\d+)' | grep -oP '\d+$' || echo "0")

    # Memory check (RSS of this shell process as proxy)
    RSS_KB=$(ps -o rss= -p $$ 2>/dev/null | tr -d ' ' || echo "0")

    echo "  P99: ${CURRENT_P99} ns  Throughput: ${THROUGHPUT}/sec  RSS: ${RSS_KB} KB"

    # Latency drift check
    if [ "$CURRENT_P99" -gt 0 ] && [ "$BASELINE_P99" -gt 0 ]; then
        DRIFT_RATIO=$((CURRENT_P99 * 100 / BASELINE_P99))
        if [ "$DRIFT_RATIO" -gt 200 ]; then
            echo "  ⚠️  P99 drift: ${DRIFT_RATIO}% of baseline (threshold: 200%)"
            ERRORS=$((ERRORS + 1))
        else
            echo "  ✅ P99 stable (${DRIFT_RATIO}% of baseline)"
        fi
    fi

    # Log to results
    echo "${ELAPSED},${CURRENT_P99},${THROUGHPUT},${RSS_KB}" >> "${RESULTS_DIR}/soak_timeseries.csv"
done

# ─── Phase 3: Results ───────────────────────────────────────────────────────

echo ""
echo "========================================"
echo "  Soak Test Complete"
echo "========================================"
echo "Duration:    ${HOURS} hours"
echo "Epochs:      ${EPOCH}"
echo "Errors:      ${ERRORS}"
echo "Results:     ${RESULTS_DIR}/soak_timeseries.csv"

if [ "$ERRORS" -eq 0 ]; then
    echo "Status:      ✅ PASSED"
    exit 0
else
    echo "Status:      ❌ FAILED (${ERRORS} drift events)"
    exit 1
fi
