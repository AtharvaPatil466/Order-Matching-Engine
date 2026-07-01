#!/usr/bin/env bash
#
# pgo_build.sh — Clang IR-based Profile-Guided Optimization pipeline.
#
# Three phases:
#   1. INSTRUMENT  build HonestBenchmark with -fprofile-instr-generate
#   2. PROFILE     run it (50K orders, seed=42) to emit raw profile data,
#                  then merge with `llvm-profdata merge`
#   3. OPTIMIZE    rebuild with -fprofile-instr-use=<merged .profdata>
#
# Works on macOS/ARM and Linux/x86 (Clang only — uses -fprofile-instr-*,
# NOT GCC's -fprofile-generate). Run from the repository root.
#
# Output: build-pgo/benchmarks/HonestBenchmark (PGO-optimized), and a
#         PGO-optimized libOrderMatcher inside build-pgo/.
#
# The profile reflects the HonestBenchmark 50K/seed=42 flow — representative
# for that benchmark, not a guarantee for arbitrary production order flow.

set -euo pipefail

# ─── Resolve repo root (script may be invoked from anywhere) ────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

INSTR_DIR="build-pgo-instr"
FINAL_DIR="build-pgo"
PROFDATA="$REPO_ROOT/$INSTR_DIR/default.profdata"
BENCH_ARGS=(--orders 50000 --seed 42)

# ─── Toolchain detection ────────────────────────────────────────────────────
NPROC="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

# llvm-profdata: PATH first, then xcrun (macOS CommandLineTools).
if command -v llvm-profdata >/dev/null 2>&1; then
    PROFDATA_BIN="llvm-profdata"
elif command -v xcrun >/dev/null 2>&1 && xcrun --find llvm-profdata >/dev/null 2>&1; then
    PROFDATA_BIN="$(xcrun --find llvm-profdata)"
else
    echo "ERROR: llvm-profdata not found on PATH or via xcrun. Install LLVM." >&2
    exit 1
fi

# Force Clang (IR-based PGO requires it). Respect an explicit $CXX if it's clang.
if [[ -n "${CXX:-}" ]] && "$CXX" --version 2>/dev/null | grep -qi clang; then
    CXX_COMPILER="$CXX"
elif command -v clang++ >/dev/null 2>&1; then
    CXX_COMPILER="clang++"
else
    echo "ERROR: clang++ not found — PGO pipeline requires Clang." >&2
    exit 1
fi

banner() { printf '\n\033[1m=== %s ===\033[0m\n' "$*"; }

banner "PGO pipeline"
echo "  repo:          $REPO_ROOT"
echo "  compiler:      $CXX_COMPILER ($($CXX_COMPILER --version | head -1))"
echo "  llvm-profdata: $PROFDATA_BIN"
echo "  jobs:          $NPROC"

# ─── Phase 1: INSTRUMENT ────────────────────────────────────────────────────
banner "Phase 1/3: INSTRUMENT build (-fprofile-instr-generate)"
rm -rf "$INSTR_DIR"
cmake -S . -B "$INSTR_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER="$CXX_COMPILER" \
    -DENABLE_PGO=ON -DPGO_MODE=generate \
    -DBUILD_TESTS=OFF -DBUILD_BENCHMARKS=ON
cmake --build "$INSTR_DIR" --parallel "$NPROC" --target HonestBenchmark
echo "instrumented binary: $INSTR_DIR/benchmarks/HonestBenchmark"

# ─── Phase 2: PROFILE + MERGE ───────────────────────────────────────────────
banner "Phase 2/3: PROFILE run + merge"
# %p (pid) keeps re-runs from clobbering; the run below produces one file.
rm -f "$INSTR_DIR"/pgo-*.profraw
LLVM_PROFILE_FILE="$REPO_ROOT/$INSTR_DIR/pgo-%p.profraw" \
    "./$INSTR_DIR/benchmarks/HonestBenchmark" "${BENCH_ARGS[@]}"
echo "raw profiles:"
ls -la "$INSTR_DIR"/pgo-*.profraw
"$PROFDATA_BIN" merge -output="$PROFDATA" "$INSTR_DIR"/pgo-*.profraw
echo "merged profile: $PROFDATA"

# ─── Phase 3: OPTIMIZE ──────────────────────────────────────────────────────
banner "Phase 3/3: OPTIMIZED build (-fprofile-instr-use)"
rm -rf "$FINAL_DIR"
cmake -S . -B "$FINAL_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER="$CXX_COMPILER" \
    -DENABLE_PGO=ON -DPGO_MODE=use -DPGO_PROFILE="$PROFDATA" \
    -DBUILD_TESTS=OFF -DBUILD_BENCHMARKS=ON
cmake --build "$FINAL_DIR" --parallel "$NPROC" --target HonestBenchmark

banner "PGO build complete"
echo "  optimized binary: $FINAL_DIR/benchmarks/HonestBenchmark"
echo "  profile used:     $PROFDATA"
