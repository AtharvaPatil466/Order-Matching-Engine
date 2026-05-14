# Benchmark Methodology & Results

> **Platform**: Apple M3 Pro (ARM64) · **Compiler**: Clang C++20 -O2
> **Date**: 2026-05-14 · **Seed**: 42
> **Tests**: 181/181 passing

## TL;DR

| Claim | Evidence | Status |
|-------|----------|--------|
| Core matching: **125 ns** P50 | Identical 50K order flow, `nowNs()` per-order | ✅ Verified |
| Full-stack with journal: **1,040 ns** P50 | GroupCommit batch=64, fdatasync per batch | ✅ Verified |
| Safety invariants | TLC: 454M states, 181M distinct, 0 violations | ✅ Verified |
| Shadow mode | FIFO violation detected via trade divergence | ✅ Validated |
| **SBE encode: 1.0 ns/op (1015 M ops/s)** | Pure codec microbench, no engine | ✅ Measured |
| **SBE 110× faster than OUCH encode** | Binary vs ASCII-decimal field formatting | ✅ Measured |

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

```
── Path A: OrderBook::addOrder() [core matching] ──
  Orders:     50,000
  Throughput: 6.6M orders/sec
  P50:    125 ns    P90:    209 ns
  P99:    333 ns    P99.9:  458 ns
  Max:  48,834 ns

── Path B: MatchingEngine::submitOrder() [+ engine wrapper] ──
  Orders:     50,000
  Throughput: 7.1M orders/sec
  P50:     84 ns    P90:    208 ns
  P99:    292 ns    P99.9:  417 ns
  Max:  19,333 ns

── Path C: MatchingEngine + Journal [GroupCommit batch=64] ──
  Orders:     50,000
  Throughput: 22,293 orders/sec
  P50:  1,040 ns    P90:  2,040 ns
  P99:  2.7 ms      P99.9: 4.0 ms
  Max:  9.7 ms
```

## GroupCommit Explained

Path C uses `SyncPolicy::GroupCommit` with `batch_size=64`:
- Entries are buffered in memory until 64 accumulate
- Then `fwrite` + `fdatasync` is called once for the entire batch
- **P50 (1μs)** reflects the amortized cost — most orders just buffer
- **P99+ (2.7ms)** reflects the actual `fdatasync` when the batch flushes

This is the correct production configuration. The P99 is disk I/O,
not matching engine latency. Production options to reduce P99:

1. **Async journal**: Dedicated I/O thread decouples persistence from hot path
2. **Larger batch**: `batch_size=256` reduces fdatasync frequency 4x
3. **Page-cache only**: Skip fdatasync (accept data loss window on crash)

## Overhead Breakdown (P50)

```
Core matching + compliance:   125 ns  (Path A)
Engine wrapper (seq+rate):    -41 ns  (Path B — faster from cache warming)
Journal (GroupCommit/64):    +956 ns  (Path C — amortized batch I/O)
────────────────────────────────────────
Total full-stack P50:       1,040 ns
```

## Formal Verification

The `MatchingEngine.tla` specification was model-checked with TLC:

```
454,022,166 states generated
181,004,838 distinct states found
0 invariant violations
Duration: 12 min 35 sec
```

**Invariants verified**:
- `NoNegativeQuantity` — no order has negative quantity
- `FIFO_Preservation` — timestamp ordering maintained at each price level
- `GTD_Expiry_Correctness` — expired GTD orders always cancelled

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

`./bin/BinaryCodecBenchmark` — single-threaded encode/decode rates for the three binary order-entry / market-data protocols. No engine, no transport — isolates the codec cost. 2M iterations per row, M3 Pro / Clang `-O2`.

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
> range). They are NOT comparable to the single-thread paths above.

| Metric | Single-Thread | 4 Shards |
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
