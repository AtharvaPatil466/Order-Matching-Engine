#!/usr/bin/env bash
# Run a libFuzzer target. Auto-detects a clang that supports
# -fsanitize=fuzzer (Apple Clang doesn't ship with it; Homebrew LLVM does).
#
# Usage:
#   ./fuzz/run_libfuzzer.sh                    # fuzz_fix_parser, default
#   ./fuzz/run_libfuzzer.sh framer             # fuzz_fix_framer
#   ./fuzz/run_libfuzzer.sh parser 60          # 60-second timeout
#   ./fuzz/run_libfuzzer.sh parser 0 corpus/   # use a specific corpus dir
#
# After the run, crashes (if any) are written to fuzz/crashes/<target>/
# and can be replayed with:
#   ./build-fuzz/fuzz/fuzz_<target>_replay fuzz/crashes/<target>/<file>

set -euo pipefail

cd "$(dirname "$0")/.."
PROJECT_ROOT=$(pwd)
BUILD_DIR="$PROJECT_ROOT/build-libfuzzer"

TARGET_SHORT="${1:-parser}"
TIMEOUT="${2:-30}"
CORPUS="${3:-fuzz/corpus}"

case "$TARGET_SHORT" in
    parser)    TARGET=fuzz_fix_parser ;;
    framer)    TARGET=fuzz_fix_framer ;;
    roundtrip) TARGET=fuzz_fix_roundtrip ;;
    *)
        echo "unknown target: $TARGET_SHORT (parser|framer|roundtrip)"
        exit 2
        ;;
esac

# --- Locate a libFuzzer-capable clang -------------------------------------

find_clang() {
    # Prefer Homebrew LLVM since Apple Clang lacks -fsanitize=fuzzer.
    for cand in \
        "$(brew --prefix llvm 2>/dev/null)/bin/clang++" \
        "/opt/homebrew/opt/llvm/bin/clang++" \
        "/usr/local/opt/llvm/bin/clang++" \
        "$(which clang++ 2>/dev/null)"
    do
        if [ -n "$cand" ] && [ -x "$cand" ]; then
            if echo 'int main(){}' | "$cand" -x c++ -fsanitize=fuzzer - -o /tmp/.fuzzer_probe$$ 2>/dev/null; then
                rm -f /tmp/.fuzzer_probe$$
                echo "$cand"
                return 0
            fi
        fi
    done
    return 1
}

CLANG=$(find_clang || true)
if [ -z "$CLANG" ]; then
    cat <<'MSG' >&2
ERROR: no clang++ supporting -fsanitize=fuzzer found.
On macOS, install Homebrew LLVM:
  brew install llvm
Then re-run this script.
MSG
    exit 3
fi
echo "[libFuzzer] using clang: $CLANG"

# --- Configure and build (only on first run or if clang changed) ----------

CONFIGURE=0
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    CONFIGURE=1
elif ! grep -q "CMAKE_CXX_COMPILER:FILEPATH=$CLANG" "$BUILD_DIR/CMakeCache.txt"; then
    echo "[libFuzzer] compiler changed; reconfiguring"
    rm -rf "$BUILD_DIR"
    CONFIGURE=1
fi

if [ "$CONFIGURE" = "1" ]; then
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    cmake \
        -DCMAKE_CXX_COMPILER="$CLANG" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DBUILD_FUZZERS=ON \
        -DBUILD_TESTS=OFF \
        -DBUILD_BENCHMARKS=OFF \
        "$PROJECT_ROOT"
    cd "$PROJECT_ROOT"
fi

cmake --build "$BUILD_DIR" -j --target "$TARGET"

# --- Run -------------------------------------------------------------------

mkdir -p "fuzz/crashes/$TARGET_SHORT"

ARGS=(
    -artifact_prefix="fuzz/crashes/$TARGET_SHORT/"
    -print_final_stats=1
    -timeout=10
)
if [ "$TIMEOUT" -gt 0 ]; then
    ARGS+=("-max_total_time=$TIMEOUT")
fi

echo "[libFuzzer] running $TARGET (timeout=${TIMEOUT}s, corpus=$CORPUS)"
"$BUILD_DIR/fuzz/$TARGET" "${ARGS[@]}" "$CORPUS"
