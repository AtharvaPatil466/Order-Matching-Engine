#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build-release}"
ARTIFACT_DIR="${ARTIFACT_DIR:-benchmark-artifacts}"
THREADS="${THREADS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)}"
RUN_ID="${RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
OUT_DIR="$ARTIFACT_DIR/$RUN_ID"

mkdir -p "$OUT_DIR"

cmake -S . -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTS=ON \
  -DBUILD_BENCHMARKS=ON

cmake --build "$BUILD_DIR" --parallel "$THREADS"
ctest --test-dir "$BUILD_DIR" --output-on-failure -L project

{
  echo "run_id=$RUN_ID"
  echo "timestamp_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "os=$(uname -a)"
  echo "compiler=$("${CXX:-c++}" --version 2>/dev/null | head -n 1 || c++ --version | head -n 1)"
  if command -v sysctl >/dev/null 2>&1; then
    echo "cpu=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || true)"
    echo "hw_ncpu=$(sysctl -n hw.ncpu 2>/dev/null || true)"
  fi
  if command -v lscpu >/dev/null 2>&1; then
    lscpu
  fi
} > "$OUT_DIR/machine.txt"

"$BUILD_DIR/benchmarks/ManualBenchmark" | tee "$OUT_DIR/manual_benchmark.txt"
"$BUILD_DIR/benchmarks/E2EBenchmark" | tee "$OUT_DIR/e2e_benchmark.txt"

cat <<'EOF'

Benchmark notes:
- Run on an otherwise idle machine.
- Record CPU model, OS version, compiler version, governor/power mode, and thermal state.
- Repeat several runs and report median plus variance, not a single best run.
- Treat these as local-machine measurements, not exchange-grade latency claims.
EOF

echo "Benchmark artifacts written to $OUT_DIR"
