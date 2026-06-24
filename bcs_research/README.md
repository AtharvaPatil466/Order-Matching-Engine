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
engine (`MatchingEngine.tla`: 454M states, 0 violations) — only holds if the
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
  metrics/         market-quality, welfare, flash-crash detection (Phase 3)
  experiments/     exp1..exp4 parameter sweeps (Phase 4)
  analysis/        Hawkes bridge, phase transition, plots (Phase 5)
  tests/           pytest suite
  results/         baseline + experiment outputs
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

## Status

- [x] **Foundation:** pybind11 bridge driving the real engine; Python->C++
      round-trip proven (`tests/test_engine_roundtrip.py`).
- [ ] Phase 1: baseline metrics on the existing agents.
- [ ] Phase 2: fundamental value, HFT agent, adverse-selection MM, scheduler.
- [ ] Phase 3: metrics + flash-crash detector.
- [ ] Phase 4: experiments 1-4.
- [ ] Phase 5: Hawkes bridge.
