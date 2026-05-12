# High-Performance Order Matching Engine

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B20)
[![Latency](https://img.shields.io/badge/Matching_P50-125ns-green.svg)](#performance)
[![Throughput](https://img.shields.io/badge/Throughput-6.6M_ops/s-blue.svg)](#performance)
[![Tests](https://img.shields.io/badge/Tests-171_CTest_targets-brightgreen.svg)](#verification)
[![TLA+](https://img.shields.io/badge/TLA%2B-454M_states_verified-blueviolet.svg)](#formal-verification)

A C++20 low-latency matching engine with institutional-grade architecture drawing on exchange design principles: O(1) price-level lookup via `FlatPriceMap`, lock-free MPSC queues, thread-per-symbol horizontal scaling, CRC-32 journaling with deterministic replay, FIX 4.4 session management, TLA+-verified safety invariants (454M states, 0 violations), cross-host log replication, and a complete operational stack (config management, webhook alerting, Prometheus metrics, Docker deployment). 26K LOC, 30 test executables, 171 CTest targets, 5 TLA+ specifications.

## 🚀 Key Features

### Core Engine
- **Professional Order Types**: Limit, Market, IOC, FOK, Stop, StopLimit, TrailingStop, Pegged, Iceberg, Hidden, PostOnly
- **O(1) Price Lookup**: `FlatPriceMap` — flat array indexed by price tick, replacing `std::map` red-black trees
- **Intrusive Data Structures**: Zero-heap matching via `ObjectPool` + intrusive doubly-linked order lists
- **Dual Match Algorithms**: Price-Time FIFO and Pro-Rata allocation

### Concurrency & Networking
- **Thread-Per-Symbol Partitioning**: N worker threads with independent lock-free `MpscQueue` ring buffers, routed by `hash(symbolId) % numThreads`
- **TCP Gateway**: FIX 4.4 session management with non-blocking I/O via `epoll`/`kqueue`, version negotiation, numeric `OrdRejReason` mapping, and `TransactTime` enforcement
- **Shared Memory IPC**: `MarketDataPublisher` using POSIX `shm_open` with versioned `ShmHeader` (magic/version/entrySize) for prefix-compatible rolling upgrades
- **Binary Protocol Versioning**: `GatewayProtocol.h` — 16-byte fixed header with V1/V2 payload evolution, forward+backward compatibility, and `GatewayResponse` ack frames

### Regulatory & Risk
- **Self-Match Prevention (SMP)**: Cancel-Taker strategy
- **Volatility Circuit Breakers**: Configurable % price band halts
- **OTR Monitoring**: Order-to-Trade ratio tracking per participant
- **Kill Switch**: Instant cancellation of all orders for a participant across all symbols
- **Pre-Trade Risk Limits**: Max order size, notional, and position limits per participant
- **Per-Participant Rate Limiting**: Token-bucket throttling at ingress (configurable rate + burst) to bound queue depth and tail latency
- **Queue-Depth Backpressure**: Rejects orders when queue exceeds configurable threshold, hard ceiling on queuing delay
- **Admin HTTP Server**: Real-time `GET /metrics`, `GET /otr`, `GET /book` JSON endpoints on port 8080

### Reliability & Observability
- **CRC-32 Journaling**: Write-ahead log with crash recovery, atomic checkpoint, and deterministic replay via virtual clock
- **Cross-Host Log Replication**: `ReplicationCoordinator` — TCP log-shipping from primary to backup, `HeartbeatMonitor` failure detection, and `LeaderLease` epoch-based fencing
- **Crash Recovery & Warm Standby**: `JournalFollower` — single-host automated recovery with `promote()` API and documented invariants
- **Deterministic Sequencing**: Monotonic `sequenceNumber` on all `Trade`, `OrderUpdate`, and `MarketDataUpdate` events for gap detection
- **GTD Virtual Clock**: `setExpiryClock(ClockFn)` seam for cross-day replay — DAY/GTD expirations are journaled and replayed identically
- **Structured Logging**: Pluggable `StructuredSink` API with `NullSink` (zero-cost default), `JsonStderrSink` (dev), and `CapturingSink` (tests)
- **Prometheus Metrics**: `MetricsRegistry` with atomic Counters, Gauges, Histograms; `/prometheus` text-exposition endpoint
- **Webhook Alerting**: `AlertDispatcher` — background thread delivery to Slack, PagerDuty, or generic HTTP webhooks with configurable severity filtering
- **Config Management**: `Config` key-value loader with env var override (`OB_` prefix), type-safe getters, and hot-reload support
- **End-to-End Latency Tracking**: Ingress timestamps on every order; per-thread `LatencyTracker` histograms for real P50/P99/P99.9 including queue delay
- **Hardware Timing**: `mach_absolute_time` (Apple Silicon) / `rdtsc` (x86) for sub-clock-quantum benchmarking
- **Docker Deployment**: Multi-stage `Dockerfile` + `docker-compose.yml` for primary-backup topology with health checks and journal volumes

### Formal Verification & Chaos Engineering
- **TLA+ Specifications**: 5 specs including MatchingEngine safety invariants (454M states, 181M distinct, 0 violations), MPSC queue linearizability, consumer protocol shutdown, and snapshot mechanism
- **Shadow Mode**: Dual-book divergence detection — validated against deliberate FIFO violations with trade-level and snapshot-level comparison
- **Fault Injection**: `FaultInjector` singleton with 10+ injection points — journal short-writes, bit-flips, fsync failures, pool exhaustion, gateway fragmentation, spurious queue failures; zero-cost in production (`OB_ENABLE_FAULT_INJECTION` off)
- **Coverage-Guided Fuzzing**: libFuzzer harness for protocol parsing and order flow

## 📊 Performance Benchmarks

Measured using `HonestBenchmark` — a single deterministic order flow (50K orders, seed=42) fed through three paths on Apple Silicon ARM64, Clang C++20 -O2. Each order individually timed with `clock_gettime_nsec_np`.

> [!IMPORTANT]
> **x86 Translation Note:** All numbers in this document are measured on a single-socket Apple M-series CPU. These are **not** multi-socket x86 numbers. Core matching latency (`125 ns`) does not map directly to Intel Xeon / AMD EPYC architectures, and multi-thread scaling bounds do not factor in cross-NUMA cache coherence traffic. Bare-metal x86 benchmarking is pending.
> 
> These are **per-order processing latencies** on the matching thread. They do **not** include network I/O, async queue delay, or OS scheduling jitter. See [BENCHMARKS.md](./BENCHMARKS.md) for full methodology and caveats.

### Three-Path Latency (identical order flow)

| Path | What's Included | P50 | P99 | P99.9 | Throughput |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Core matching** | OrderBook + STP + WashTrade + LULD | **125 ns** | 333 ns | 458 ns | 6.6M ops/s |
| **Engine wrapper** | + sequence alloc, rate limiter | 84 ns | 292 ns | 417 ns | 7.1M ops/s |
| **Full-stack journal** | + GroupCommit (batch=64, fdatasync) | 1,040 ns | 2.7 ms | 4.0 ms | 22K ops/s |

> [!NOTE]
> The engine wrapper appears faster than core matching due to CPU cache warming (it runs second). The real core matching cost is Path A: **125 ns P50**.
>
> The journal P99 (2.7ms) is entirely `fdatasync` disk I/O. GroupCommit amortizes this across 64 entries — P50 is only 1μs. Production deployments use async journal threads to decouple persistence from the hot path.

### Multi-Threaded Stress Test (4 threads)
| Scenario | Orders | Trades | Result |
| :--- | :--- | :--- | :--- |
| Concurrent Producers | 20,000 | ~6,300 | ✅ |
| Mixed Workload | 20,000 | ~5,800 | ✅ |
| Kill Switch Under Load | 11,000 | — | ✅ |
| Multi-Symbol (4 symbols) | 12,000 | ~5,900 | ✅ |
| High-Throughput Burst | 50,000 | ~19,500 | ✅ 5M ops/s |

## 🏗️ Architecture

```
                     ┌──────────────────┐
    FIX Gateway ────▶│  MatchingEngine  │◀──── Admin HTTP :8080
                     │  (order router)  │
                     └──┬───┬───┬───┬───┘
                        │   │   │   │
                    ┌───▼─┐ │ ┌─▼───▼──┐
                    │ Q[0]│ │ │ Q[1..N] │   MpscQueue per thread
                    └──┬──┘ │ └──┬──────┘
                       │    │    │
                  ┌────▼──┐ │ ┌──▼────┐
                  │Thread0│ │ │Thread1│     Worker threads
                  │ BTC,  │ │ │ SOL,  │     (symbol affinity)
                  │ ETH   │ │ │ AVAX  │
                  └───────┘ │ └───────┘
                            │
                     ┌──────▼──────┐
                     │  OrderBook  │     FlatPriceMap O(1)
                     │  ObjectPool │     Intrusive linked lists
                     │  Journal    │     CRC-32 WAL
                     └─────────────┘
```

See [Architecture.md](./Architecture.md) for the full technical deep-dive.
See [docs/ProductionReadiness.md](./docs/ProductionReadiness.md) for the remaining work required before treating this as a production exchange component.

## 🚦 Getting Started

### Prerequisites
- Clang 14+ or GCC 11+ (C++20 support)

### Build & Run
```bash
# Build with all enterprise features
./build.sh

# Build for sub-100ns lean mode
./build.sh --lean

# Run the engine (4 threads, admin on port 8080)
./bin/OrderEngine --threads 4 --port 8080 --symbols 4

# Run tests
./bin/ManualTest              # Functional tests
./bin/StressTest              # Multi-threaded stress tests
./bin/PropertyTest            # Randomized invariant checks
./bin/JournalCrashTest        # Crash recovery validation
./bin/GatewayProtocolTest     # Binary protocol schema evolution
./bin/GTDReplayTest           # Deterministic expiry replay
./bin/MdFeedSchemaCompatTest  # Market data feed compatibility
./bin/ManualBenchmark         # Per-order processing latency
./bin/E2EBenchmark            # End-to-end latency (includes queue delay)
```

### CI / Hardening
```bash
# CMake project tests only
ctest --test-dir build --output-on-failure -L project

# Sanitizer build
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure -L project

# Reproducible local benchmark run
./scripts/run_benchmarks.sh
```

The GitHub Actions workflow runs release tests and sanitizer tests on every push/PR.

### Admin Server
```bash
curl localhost:8080/metrics              # throughput & queue depth
curl "localhost:8080/book?symbolId=0"    # L2 order book snapshot
curl "localhost:8080/otr?participantId=1" # order-to-trade ratio
```

## 🧪 Verification

**30 test executables** covering **171 CTest targets**[^1] across 10 categories:

| Category | Tests | Description |
|----------|-------|-------------|
| **Functional** | ManualTest, UnitTests (GTest) | All order types, matching, cancellation, edge cases |
| **Stress** | StressTest | 5 multi-threaded scenarios, 4 concurrent producers |
| **Property** | PropertyTest, JournalReplayPropertyTest, SnapshotConsistencyTest | Randomized invariant checks, replay determinism |
| **Chaos** | FaultInjectorTest, PoolExhaustionTest, JournalChaosTest, JournalCorruptionTest, GatewayChaosTest, QueueChaosTest, CheckpointChaosTest, CombinedChaosTest | 10+ fault-injection points, short-writes, bit-flips, EAGAIN |
| **Integration** | GatewayIntegrationTest, FixTcpGatewayTest, AdminServerEndpointsTest | TCP round-trip, FIX session, HTTP endpoints |
| **Protocol** | GatewayProtocolTest, MarketDataSchemaTest, MdFeedSchemaCompatTest, RejectReasonContractTest, SubmitResultTest | Binary wire format evolution, ShmHeader compat |
| **Recovery** | JournalCrashTest, JournalFollowerTest, GTDReplayTest | Crash recovery, log-ship convergence, virtual-clock replay |
| **HA & Ops** | ReplicationProtocolTest, ConfigTest, AlertDispatcherTest | Primary-backup replication, config loader, webhook alerts |
| **Shadow** | ShadowModeTest | Dual-book divergence detection, FIFO violation catching |
| **Benchmark** | BenchmarkRegression (GTest) | P99 latency regression gates |

[^1]: *CTest targets map to individual executables and GTest cases. These 171 targets encompass over 350+ underlying assertions and scenarios, resolving the previous manual estimate of "220+ test cases".*

### Formal Verification (TLA+)

**5 TLA+ specifications**, model-checked with TLC:

| Specification | States | Invariants |
|--------------|--------|------------|
| `MatchingEngine.tla` | **454M generated, 181M distinct** | NoNegativeQuantity, FIFO_Preservation, GTD_Expiry_Correctness *(Model Scope: 2 participants, 2 price levels, 4 orders, qty 1-3)* |
| `MpscQueue.tla` | ~250K | Lock-free ring buffer linearizability |
| `EngineConsumer.tla` | ~200K | Worker loop shutdown safety |
| `Snapshot.tla` / `SnapshotLocked.tla` | ~300K | Read/write mutex prevents torn snapshots |

### Shadow Mode Validation
- Dual `OrderBook` instances fed identical order streams
- Deliberately broken FIFO detected via trade-level comparison
- 200-order clean run verified zero false positives

## 📚 Documentation

| Document | Purpose |
|----------|---------|
| [Architecture.md](./Architecture.md) | Full technical deep-dive |
| [BENCHMARKS.md](./BENCHMARKS.md) | Honest three-path latency methodology & results |
| [PerformanceWhitepaper.md](./PerformanceWhitepaper.md) | Benchmark methodology & analysis |
| [docs/Verification.md](./docs/Verification.md) | TLA+ model checking results |
| [docs/Runbook.md](./docs/Runbook.md) | Operational procedures, incident response |
| [docs/CapacityPlanning.md](./docs/CapacityPlanning.md) | Memory, CPU, disk, network sizing |
| [docs/ProductionReadiness.md](./docs/ProductionReadiness.md) | Remaining production checklist |
| [config/engine.conf.example](./config/engine.conf.example) | All configuration keys with defaults |

## 🔮 Remaining Work

The only remaining gaps require specialized hardware not available in a standard development environment:

| Item | Status | Blocker |
|------|--------|---------|
| x86 Bare Metal Benchmarks | E2E bench exists | Multi-socket EC2 c5.metal instance |
| `Replication.tla` Verification | Written | Compute time for TLC model checker |
| io_uring zero-copy data path | Seams exist (`FixSession`/`FixFramer`) | Linux + io_uring kernel |
| DPDK kernel bypass | Architecture ready | Linux + supported NIC |
| Solarflare/Onload | Architecture ready | Solarflare hardware |
| Wire-to-wire latency measurement | E2E bench exists | Multi-host test rig |

---
*Developed for professional quantitative trading systems.*
*C++20 · 26K LOC · 30 test executables · 171 CTest targets · 454M TLA+ states verified*
