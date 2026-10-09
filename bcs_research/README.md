# BCS Research Extension — Budish–Cramton–Shim on a Verified Engine

Empirical test of the Budish, Cramton & Shim (2015, QJE) arms-race argument:
does a continuous limit order book endogenously generate flash-crash dynamics
from rational HFT behavior alone, with no exogenous shock — and do frequent
batch auctions remove it?

## Why this layout differs from the original plan

The original plan was written in pure Python and assumed a Python simulator
(`SimulationDriver`, `NoiseTrader`, `MarketMaker`, `VPINCalculator`) already
existed. It does not. Those components exist **as C++** (`include/*.h`,
`src/`), and there is no Python binding to the engine. More importantly, the
plan's headline differentiator — running on the **TLA+-verified** matching
engine (`MatchingEngine.tla`: 171,187,419 distinct states, 0 violations) — only holds if the
experiments drive the *real C++ engine*, not a Python re-implementation of it.

So this extension uses a **pybind11 bridge**: Python BCS agents and the
latency/race scheduler sit on top, driving the actual verified C++
`MatchingEngine` underneath.

```
   Python BCS agents  (HFT, adverse-selection MM, noise traders)
          |
   Python latency-aware scheduler   <-- models per-agent observation latency
          |                              and the submission race (BCS mechanism)
          v
   bcs_engine  (pybind11 module)
          |
   C++ MatchingEngine               <-- TLA+-verified, deterministic core
```

The engine has **no notion of agent submission latency** — it is a
deterministic, synchronous matcher. The entire BCS mechanism (latency-
differentiated observation of a fundamental value + a submission-timing race)
therefore lives in the Python scheduler, which orders agent actions by their
computed `submit_at` time before handing them to the engine one at a time.

## Layout

```
bcs_research/
  bindings/        pybind11 module exposing the C++ engine to Python
    engine_bindings.cpp
    CMakeLists.txt
  agents/          fundamental value, HFT, adverse-selection MM (Phase 2)
  simulation/      latency scheduler + harness (Phase 2)
  metrics/         market-quality, welfare, flash-crash detection, bootstrap (Phase 3)
  experiments/     exp1..exp4 sweeps + exp1 hardening (Phase 4)
  hawkes/          vendored univariate-MLE Hawkes calibrator (Phase 5)
  analysis/        Hawkes bridge, phase transition, figures (Phase 5)
  paper/           paper draft (markdown + PDF)
  tests/           pytest suite
  results/         baseline + experiment outputs (JSON) + figures (PNG)
```

## Build & run

```bash
# 1. Engine static lib must exist (build/libOrderMatcher.a). If not:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

# 2. Build the pybind bridge (uses bcs_research/.venv pybind11)
bcs_research/.venv/bin/python bcs_research/build_bridge.py

# 3. Run the round-trip test
bcs_research/.venv/bin/python -m pytest bcs_research/tests -q
```

On Linux, step 1 needs `-DCMAKE_POSITION_INDEPENDENT_CODE=ON -DENABLE_IO_URING=OFF`.
Without the first, ld refuses to link the archive into the Python module. Without
the second, on a box with liburing-dev, the module builds and then fails at import
(`undefined symbol: io_uring_queue_init`). `reproduce.sh` below sets both.

## Reproduce a headline result (one command)

```bash
bcs_research/reproduce.sh     # from a clean checkout: cmake, a C++20 compiler, python3 >= 3.12
```

**The result.** This is the single-maker inversion cell of paper §4.1. The setup
is one market maker, k = 3 HFTs, and a snipe-to-quote ratio of 0.0097: a 20-unit
snipe against the calibrated 2,061-unit quote, inside the band measured on the
BTCUSDT-perp tape. Adding the HFTs changes the maker's PnL by **+1,542.24
[1,278.39, 1,843.60]** (n = 100, 95% bootstrap CI). The maker gains and the noise
traders pay (−2,758.54), so the BCS incidence prediction inverts.

**Why this one.** (a) The paper rests on it. The abstract states it ("a single
maker inverts the prediction and profits"), and it is the M = 1 point that
§4.2's maker-count sign change is measured from. (b) It is not one of the results
the review demotes: Experiment 1's rent test, the Hawkes null, the NT-delta
18/82 split, and the "136 of 144 cells" count. (c) It is cheap. One cell is 200
engine runs, so it runs at the full published n = 100 with no reduced-seed
stand-in.

**What the script does.** Everything goes into `build-bcs-repro/`
(gitignored). The script never reads or writes `build/` or `bcs_research/build/`.

1. Builds `libOrderMatcher.a` with the root CMake: Release,
   `ENABLE_ASSERTIONS=ON`, `ENABLE_IO_URING=OFF`, `-fPIC`.
2. Creates a venv and installs `requirements.lock`.
3. Builds the pybind bridge against that library.
4. Fails if any engine or bridge translation unit would compile with `NDEBUG` in
   effect, or with an `OB_*` feature macro the bridge cannot see. Either one is
   the ODR trap described in `bindings/CMakeLists.txt`: the module links and
   then crashes or returns garbage.
5. Re-runs the cell with `exp6_incidence_surface.run_cell`, then checks it
   against `results/experiments/exp6_incidence_surface.json`. Every stored
   statistic must match: mean, CI bounds, std and n for all 16 metrics. The
   verdict must be `maker_gains`. It then runs `exp7_3d_surface.verify_nesting()`,
   which checks that the 3-D surface's M = 1 column reproduces three exp6 cells.

**Tolerance.** Every compared float must satisfy |reproduced − stored| ≤ 1e-6. The
values are O(10²–10⁵) and published to the unit, so the tolerance only absorbs
last-bit libm differences between platforms. The script reports bit-exactness
separately. Both checks were bit-exact on every platform below.

| Where (from clean, incl. engine build + venv) | Toolchain | Wall time |
| --- | --- | --- |
| Apple-silicon laptop, macOS | Apple clang, CPython 3.13.0 | 79 s |
| `ubuntu:24.04` container, arm64 (OrbStack) | GCC 13.3, CPython 3.13.16 | 84 s |
| `ubuntu:24.04` container, amd64 under Rosetta | GCC 13.3, CPython 3.13.16 | 151 s |

Overrides: `PYTHON` (base interpreter for the venv), `BCS_REPRO_DIR`, `JOBS`.

**Seed policy.** Every experiment driver takes `n_seeds` and runs seeds
`1..n_seeds`. Nothing draws from an unseeded generator. Each source of randomness
in a run has its own numpy `default_rng` stream, derived from the run seed `s`:

- fundamental value: `s`
- noise trader `i`: `1000·s + i`
- HFT `j`: `2000·s + j`

The treatment and baseline arms share `s`, so they see the same fundamental path
and noise flow, and every delta is seed-matched. Bootstrap CIs resample with
`seed=0` (`boot_seed`). Agent constructors default to `seed=0`, never `None`.
Python dependencies are pinned in full in `requirements.lock`; `requirements.txt`
keeps only the direct ones.

**CI.** `.github/workflows/bcs-research.yml` runs on PRs and pushes that touch
`bcs_research/**`, `src/**`, `include/**` or `CMakeLists.txt`, and on demand. On
`ubuntu-latest` with GCC and CPython 3.13, it runs `reproduce.sh` in full (no
smoke mode is needed at this cost). It then runs `pytest bcs_research/tests`
against the module that build produced.

The numerical nesting check is `verify_nesting()`, called from `reproduce.sh`.
The nesting tests under `tests/` (`test_exp7_3d_surface.py`, `test_exp6_incidence.py`)
pin only the configuration the nest relies on: M = 1 resolves to the single
3-tick maker, and the grid contains the published cells. They run no
simulations.

## Results (summary)

Full writeup with figures, tables, and 95% bootstrap CIs in
[`paper/bcs_paper_draft.md`](paper/bcs_paper_draft.md) (PDF alongside). Headlines,
stated with their honest nuance:

- **Exp 1 — welfare transfer.** With HFTs present, HFT rent of $45.59 [25.65,
  70.04] is a near-exact zero-sum transfer from the market maker (the zero-sum
  residual closes to machine zero through the verified engine); liquidity gaps
  rise from 3.45 to 10.40 (non-overlapping CIs). Holds across all nine cells of
  a 3x3 sensitivity x decay calibration.
- **Exp 2 — latency scaling.** Rent rises ~linearly at ~$10.80 per tick of MM
  staleness; a phase transition is *not* supported (dBIC favors the linear fit).
- **Exp 3 — HFT competition.** Per-HFT rent dissipates as ~1/k while *total*
  rent stays flat (~$45.5); the marginal social cost surfaces as rising
  fragility (liquidity gaps ~14x by 21 HFTs), not rising rent.
- **Exp 4 — batch auctions.** Market-maker welfare recovers monotonically away
  from the degenerate single-tick interval (~95% recovered by interval 500);
  the parallel HFT-rent reduction is suggestive but noisy (wide CIs).
- **Exp 5 — Hawkes bridge (preliminary).** Pilot is directionally consistent
  but not significant; treated as scaffolding pending the full run.

## Status

- [x] **Foundation:** pybind11 bridge driving the real engine; Python->C++
      round-trip proven (`tests/test_engine_roundtrip.py`).
- [x] Phase 1: baseline metrics on the existing agents.
- [x] Phase 2: fundamental value, HFT agent, adverse-selection MM, scheduler.
- [x] Phase 3: metrics (welfare decomposition, lagged Kyle's lambda,
      liquidity-gap + flash-crash detectors, bootstrap CIs).
- [x] Phase 4: experiments 1-4 + Exp 1 hardening (n=20 CIs, 3x3 calibration
      robustness, baseline-gap characterization).
- [~] Phase 5: Hawkes bridge — vendored univariate-MLE calibrator + 5-sim pilot
      (preliminary, not significant); full 50-sim run + real-data leg pending
      AlphaForge Phase 1.
- [x] Writeup: full paper draft (`paper/bcs_paper_draft.md`, + PDF) — Abstract
      through Conclusion.

126 pytest tests green across 17 test files (`bcs_research/.venv/bin/python -m
pytest bcs_research/tests -q`).
