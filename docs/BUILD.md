# Build Guide

## Standard build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Release enables `-O3` plus an architecture target chosen by `OB_ARCH`
(see `CMakeLists.txt`):

| `OB_ARCH` | Effect | Used by |
|---|---|---|
| `native` (default) | `-march=native -mtune=native` | local builds and every benchmark figure in `BENCHMARKS.md` |
| a baseline, e.g. `x86-64-v3` | `-march=<value>`, no `-mtune` | `Dockerfile` and the chaos image |
| empty | no architecture flags | portability checks |

**The default is `native` on purpose, and the deployment images do not use it.**
A Release build pinned to the builder's CPU is fine for benchmarking and wrong
for a container: shipped to an older host it gives SIGILL rather than degrading.
So the Dockerfile pins `x86-64-v3` on x86 (AVX2 era, 2013+) and `armv8-a` on
arm64 — the arm64 branch exists because `-march=x86-64-v3` is rejected outright
by an ARM compiler, and pinning a deployment build must not make the local build
impossible.

Override with `-DOB_ARCH=<value>` at configure time, or `--build-arg OB_ARCH=`
for the image.

Note `-Wno-error=invalid-feature-combination` is applied only under
`OB_ARCH=native`: its cause is `-march=native` on newer silicon, so a pinned
build carries no arch-related warning demotions and `-Werror` is absolute there.

### Notable CMake options (all opt-in, default OFF unless noted)

| Option | Default | Effect |
|--------|---------|--------|
| `ENABLE_ASSERTIONS` | ON | Keep `assert()` live (`-UNDEBUG`) |
| `ENABLE_LTO` | OFF | Link-time optimization in Release |
| `ENABLE_SANITIZERS` | OFF | ASan + UBSan |
| `ENABLE_THREAD_SANITIZER` | OFF | TSan |
| `ENABLE_FAULT_INJECTION` | OFF | Compile fault-injection points |
| `ENABLE_IO_URING` | ON | io_uring journal write path when liburing is found (Linux); no-op otherwise |
| `ENABLE_PGO` | OFF | Profile-guided optimization (see below) |

---

## Profile-Guided Optimization (PGO)

Clang IR-based PGO (`-fprofile-instr-generate` / `-fprofile-instr-use`, **not**
GCC's `-fprofile-generate`). PGO feeds the compiler real branch-taken
statistics from an instrumented run, which can beat `-O2`/`-O3` static
heuristics on branch-heavy code. Requires **Clang** and `llvm-profdata`.

### One-shot pipeline

```bash
bash scripts/pgo_build.sh
```

Three phases:

1. **INSTRUMENT** — build `HonestBenchmark` with `-fprofile-instr-generate`
   (`ENABLE_PGO=ON PGO_MODE=generate`) into `build-pgo-instr/`.
2. **PROFILE** — run it (`--orders 50000 --seed 42`), emitting `pgo-<pid>.profraw`,
   then `llvm-profdata merge` → `build-pgo-instr/default.profdata`.
3. **OPTIMIZE** — rebuild with `-fprofile-instr-use=<profdata>`
   (`PGO_MODE=use PGO_PROFILE=<abs path>`) into `build-pgo/`.

**Output binary:** `build-pgo/benchmarks/HonestBenchmark` (plus a
PGO-optimized `libOrderMatcher` in `build-pgo/`).

### Manual invocation

```bash
# instrument
cmake -B build-pgo-instr -DCMAKE_BUILD_TYPE=Release \
      -DENABLE_PGO=ON -DPGO_MODE=generate
cmake --build build-pgo-instr -j --target HonestBenchmark
LLVM_PROFILE_FILE="$PWD/build-pgo-instr/pgo-%p.profraw" \
    ./build-pgo-instr/benchmarks/HonestBenchmark --orders 50000 --seed 42
llvm-profdata merge -o build-pgo-instr/default.profdata build-pgo-instr/pgo-*.profraw
# optimize
cmake -B build-pgo -DCMAKE_BUILD_TYPE=Release -DENABLE_PGO=ON -DPGO_MODE=use \
      -DPGO_PROFILE="$PWD/build-pgo-instr/default.profdata"
cmake --build build-pgo -j --target HonestBenchmark
```

### Notes / caveats

- **Profile representativeness:** the profile reflects the HonestBenchmark
  50K/seed=42 order flow — representative for that benchmark, not a guarantee
  for arbitrary production flow. Re-profile with a workload that matches your
  real traffic if that differs materially.
- **`-Werror` interaction:** the Release build uses `-Werror`. In use-mode
  Clang emits `-Wprofile-instr-unprofiled` (functions the benchmark never
  exercised — gateways, research, etc.) and `-Wprofile-instr-out-of-date`
  (source drift vs. profile). CMake demotes exactly those two to warnings so
  PGO doesn't break the build; other warnings still fail as before.
- **Profile data is not committed:** `*.profraw` / `*.profdata` are gitignored;
  regenerate via the pipeline.
- **Cross-platform:** the pipeline runs on macOS/ARM and Linux/x86 (Clang).
  Benchmark numbers are only authoritative on the x86 bare-metal target
  (AWS c6in.metal); the macOS run is for verifying the pipeline works.
