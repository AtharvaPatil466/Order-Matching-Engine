#!/usr/bin/env bash
# One-command reproduction of the paper's single-maker inversion cell (§4.1),
# from a clean checkout:
#
#   bcs_research/reproduce.sh
#
# Builds the engine library and the pybind bridge from scratch into a fresh
# directory, re-runs ONE published cell at its pinned seeds, and compares every
# statistic against results/experiments/exp6_incidence_surface.json. Then runs
# the M=1 nesting check (exp7_3d_surface.verify_nesting) against the same file.
# Exits non-zero on any build failure, flag drift, or numeric mismatch.
#
# Env overrides:
#   PYTHON          base interpreter for the venv (default python3; >= 3.12)
#   BCS_REPRO_DIR   build/venv directory (default <repo>/build-bcs-repro)
#   JOBS            build parallelism (default 4)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="${BCS_REPRO_DIR:-$ROOT/build-bcs-repro}"
PYTHON="${PYTHON:-python3}"
JOBS="${JOBS:-4}"
START=$SECONDS

echo "==> [1/5] engine library -> $OUT/engine"
# ENABLE_ASSERTIONS=ON is what the bridge is compiled to match (-UNDEBUG on
# both sides). ENABLE_IO_URING=OFF because the bridge links the .a by path and
# so never sees OrderMatcher's PUBLIC usage requirements: on a Linux box with
# liburing-dev the module builds, then fails at import with "undefined symbol:
# io_uring_queue_init", and OB_HAVE_LIBURING changes Journal.h (included by
# MatchingEngine.h) on one side of the boundary only. Step 4 checks NDEBUG and
# these feature macros.
# POSITION_INDEPENDENT_CODE because the .a is linked into a shared object (the
# Python module): macOS is PIC by default, Linux is not, and ld refuses the
# archive with "recompile with -fPIC".
cmake -S "$ROOT" -B "$OUT/engine" \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_ASSERTIONS=ON \
    -DENABLE_IO_URING=OFF \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_TESTS=OFF -DBUILD_BENCHMARKS=OFF -DBUILD_TOOLS=OFF \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build "$OUT/engine" --target OrderMatcher --parallel "$JOBS"

echo "==> [2/5] python venv -> $OUT/venv (requirements.lock)"
"$PYTHON" -m venv "$OUT/venv"
PY="$OUT/venv/bin/python"
"$PY" -m pip install --quiet --disable-pip-version-check -r "$HERE/requirements.lock"

echo "==> [3/5] pybind bridge -> $OUT/bridge"
# Same configure as build_bridge.py, but into $OUT and against the library
# built above — never bcs_research/build/ or the root build/.
cmake -S "$HERE/bindings" -B "$OUT/bridge" \
    -DCMAKE_BUILD_TYPE=Release \
    -Dpybind11_DIR="$("$PY" -m pybind11 --cmakedir)" \
    -DPython_EXECUTABLE="$PY" -DPYTHON_EXECUTABLE="$PY" \
    -DORDERMATCHER_LIB="$OUT/engine/libOrderMatcher.a" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build "$OUT/bridge" --parallel "$JOBS"

echo "==> [4/5] flag check: engine and bridge agree on NDEBUG and feature macros"
"$PY" - "$OUT/engine/compile_commands.json" "$OUT/bridge/compile_commands.json" <<'PYEOF'
import json, shlex, sys

# A TU that links libOrderMatcher.a with NDEBUG defined compiles the engine's
# inline header functions with assert() stripped while the .a has them live:
# an ODR violation that crashes on teardown or silently rejects every order.
# Feature macros defined PUBLIC on OrderMatcher never reach the bridge (it links
# the .a by path), so none may be on. OB_GIT_SHA/OB_BUILD_TIME are metadata.
HARMLESS = {"OB_GIT_SHA", "OB_BUILD_TIME"}
bad = []
for path in sys.argv[1:]:
    for e in json.load(open(path)):
        args = e.get("arguments") or shlex.split(e["command"])
        nd = [a for a in args if a in ("-DNDEBUG", "-UNDEBUG")]
        if not nd or nd[-1] != "-UNDEBUG":
            bad.append(f"{e['file']}: NDEBUG not undone (last of {nd or 'none'})")
        macros = {a[2:].split("=")[0] for a in args if a.startswith("-DOB_")}
        if macros - HARMLESS:
            bad.append(f"{e['file']}: feature macros the bridge cannot see: {sorted(macros - HARMLESS)}")
if bad:
    sys.exit("FLAG DRIFT — engine and bridge would disagree:\n  " + "\n  ".join(bad))
print("    ok: every TU compiles with -UNDEBUG last and no OB_* feature macros")
PYEOF

echo "==> [5/5] headline cell + M=1 nesting check (seeds 1..100, bootstrap seed 0)"
BCS_ROOT="$HERE" BRIDGE_DIR="$OUT/bridge" PYTHONPATH="$OUT/bridge" "$PY" - <<'PYEOF'
import json, os, sys
from pathlib import Path

# Import the module built above BEFORE any experiment file runs: they all
# sys.path.insert(0, bcs_research/build), which on a dev box holds a different
# (possibly stale) bcs_engine.
import bcs_engine
bridge = Path(os.environ["BRIDGE_DIR"]).resolve()
assert Path(bcs_engine.__file__).resolve().parent == bridge, \
    f"imported {bcs_engine.__file__}, expected a module from {bridge}"

root = Path(os.environ["BCS_ROOT"])
sys.path.insert(0, str(root / "experiments"))
from exp6_incidence_surface import (CFG, baseline_rows, calibrated_cfg,  # noqa: E402
                                    classify_incidence, run_cell)
from exp7_3d_surface import verify_nesting                               # noqa: E402

# Absolute tolerance on every compared float. The cell's values are O(1e2-1e5)
# currency units and the paper reports them to the unit, so 1e-6 is far below
# anything published; it exists only to absorb last-bit libm differences across
# platforms. Bit-exactness is reported separately.
TOL = 1e-6
QTY, K, N, BOOT = 20, 3, 100, 0     # ratio 20/2061 = 0.0097, 3 HFTs

stored = json.loads((root / "results/experiments/exp6_incidence_surface.json").read_text())
assert stored["n_seeds"] == N, stored["n_seeds"]
want = next(c for r in stored["rows"] if r["hft_qty"] == QTY
            for c in r["cells"] if c["n_hfts"] == K)

cfg = {**CFG, **calibrated_cfg()}
got = run_cell(QTY, K, cfg, baseline_rows(cfg, N), N, BOOT)

failures, exact = [], True
for grp in ("treatment_ci", "delta_ci"):
    for metric, stats in want[grp].items():
        for stat, w in stats.items():
            g = got[grp][metric][stat]
            exact = exact and g == w
            if abs(g - w) > TOL:
                failures.append(f"{grp}.{metric}.{stat}: got {g!r}, stored {w!r}")

mm = got["delta_ci"]["mm_pnl"]
verdict = classify_incidence(got)
print(f"    headline: ratio {QTY / cfg['quote_qty']:.4f}, k={K}, n={N}: "
      f"maker dPnL {mm['mean']:+.2f} [{mm['lower']:.2f}, {mm['upper']:.2f}] -> {verdict}")
print(f"    stored  : maker dPnL {want['delta_ci']['mm_pnl']['mean']:+.2f} "
      f"[{want['delta_ci']['mm_pnl']['lower']:.2f}, {want['delta_ci']['mm_pnl']['upper']:.2f}] "
      f"-> {classify_incidence(want)}")
print(f"    headline bit-exact: {exact}")
if verdict != "maker_gains":
    failures.append(f"verdict {verdict}, the paper's claim is maker_gains")

nest = verify_nesting(cfg, BOOT)
for p in nest["probes"]:
    for metric, d in p["abs_diffs"].items():
        if d > TOL:
            failures.append(f"nesting qty={p['hft_qty']} k={p['n_hfts']} {metric}: |diff| {d!r}")
print(f"    nesting bit-exact: {nest['all_exact']}")

if failures:
    sys.exit("MISMATCH against stored results:\n  " + "\n  ".join(failures))
print(f"    ok: headline and nesting reproduce within {TOL:g}")
PYEOF

echo "REPRODUCED in $((SECONDS - START))s"
