# Benchmark Methodology & Results

> **Authoritative platform**: AWS c6in.metal — dual Intel Xeon Platinum 8375C
> @ 2.90GHz, 64 physical cores (hyperthreading disabled via `nosmt`),
> Ubuntu 26.04, Clang C++20 `-O3 -march=native`, `numactl --cpunodebind=0
> --membind=0`. 50,000 orders, seed=42, 5 stable runs, commit `d2e688c`.
> **Tests**: 426/426 passing.
> **Reference platform**: Apple M3 Pro (ARM64) dev machine — quoted separately
> below and clearly labelled as a *dev-machine reference only*, NOT an SLA.
> Throughput is wall-clock and load-sensitive; P50 is the stable per-op number.

## TL;DR

| Claim | Evidence | Status |
|-------|----------|--------|
| Core matching (x86, PGO): **237 ns** P50 · 3.10M ops/s | Clang IR-PGO on the seed=42 workload (AWS c6in.metal); 261 ns non-PGO baseline | ✅ Verified |
| Full-stack with journal (x86): **615 ns** P50 | GroupCommit batch=64, async io_uring ack on EBS | ✅ Verified |
| Safety invariants | TLC: 171,187,419 distinct states, 0 violations (matching/cross layer modeled; `MatchingEngine4.cfg`, MaxOrders=4) | ✅ Verified |
| Shadow mode | FIFO violation detected via trade divergence | ✅ Validated |
| **SBE encode: 1.0 ns/op (1015 M ops/s)** | Pure codec microbench, no engine | ✅ Measured |
| **SBE 110× faster than OUCH encode** | Binary vs ASCII-decimal field formatting | ✅ Measured |
| Coordinated omission is corrected, not ignored | `CoordinatedOmissionBenchmark` runs open-loop at a fixed offered rate and reports CO-naive *and* CO-corrected | ✅ Measured (ARM only) |
| Cold-path cost ≈ 10× warm at P50 | `ColdCacheBenchmark`, working set evicted before each timed op | ✅ Measured (ARM only) |
| Sustained per-window latency | `SustainedLoadTest` latency columns are cumulative, not per-window — harness defect | ❌ Unmeasured |

## The four benchmarks

Four benchmarks characterize the engine. They measure different things and are
expected to disagree; quoting any one of them alone misrepresents the engine.

| Benchmark | Question | Load model | Workload |
|---|---|---|---|
| `HonestBenchmark` | Floor on the matching path under favourable conditions | Closed-loop | 50K orders, seed=42, **100% fill**, no cancels |
| `RealisticFlowBenchmark` | Cost on venue-shaped flow | Closed-loop | 500K events, 44% cancel / 46% new / 8% IOC / 2% modify |
| `CoordinatedOmissionBenchmark` | What a client sees when arrivals do not wait for us | **Open-loop**, paced | `RealisticWorkload` New+Cancel stream at a fixed offered rate |
| `ColdCacheBenchmark` | First-touch cost after an idle gap | Closed-loop | Same `RealisticWorkload` stream, working set evicted before each timed op |

The last two share `benchmarks/RealisticWorkload.h` — a New+Cancel-only stream
(15% marketable, 65% of resting orders scheduled for cancel, power-law sizes,
20 participants). That is **not** `RealisticFlowBenchmark`'s workload, which
uses its own inline generator with IOC and modify events and 8 participants.
The four benchmarks' absolute numbers are not directly comparable to each other.

**`HonestBenchmark`** (seed=42, 50K orders, **100.0% fill rate — 50,000 of
50,000, confirmed in its own output**) is the controlled reproducible baseline.
It measures the best-case hot path — a dense resting book, predictable price
clustering, a fill on every new order — under a **closed-loop** harness that
issues the next order only after the previous one returns. **P50 237 ns (PGO),
P99 910 ns, ratio 3.8× on x86.** This is a *floor*, not an expected operating
number: 100% fill is the least cancel-like order flow that exists, and a
closed-loop harness cannot see coordinated omission. It is legitimate and
reproducible, and it is the flow every figure in the Results section and all
optimization history are measured on.

**`RealisticFlowBenchmark`** (500K events, realized 43.9% cancel / 46.2% new /
7.9% IOC / 2.0% modify, mean resting depth 2,418 orders) models venue-shaped
order flow with a sustaining resting book. No empty-book cancels (0 of
217,256). **P50 208 ns combined, P99 709 ns on ARM** (see the ARM section for
the sample-loss caveat that makes both an upper bound). The cancel P50 is *at*
ARM timer resolution and is not a measurement; x86 TSC resolves it. This
benchmark was rewritten from a prior version that had a structural defect — the
cancel fraction exceeded the new-order fraction, draining the resting pool to
empty and measuring an empty book at venue-shaped labels.

**`CoordinatedOmissionBenchmark`** is the answer to the standard objection that
the two above are closed-loop. It runs **open-loop**: it pins an intended start
time per operation at a fixed offered rate and reports both
`completion − actual_start` (CO-naive, what a closed-loop harness sees) and
`completion − intended_start` (CO-corrected, what a client experiences).
Corrected is the honest number. Results in the ARM section below; **it has never
been run on x86.**

**`ColdCacheBenchmark`** evicts the working set through a 64 MB buffer before
each timed op and reports warm and cold side by side. Results in the ARM
section; **also never run on x86.**

None of the four represents a cancel-heavy **sparse**-book regime
(cancel-to-trade ratios north of 20:1, price levels churn, book depth near
zero). `RealisticFlowBenchmark` is cancel-heavy but its book *sustains* at depth
2,418 by construction. The sparse regime stresses `FlatPriceMap` traversal over
sparse slots, cancel-path hash lookups on recently-consumed IDs, and object pool
churn. It is the planned next workload addition.

## Methodology

All three paths process the **identical** deterministic order stream:
- 50,000 limit orders, random walk mid ± 50 ticks
- 20 participants, quantities 1–100
- Realistic clustering to force matching

Each order is individually timed: `t0 = nowNs()` → operation → `t1 = nowNs()`.

### What IS Measured

| Path | Components |
|------|------------|
| **A** | `OrderBook::addOrder()` — matching, STP, WashTrade, LULD, price-time priority |
| **B** | Path A + `MatchingEngine::submitOrder()` — sequence allocation, rate limiter check |
| **C** | Path B + `Journal` — GroupCommit (batch=64) with `fdatasync` per batch |

### What is NOT Measured

- Network I/O (FIX parsing, TCP/UDP)
- Async queue delay (all paths are synchronous)
- OS scheduling jitter (no `isolcpus`, no core pinning)
- Memory allocator contention (single-threaded)

## Results

### Authoritative — x86 bare metal (AWS c6in.metal, standard Release build)

| Path | What's Included | P50 | P90 | P99 | Throughput |
|------|------------------|----:|----:|----:|-----------:|
| **A** Core matching | OrderBook + STP + WashTrade + LULD | **261 ns** | 620 ns | 1,010 ns | 2.80M ops/s |
| **B** Engine wrapper | + sequence alloc, rate limiter | 269 ns | 620 ns | 1,001 ns | 2.74M ops/s |
| **C** Full-stack journal | + GroupCommit (batch=64, async io_uring ack on EBS) | 615 ns | 1,048 ns | 3,568 ns | 1.28M ops/s |

> **PGO:** Clang IR-based profile-guided optimization (profiled on the seed=42
> HonestBenchmark workload) takes Path A to **P50 237 ns / P99 910 ns /
> 3.10M ops/s** — the headline figure. The table above is the standard (non-PGO)
> Release build.

`perf` (**mis-scoped — see the correction below**, 50K orders seed=42):

| Counter | Per-order mean |
|---|---:|
| IPC | 1.34 |
| Branch-misses / order | 17.8 |
| L1-dcache-misses / order | 152 |
| Instructions / order | 12,638 |

**These figures are withdrawn. The table above is retained only so the
correction has something to point at; do not cite the numbers.**

They were labelled "Path A" and they are not Path A. `scripts/aws_benchmark.sh`
runs `perf stat` around the **entire HonestBenchmark process** — its own echo
line says "counters are whole-run totals" — passing only `--orders 50000
--seed 42`. No path filter, and no `--no-journal`. So one process generates
50,000 orders and then runs warmup plus measured passes of Path A, Path B **and
Path C, including Path C's `fdatasync`**. Dividing that by 50,000 charges the
journal path's syscalls and cache traffic to core matching.

THE PREVIOUS PARAGRAPH HERE WAS A RATIONALISATION, AND IT IS WORTH RECORDING
RATHER THAN DELETING. It explained the gap as the per-order mean being pulled
above the P50 by "the tail of multi-level sweep events" — infrequent but
expensive matches touching extra `FlatPriceMap` slots and `IntrusiveList`
nodes. That is a real phenomenon and it is not the cause here. It cannot be:
12,638 instructions at IPC 1.34 is ~9,430 cycles, ~3.25 µs at 2.90 GHz, against
a 261 ns P50 — more than twelvefold. No plausible tail mass moves a mean twelve
times its median. The honest reading is that the explanation was reasoned
backwards from a number that had already been accepted, which is exactly the
failure mode the rest of this document exists to avoid.

A correctly-scoped rerun needs `perf stat` around the measured region alone.
`HonestBenchmark --only a` now exists for that, and the run needs x86 Linux —
the dev box is Apple Silicon and has no `perf`.

The 261 ns P50 is structurally bound (pointer-chasing L1 misses + data-dependent
branch mispredicts + Spectre eIBRS), not instruction-bound. This cycle's
branchless price-cross shaved 10 ns P50 / 62 ns P99 (see Optimization History
below); next-order prefetch and the price-level arena allocator were both
implemented and reverted as net-negative on this 100%-fill flow.

### Reference — Apple Silicon ARM64 (M3 Pro dev machine, NOT an SLA)

All figures below are from a from-scratch Release build (`-O3 -march=native`,
`-DBUILD_BENCHMARKS=ON`) on macOS 26.6 / Apple M3 Pro. They are **indicative
only**; x86 is the source of truth.

#### Measurement floor — read this before any ARM number

`bench::nowNs()` is `std::chrono::steady_clock`, which on this M3 Pro ticks at
**41 ns (smallest non-zero delta; 42 ns median)**. Measured directly: of
2,000,000 back-to-back `nowNs()` calls, **41.7% returned the same value as their
predecessor**. Two consequences apply to every ARM figure here:

1. **Every ARM latency is quantized to a ~41.67 ns grid.** A reported 208 ns is
   5 ticks, ±1 tick (±20%). A reported 42 ns is *one* tick — not a measurement,
   only a statement that the op finished inside one clock period.
2. **Sub-tick ops are counted — FIXED; the ARM tables below predate it.**
   `recordInterval()` used to read `if (end > start) record(...)`, so an op
   faster than one tick left no sample and every percentile was biased upward —
   the fastest samples are exactly the ones that vanished (22-32% on cancel).
   Both recorders now record a sub-tick span as 0 and report the count, and a
   percentile inside that band prints `<tick` rather than a number. `end <
   start` stays unrecorded and is counted separately as a clock anomaly.
   Pinned by `tests/LatencyTrackerIntervalTest.cpp`. The tables below were
   measured before the fix and are left as measured; a like-for-like re-run
   recovered 19,426 cancel samples at 200K events and moved the cancel mean
   from 174 ns to 135 ns.

An x86 TSC tick on a 2.9 GHz part is ~0.34 ns — roughly 120× finer than this
box's 41.67 ns — so neither problem arises there. Any ARM number within a small
multiple of 42 ns should be read as quantization, not measurement.

#### `HonestBenchmark` — closed-loop, 100% fill, seed=42, 50K orders

| Path | P50 | P90 | P99 | P99.9 | Max | Throughput |
|------|----:|----:|----:|------:|----:|-----------:|
| A Core matching | 250 ns | 542 ns | 1,166 ns | 1,709 ns | 91,917 ns | 2.76M ops/s |
| B Engine wrapper | 250 ns | 500 ns | 791 ns | 1,208 ns | 43,292 ns | 2.95M ops/s |
| C Full-stack journal | 1,750 ns | 4,096 ns | 1,761,280 ns | 3,899,392 ns | 10,773,458 ns | 24,603 ops/s |

Fill rate **100.0% on all three paths (50,000 of 50,000)**. Paths A and B report
an identical 250 ns P50 (6 ticks) because the wrapper overhead is smaller than
one tick here; x86 measures it at 8 ns (269 − 261). Path C is a macOS/APFS
`fdatasync` artifact, **not structural** — 615 ns on Linux x86 with the async
io_uring ack.

A previous revision of this table reported ~125 ns for Paths A and B. That
figure did not reproduce and is not retained.

#### `RealisticFlowBenchmark` — closed-loop, cancel-heavy, 500K events, seed=42

Realized mix over 495,000 timed ops: **43.9% cancel (217,256) / 46.2% new
(228,697) / 7.9% IOC (39,328, 41.3% of them filled) / 2.0% modify (9,719)**.
Mean resting depth **2,418 orders**, **0 empty-book no-op cancels**.

| Path | Samples / timed | P50 | P90 | P99 | P99.9 | Max |
|---|---|----:|----:|----:|------:|----:|
| Cancel | 159,827 / 217,256 | *42 ns — one tick, at the floor* | 375 ns | 667 ns | 792 ns | 18,208 ns |
| New | 228,693 / 228,697 | 208 ns | 583 ns | 708 ns | 1,416 ns | 8,583 ns |
| IOC | 38,389 / 39,328 | 208 ns | 375 ns | 791 ns | 1,792 ns | 4,792 ns |
| Modify | 9,719 / 9,719 | 292 ns | 667 ns | 1,125 ns | 1,750 ns | 2,625 ns |
| **Combined** | 436,628 / 495,000 | **208 ns** | 458 ns | 709 ns | 1,250 ns | 18,208 ns |

**The cancel P50 of 42 ns is not a measurement** — it is exactly one tick, and
means only that the median cancel completes in under 42 ns. **26.4% of cancels
(57,429 of 217,256) finished inside one tick and were dropped from the
histogram**, so the cancel percentiles cover the slower 73.6% and are biased
upward. The same loss costs the combined row 58,372 of 495,000 samples (11.8%),
making the combined P50 of 208 ns an upper bound rather than a centre. Cancel is
the fastest path in the engine and the one this box is least able to measure.

The 18 µs cancel maximum against a 792 ns P99.9 is macOS scheduler preemption,
not engine work — no `isolcpus`, no core pinning.

#### `CoordinatedOmissionBenchmark` — open-loop rate sweep

A closed-loop harness issues its next request only after the previous returns,
so when the system stalls the harness stalls with it and the stall is never
charged to any sample: the requests that should show the worst latency are the
ones never issued. This benchmark runs open-loop at a fixed offered rate,
pinning `intended[i] = t0 + i / rate`, and reports both **CO-naive**
(`completion − actual_start`) and **CO-corrected**
(`completion − intended_start`). **Corrected is the honest number** — a client
sends at its own cadence and does not wait for the engine, so time an order
spends queued behind a late predecessor is latency the client really pays and
the naive number discards.

200K new orders → 305,758 measured ops, seed=7:

| Offered rate | Backlogged | Naive P99 | Corrected P99 | Naive P99.9 | Corrected P99.9 |
|---|---:|---:|---:|---:|---:|
| 100K/s | 0.0% (131) | 1,166 ns | 1,250 ns | 4,192 ns | 5,280 ns |
| 500K/s | 0.2% (616) | 834 ns | 1,000 ns | 2,334 ns | 5,536 ns |
| 1M/s | 0.4% (1,261) | 750 ns | 1,000 ns | 1,958 ns | 6,016 ns |
| 2M/s | 6.6% – 54% | — | *unstable* | — | — |
| 3M/s | 5.5% – 99.9% | — | *unstable* | — | — |
| 4M/s | 42.7% | 583 ns | **27,136 ns** | 1,458 ns | **56,576 ns** |
| 6M/s | 100.0% | 500 ns | **16,777,216 ns** | 1,333 ns | **17,039,360 ns** |

**Below capacity (100K–1M/s)** the two tables nearly coincide at P50 and P99;
divergence is confined to P99.9 and beyond, where corrected already exceeds
naive by **1.26× at 100K/s, 2.37× at 500K/s and 3.07× at 1M/s** — the
understatement grows with load. Reproducible: three repeats at 1M/s gave
0.2%/0.5%/0.6% backlog, naive P99 666/792/834 ns, corrected P99
750/1,208/1,208 ns.

**At and past capacity the corrected tail explodes while the naive tail does
not.** At 6M/s naive P99 *improves* to 500 ns while corrected P99 is 16.8 ms — a
factor of 33,000, with 100% of samples backlogged. A closed-loop harness on a
saturated engine reports its best-looking numbers precisely because it has
stopped measuring anything but service time.

**The 2M–3M/s knee is not reproducible on this box and is not reported as a
number.** Three repeats at 2M/s gave 42.0%/48.5%/54.2% backlog with corrected
P99 from 2.6 ms to 10.4 ms; three at 3M/s gave 83.4%/90.1%/99.9%; initial runs
at each gave 6.6% and 5.5%. Near capacity on a non-isolated macOS box the
run-to-run variance exceeds the effect. **Locating the knee needs x86 with
`isolcpus` and core pinning — unmeasured.** This box establishes only that it
lies between 1M/s (stable, 0.4%) and 4M/s (42.7%).

#### `ColdCacheBenchmark` — warm vs cold, 20K orders → 30,965 ops, seed=7

64 MB eviction buffer streamed before each timed op; the eviction is not timed.

| | Samples | P50 | P90 | P99 | P99.9 | Max | Mean |
|---|---:|---:|---:|---:|---:|---:|---:|
| Warm | 28,258 | 250 ns | 500 ns | 958 ns | 4,832 ns | 36,542 ns | 302 ns |
| Cold | 28,808 | **2,625 ns** | 7,648 ns | 12,416 ns | 34,560 ns | 3,531,875 ns | 3,855 ns |
| Penalty | | +2,375 ns | +7,148 ns | +11,458 ns | +29,728 ns | | |

**Cold costs ≈10× warm at P50.** Both are well clear of the 41 ns floor, so this
comparison is resolvable here even though the magnitudes are not authoritative.
A production tail SLA must budget for the cold number — warm 250 ns applies only
to a continuously busy engine, and the first order after a quiet period is the
one that matters most at a market open or after a halt.

#### `SustainedLoadTest` — throughput and memory only

A 30 s run completed. **Throughput and memory are reported; the latency columns
are defective and are not reproduced.**

- 30,172,625 orders in 30.2 s → **999,987 orders/sec** sustained, 0 rejected
- Resident memory 843,520 KB → 1,186,016 KB (+40.6%)
- Exit code **1** — its own regression gate fired

Two harness defects, not engine defects:

1. **"Per-window" latency is cumulative since engine start.** The test calls
   `engine.getAggregateE2ELatency()` per window; that merges per-thread
   `LatencyTracker`s which are never reset (`src/MatchingEngine.cpp:2361`). The
   tell is a P50 that *decays* monotonically from ~780 ms (window 4) to ~48 µs
   (window 29) — a cumulative histogram diluted by later samples, not a system
   speeding up. Reported P99s sit on exact powers of two (1,073,741,824 ns =
   2³⁰), which are log-linear bucket edges in the second range.
2. **Offered rate is double the requested rate.** `interOrderNs` derives once
   from the global `--rate`, but each of `max(1, numThreads/2)` producers paces
   at that full rate. Defaults (`--threads 4 --rate 500000`) offer 1M/s — which
   is exactly the 999,987/s observed.

The exit-1 verdict is therefore also meaningless: the gate compares the
cumulative aggregate, so one bad early window latches it to failing.
**Sustained per-window latency and degradation over time are unmeasured.**

#### Not yet measured (needs x86)

| Gap | Why |
|---|---|
| True cancel-path P50 | Below the 41 ns ARM tick; needs x86 TSC |
| Engine-wrapper overhead on ARM | Smaller than one tick; x86 measures 8 ns |
| CO knee (saturation rate) | 2M–3M/s not reproducible without `isolcpus`/pinning |
| CO-corrected tail on x86, any rate | `CoordinatedOmissionBenchmark` has never run on x86 |
| `RealisticFlowBenchmark` on x86 | Never run there; all realistic-flow numbers are ARM |
| `ColdCacheBenchmark` on x86 | Never run there |
| instructions/order, IPC, L1d misses | No `perf`/HW counters on Apple Silicon (see withdrawal above) |
| Sustained per-window latency | Harness defective — see above |

The ARM-vs-x86 gap on core matching is microarchitectural (wider OoO window +
stronger branch prediction on pointer-chasing code), not a build-flag or
field-ordering effect.

## Optimization History (latest cycle)

Validated on AWS c6in.metal against the same 50K/seed=42 flow:

| Change | Result |
|--------|--------|
| **Branchless price-cross** | Path A −10 ns P50, −62 ns P99 |
| **Next-order prefetch** | **Stays reverted — re-tested properly, verdict unchanged.** The original basis (L1-dcache-misses 47->152/order) was the mis-scoped whole-process counter corrected in `156acb3`, and the original workload was 100%-fill, where a prefetch cannot pay. Re-measured behind `-DENABLE_MATCH_PREFETCH=ON` over 21 round-robin rounds on BOTH flows, deltas judged by Mann-Whitney U: no benefit anywhere (p 0.43-0.95), and the only separable effect is a **penalty** on RealisticFlow IOC mean (+1.2%, p=0.007) — the one sub-path where the matching loop actually iterates, which is the direction the original mechanical argument predicted. **Residual open question:** neither benchmark does a deep multi-order sweep, the regime where a prefetched node would be consumed; Path A fills ~1 resting order per incoming order and RF's Pareto IOCs mostly consume 1-2. So this shows prefetch does not help *these* flows, not that it cannot help a sweep-heavy one. Settled by: x86 + a sweep-heavy workload under `perf stat` scoped via `--only a`, measuring L1-dcache-load-misses per matching-loop *iteration*. |
| **io_uring async ack (Option 1)** | Path C P99 **−32.8%** (5,312 → 3,568 ns); Path C P50 **+167 ns** — expected, since `submitOrder()` now returns before durability and the completion-reaper thread's overhead surfaces in P50 |
| **Price-level arena allocator** | **Stays reverted — and now measured, not assumed.** Judged on the cancel-heavy flow it was supposed to win on it is 2.5-3.6% WORSE on mean latency (p~0.002) with no tail benefit; on 100%-fill it is +16% mean, -13% throughput, +33% P99, +62% P99.9 (p < 1e-5 throughout). Those effects are 5-400x the clock noise floor, so the verdict does not rest on ARM timer resolution. It is *correct* (523/523 with it on) — just slower. **Also found:** a repeatable ~185us hot-path stall that GROWS with cumulative orders (9.9us at 5K -> 240us at 200K, while the pooled baseline stays flat at ~10us). `OrderArena::activePriceToSlab_` is claimed to be sized so no rehash hits the hot path, but entries are not bounded by slab count: re-affiliating a slab to a new price orphans every earlier price still mapped to it, so orphans accumulate and the map periodically rehashes mid-match. Not carried in the tree — recoverable from `ff55338` (+ `c9e2260` for the SLAB_SIZE override), and that defect is the first thing to fix if anyone revisits. |

## Sustained Load — per-window, measured

`SustainedLoadTest` had two defects that made its latency columns unusable, both
now fixed:

* **Windows were cumulative.** It sampled `getAggregateE2ELatency()` once per
  window, but the engine's per-thread trackers are never reset, so every window
  reported the distribution since process start. The artifact looked like
  warmup — P50 decaying monotonically (780 ms → 48 µs across 30 windows in one
  recorded run) purely because the population kept growing, with P99s landing
  on exact powers of two. `LatencyTracker::deltaFrom` now subtracts the previous
  boundary's snapshot, so each window reports what was actually recorded during
  it. Bucket counts subtract exactly; min/max cannot be recovered that way and
  are cleared rather than carrying a misleading value.
* **The offered rate was multiplied by the producer count.** `interOrderNs` came
  from the full `--rate`, and each of `max(1, threads/2)` producers paced itself
  at it, so the run offered `producers × rate`. At defaults that meant 1M/s
  offered while the header printed 250k/s — and the measured throughput came
  back at ~1M/s, which read as agreement. Now divided by the producer count:
  a 400k/s target measures 400,000/s.

**What sustained load actually looks like** (400k/s, 4 workers, 20 × 1 s
windows, Apple Silicon — indicative, not authoritative):

| Phase | Windows | P50 | P99 | RSS |
| :--- | :--- | --: | --: | :--- |
| Warmup | 0–5 | 2,160 → 10,688 ns | up to 10.4 ms | flat 953 MB |
| Steady | 6–16 | **~790 ns** | 7–11 µs | 953 MB → 1,186 MB, then flat |
| Steady + excursions | 17–19 | ~850 ns | 330–415 µs | flat |

Throughput is flat at 402k/s in every window. **There is no degradation**: the
first ~6 s is allocator and page-fault warmup, after which P50 settles and RSS
stops growing. Periodic P99 excursions into the hundreds of microseconds
continue in steady state and are the real open question here.

**Latency is lower at high offered rate than at low.** ~790 ns P50 at 400k/s
versus ~4,500 ns at 50k/s. At low rate the queues drain between orders and each
one pays cold-cache cost; at saturation the structures stay resident. So any
threshold is only meaningful next to the rate it was measured at, and "slower
under light load" is the expected shape rather than a fault.

These are **end-to-end ingress→completion** figures, dominated by queue
residency rather than service time — not comparable to the Path A service-time
numbers above, and not an SLA. The tool's default `--p99-threshold` of 5 µs is
uncalibrated for this and fires on every window; it is a measurement tool, not
a pass/fail gate, until someone picks a threshold against a stated rate.

## GroupCommit Explained

Path C uses `SyncPolicy::GroupCommit` with `batch_size=64`:
- Entries are buffered in memory until 64 accumulate
- Then `fwrite` + `fdatasync` is called once for the entire batch
- **P50 (1μs)** reflects the amortized cost — most orders just buffer
- **P99+ (2.7ms)** reflects the actual `fdatasync` when the batch flushes

This is the correct production configuration. The P99 is disk I/O,
not matching engine latency. Production options to reduce P99:

1. **Async journal** (shipped): io_uring async ack — a completion-reaper thread decouples the durability fsync from the hot path; Path C P99 fell 32.8% (see Optimization History)
2. **Larger batch**: `batch_size=256` reduces fdatasync frequency 4x
3. **Page-cache only**: Skip fdatasync (accept data loss window on crash)

## Overhead Breakdown (P50)

x86 (authoritative — clean additive ordering A < B < C):
```
Core matching + compliance:   261 ns  (Path A)
Engine wrapper (seq+rate):     +8 ns  (Path B − A)
Journal (async io_uring ack): +346 ns  (Path C − B — completion-reaper overhead)
────────────────────────────────────────
Total full-stack P50:         615 ns  (Path C)
```
(On the ARM dev machine, the ~42 ns clock granularity can make Path B *appear*
faster than Path A — a measurement artifact, not real; the x86 ordering above
is the true overhead structure.)

## Formal Verification

The `MatchingEngine.tla` specification was model-checked with TLC:

```
368,192,427 states generated
171,187,419 distinct states found
11 levels deep (BFS depth), 0 states left on queue
0 invariant violations
Config: MatchingEngine4.cfg (MaxOrders=4, MaxTime=2)
Duration: 17 min 21 sec
```

**Invariants verified**:
- `NoNegativeQuantity` — no order has negative quantity
- `FIFO_Preservation` — timestamp ordering maintained at each price level
- `GTD_Expiry_Correctness` — expired GTD orders always cancelled
- `MatchingConservation` — placed = resting + filled + cancelled
- `FIFOExecution` — a fill consumes the earliest resting order at a price

See [`docs/Verification.md`](docs/Verification.md) for full details and
limitations of what the proof covers.

## Shadow Mode Validation

Shadow comparison validated against a deliberate FIFO violation:

```
Primary: order 1 (buy 50@100), order 2 (buy 30@100), sell 40@100
  → Trade: buy=1, sell=3, qty=40  (FIFO: order 1 matched)

Shadow (FIFO broken): order 2 first, then order 1, then sell
  → Trade[0]: buy=2, sell=3, qty=30  (wrong counterparty)
  → Trade[1]: buy=1, sell=3, qty=10  (remainder)

Divergence detected: trade_count mismatch (1 vs 2)
```

The comparator catches both trade count and counterparty divergence.
200-order clean run verified zero false positives.

## Binary Codec Microbenchmark

`./bin/BinaryCodecBenchmark` — single-threaded encode/decode rates for the three binary order-entry / market-data protocols. No engine, no transport — isolates the codec cost. 2M iterations per row, M3 Pro / Clang `-O3 -march=native`.

| Protocol | Operation | Wire size | ns/op | M ops/s | GB/s |
|----------|-----------|----------:|------:|--------:|-----:|
| OUCH 4.2 | EnterOrder encode | 49 B | 112.0 | 8.93 | 0.44 |
| OUCH 4.2 | EnterOrder decode | 49 B | 10.2 | 98.48 | 4.83 |
| ITCH 5.0 | AddOrder encode | 36 B | 34.1 | 29.36 | 1.06 |
| **SBE** | **NewOrderV1 encode** | **32 B** | **1.0** | **1015** | **32.49** |
| **SBE** | **NewOrderV1 decode** | **32 B** | **0.5** | **2128** | **68.10** |

### Why the gap

The 110× difference between OUCH and SBE encode reflects what each protocol asks the CPU to do:

- **OUCH 4.2** carries ASCII-decimal text in 8 of its fixed slots (order token, stock, firm) — every encode runs `snprintf`-equivalent integer formatting. That's hundreds of cycles per field.
- **ITCH 5.0** has one ASCII slot (stock); the rest are big-endian binary. Faster than OUCH but still pays byte-swap on a little-endian host.
- **SBE** is little-endian binary (native to x86/ARM), every field is a plain integer write. The codec compiles down to ~24 bytes of moves — at the limit of the timer's resolution.

The 1.0 ns/op SBE encode number is approaching the floor of what's measurable on a 24 MHz TSC. Real impact depends on whether the codec is the bottleneck (it's usually not — the engine dominates), but for venues moving billions of messages per day the difference is real.

## Sharding (Separate Measurement)

> **⚠ These numbers use a DIFFERENT order flow** (pre-partitioned by price
> range) and are **ARM64 dev-machine (M3 Pro) reference figures**, NOT the x86
> authoritative numbers. They are NOT comparable to the single-thread paths above.

| Metric (ARM64 dev reference) | Single-Thread | 4 Shards |
|--------|--------------|----------|
| Throughput | 7.6M ops/sec | 45.2M ops/sec |
| P50 | 84 ns | 42 ns |
| **Speedup** | — | **5.97x** |

**Critical caveat**: Real market data clusters around a moving midpoint. The
cross-shard match rate under realistic conditions is unknown. This is the
throughput ceiling, not the floor.

## Reproducing

```bash
# Three-path benchmark
cd build/
./benchmarks/HonestBenchmark --orders 50000 --seed 42

# Shadow mode test
./tests/ShadowModeTest

# TLA+ verification
cd spec/
java -XX:+UseParallelGC -cp tla2tools.jar tlc2.TLC MatchingEngine \
     -config MatchingEngine.cfg -workers auto -deadlock

# Full test suite
cd build/ && ctest --output-on-failure
```
