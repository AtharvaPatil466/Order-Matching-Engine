# High-Performance Order Matching Engine

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B20)
[![Latency](https://img.shields.io/badge/Processing_Latency-~40ns-green.svg)](#performance)
[![Throughput](https://img.shields.io/badge/Throughput-25M_ops/s-blue.svg)](#performance)
[![Build](https://img.shields.io/badge/Build-Passing-brightgreen.svg)](#getting-started)

A C++20 low-latency matching-engine with institutional-grade infrastructure: O(1) price-level lookup via `FlatPriceMap`, lock-free MPSC queues, thread-per-symbol horizontal scaling, CRC-32 journaling with deterministic replay, FIX 4.4 session management, TLA+-verified concurrency, primary-backup HA with epoch-based leader fencing, and a complete operational stack (config management, webhook alerting, Prometheus metrics, Docker deployment). Includes 28 test executables covering functional, stress, chaos, property, protocol, and HA replication testing.

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
- **Primary-Backup HA**: `ReplicationCoordinator` — TCP log-shipping from primary to backup, `HeartbeatMonitor` failure detection, `LeaderLease` epoch-based fencing, automatic backup promotion on primary failure (CME/NYSE exchange architecture pattern)
- **Journal Follower**: `JournalFollower` — single-host warm standby with `promote()` API and documented invariants
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
- **TLA+ Specifications**: MPSC queue, consumer protocol, and snapshot mechanism verified (~750k states)
- **Fault Injection**: `FaultInjector` singleton with 10+ injection points — journal short-writes, bit-flips, fsync failures, pool exhaustion, gateway fragmentation, spurious queue failures; zero-cost in production (`OB_ENABLE_FAULT_INJECTION` off)
- **Coverage-Guided Fuzzing**: libFuzzer harness for protocol parsing and order flow

## 📊 Performance Benchmarks

Measured locally using hardware timing on Apple Silicon M-series. All numbers are from `./bin/ManualBenchmark` and should be treated as machine-specific results, not a portable latency guarantee.

> [!IMPORTANT]
> These are **per-order processing latencies** — how long each operation takes once it is dequeued and executing on the matching thread. They do **not** include queuing delay. Since a matching engine serializes executions, the end-to-end latency for the Kth order in a burst of N is approximately `K × avg_processing_time`. Under sustained load, tail latency is dominated by queue depth, not processing variance. These benchmarks measure processing time consistency (branch predictability, cache behavior), not an end-to-end SLA.

### 🏎️ Lean Mode (`OB_LEAN_MODE`)
Disables risk checks, OTR tracking, and event notifications for raw matching speed.

| Operation | Avg | P50 | P99 | Throughput |
| :--- | :--- | :--- | :--- | :--- |
| **Add** | 39.8 ns | 42 ns | 42 ns | 25.2M ops/s |
| **Match** | 70.1 ns | 83 ns | 125 ns | 14.3M ops/s |
| **Cancel** | 32.4 ns | 0 ns | 208 ns | 30.8M ops/s |

### 🛡️ Full Mode (Enterprise Default)
Includes SMP, circuit breakers, OTR, risk limits, and full event routing.

| Operation | Avg | P50 | P99 | Throughput |
| :--- | :--- | :--- | :--- | :--- |
| **Add** | 520.3 ns | 541 ns | 1167 ns | 1.9M ops/s |
| **Match** | 109.2 ns | 84 ns | 250 ns | 9.1M ops/s |
| **Cancel** | 1092.3 ns | 875 ns | 6208 ns | 0.9M ops/s |

### Multi-Threaded Stress Test (4 threads)
| Scenario | Orders | Trades | Result |
| :--- | :--- | :--- | :--- |
| Concurrent Producers | 20,000 | ~6,300 | ✅ |
| Mixed Workload | 20,000 | ~5,800 | ✅ |
| Kill Switch Under Load | 11,000 | — | ✅ |
| Multi-Symbol (4 symbols) | 12,000 | ~5,900 | ✅ |
| High-Throughput Burst | 50,000 | ~19,500 | ✅ 5M ops/s |

> [!NOTE]
> Processing latency is reported in ~41.6ns increments due to Apple Silicon clock granularity. Actual logic time is likely faster.
> See [PerformanceWhitepaper.md](./PerformanceWhitepaper.md) for methodology.

### End-to-End Latency (ingress → completion, including queue delay)

Measured using `./bin/E2EBenchmark`. Timestamps are taken at enqueue and recorded after processing completes on the worker thread, so these numbers include queueing delay, contention, and matching time.

`E2EBenchmark` now reports **accepted throughput** separately from ingress rejections. The rate-limited scenarios pace producers slightly below the configured cap so the numbers reflect steady accepted flow instead of intentional drop-heavy overload.

| Scenario | Threads | P50 | P99 | P99.9 | Throughput |
| :--- | :--- | :--- | :--- | :--- | :--- |
| Single order (drain between submits) | 2 | 167 ns | 292 ns | 3.58 μs | 3.30M accepted ops/s |
| 10K burst (no rate limit) | 2 | 208 ns | 30.21 μs | 32.13 μs | 7.78M accepted ops/s |
| Rate-limited (5K/s, 10 producers) | 2 | 3.20 μs | 57.86 μs | 109.06 μs | 0.04M accepted ops/s |
| Rate-limited (10K/s, 20 producers) | 4 | 5.82 μs | 52.74 μs | 74.24 μs | 0.17M accepted ops/s |

**How tail latency is bounded**: Per-participant token-bucket rate limiting (configurable msgs/sec + burst size) caps the aggregate inbound rate. Queue-depth backpressure (configurable threshold, default 80%) rejects orders before the queue saturates. Together, these give a hard ceiling on queuing delay: `max_queue_depth × avg_processing_time`.

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

**28 test executables** covering 220+ individual test cases across 9 categories:

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
| **Benchmark** | BenchmarkRegression (GTest) | P99 latency regression gates |

### Formal Verification (TLA+)
- `MpscQueue.tla` — lock-free ring buffer linearizability
- `ConsumerProtocol.tla` — worker loop shutdown safety
- `Snapshot.tla` — read/write mutex prevents torn snapshots
- **~750k states** explored, all invariants hold

## 📚 Documentation

| Document | Purpose |
|----------|---------|
| [Architecture.md](./Architecture.md) | Full technical deep-dive |
| [PerformanceWhitepaper.md](./PerformanceWhitepaper.md) | Benchmark methodology |
| [docs/Runbook.md](./docs/Runbook.md) | Operational procedures, incident response |
| [docs/CapacityPlanning.md](./docs/CapacityPlanning.md) | Memory, CPU, disk, network sizing |
| [docs/ProductionReadiness.md](./docs/ProductionReadiness.md) | Remaining production checklist |
| [config/engine.conf.example](./config/engine.conf.example) | All configuration keys with defaults |

## 🔮 Remaining Work

The only remaining gaps require specialized hardware not available in a standard development environment:

| Item | Status | Blocker |
|------|--------|---------|
| io_uring zero-copy data path | Seams exist (`FixSession`/`FixFramer`) | Linux + io_uring kernel |
| DPDK kernel bypass | Architecture ready | Linux + supported NIC |
| Solarflare/Onload | Architecture ready | Solarflare hardware |
| Wire-to-wire latency measurement | E2E bench exists | Multi-host test rig |

---
*Developed for professional quantitative trading systems.*
