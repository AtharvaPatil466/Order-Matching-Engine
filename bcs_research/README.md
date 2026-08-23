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
