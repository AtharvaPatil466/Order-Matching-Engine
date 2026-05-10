# High-Performance Order Matching Engine — Project Overview

> **21,600+ lines of C++20** | 30 headers | 8 source files | 30 test files | 28 test executables
>
> A production-grade, institutional-architecture matching engine built for sub-100ns processing latency and horizontal scalability.

---

## 1. Executive Summary

This is a C++20 low-latency order matching engine with institutional-grade infrastructure modeled after CME/NYSE exchange architecture. It implements O(1) price-level lookup via `FlatPriceMap`, lock-free MPSC queues, thread-per-symbol horizontal scaling, CRC-32 journaling with deterministic replay, FIX 4.4 session management, TLA+-verified concurrency, primary-backup high availability with epoch-based leader fencing, and a complete operational stack including config management, webhook alerting, Prometheus metrics, and Docker deployment.

### Codebase Statistics

| Metric | Count |
|--------|-------|
| Total C++ LOC | 21,630+ |
| Header files (`include/`) | 30 |
| Source files (`src/`) | 8 |
| Test files (`tests/`) | 30 |
| Test executables | 28 |
| Individual test cases | 220+ |
| TLA+ specifications | 4 (750k+ states verified) |
| Documentation files | 6 |

---

## 2. Core Engine Architecture

### System Topology

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

### Order Types Supported
- **Standard**: Limit, Market
- **Time-In-Force**: IOC (Immediate-or-Cancel), FOK (Fill-or-Kill), DAY, GTD (Good-Til-Date)
- **Conditional**: Stop, StopLimit, TrailingStop
- **Institutional**: Pegged, Iceberg (hidden quantity), Hidden, PostOnly
- **Match Algorithms**: Price-Time FIFO and Pro-Rata allocation

### Core Data Structures

| Component | Purpose | Complexity |
|-----------|---------|------------|
| `FlatPriceMap` | Price-level lookup by tick index | O(1) insert/lookup |
| `FlatHashMap` | Robin-Hood open-addressing hash map | O(1) amortized |
| `IntrusiveList` | Doubly-linked order queue per price level | O(1) insert/remove |
| `ObjectPool` | Pre-allocated slab allocator for `Order` nodes | O(1) alloc/dealloc |
| `RingBuffer` | Cache-line aligned lock-free ring buffer | O(1) push/pop |
| `MpscQueue` | Multi-producer, single-consumer lock-free queue | O(1) push/pop |

### Data Flow

1. **Ingress**: Order arrives via TCP Gateway (binary framing or FIX 4.4)
2. **Rate Limiting**: Token-bucket per-participant throttling at ingress
3. **Queue Routing**: `hash(symbolId) % numThreads` → lock-free `MpscQueue`
4. **Validation**: Circuit Breaker check, OTR enforcement, risk limit verification
5. **Matching**: Price-time priority against resting book (`FlatPriceMap`)
6. **Execution**: `Trade` records generated, callbacks invoked
7. **Persistence**: CRC-32 journal entry written (WAL)
8. **Analytics**: VWAP, TWAP, and OTR stats updated
9. **Market Data**: L2 updates published via shared memory (`shm_open`)

---

## 3. Concurrency & Networking

### Thread-Per-Symbol Partitioning
N worker threads with independent lock-free `MpscQueue` ring buffers. Orders are deterministically routed by `hash(symbolId) % numThreads`, eliminating cross-thread contention on the hot path.

### TCP Gateway (Binary Protocol)
- Non-blocking I/O via `epoll` (Linux) / `kqueue` (macOS)
- `GatewayProtocol.h` — 16-byte fixed header with V1/V2 payload evolution
- Forward and backward compatibility via version negotiation
- `GatewayResponse` ack frames with numeric reject reasons

### FIX 4.4 Gateway
- Full FIX 4.4 session management (`FixSession.h`)
- `FixFramer` for message reassembly across TCP fragments
- `FIXParser` — zero-copy tag-value parser
- `FixTcpGateway` — production-grade FIX acceptor
- `TransactTime` enforcement and `OrdRejReason` numeric mapping

### Shared Memory IPC
- `MarketDataPublisher` using POSIX `shm_open` with versioned `ShmHeader`
- Magic/version/entrySize fields for prefix-compatible rolling upgrades
- Low-overhead L2 market data distribution to co-located consumers

---

## 4. Regulatory & Risk Management

| Feature | Implementation |
|---------|---------------|
| **Self-Match Prevention (SMP)** | Cancel-Taker strategy — prevents wash trading |
| **Circuit Breakers** | Configurable % price band halts (volatility protection) |
| **OTR Monitoring** | Real-time Order-to-Trade ratio tracking per participant |
| **Kill Switch** | Instant cancellation of all orders per participant across all symbols |
| **Pre-Trade Risk Limits** | Max order size, notional value, and position limits per participant |
| **Rate Limiting** | Token-bucket throttling at ingress (configurable rate + burst) |
| **Queue Backpressure** | Rejects orders when queue exceeds configurable threshold (default 80%) |

---

## 5. Reliability & High Availability

### CRC-32 Journaling
- Write-ahead log with atomic CRC-32 integrity on every entry
- Crash recovery: replay stops at first corrupted/truncated record
- Atomic checkpoint: snapshots active book state, rewrites journal
- Deterministic replay via virtual clock (`setExpiryClock(ClockFn)`)

### Primary-Backup Replication
Modeled after CME/NYSE exchange HA architecture:
- `ReplicationCoordinator` — TCP log-shipping from primary to backup
- `HeartbeatMonitor` — configurable failure detection timeout
- `LeaderLease` — epoch-based fencing prevents split-brain
- `JournalFollower` — single-host warm standby with `promote()` API
- Automatic backup promotion on primary failure detection

### Deterministic Sequencing
- Monotonic `sequenceNumber` on all `Trade`, `OrderUpdate`, and `MarketDataUpdate` events
- Gap detection for downstream consumers
- GTD virtual clock seam for cross-day replay — DAY/GTD expirations are journaled and replayed identically

---

## 6. Observability & Operations

### Structured Logging
- Pluggable `StructuredSink` API
- `NullSink` — zero-cost in production (compiled out)
- `JsonStderrSink` — development diagnostics
- `CapturingSink` — test assertions on log output

### Prometheus Metrics
- `MetricsRegistry` with atomic Counters, Gauges, Histograms
- `/prometheus` text-exposition endpoint for standard Prometheus scraping
- Tracks: order flow, rejects, queue depth, latency histograms, journal health, gateway sessions

### Webhook Alerting
- `AlertDispatcher` — background thread delivery
- Targets: Slack, PagerDuty, or generic HTTP webhooks
- Configurable severity filtering (Info, Warning, Critical)

### Configuration Management
- `Config` key-value loader with env var override (`OB_` prefix)
- Type-safe getters (`getInt`, `getString`, `getBool`)
- Hot-reload support for runtime-adjustable parameters
- Example config: `config/engine.conf.example`

### Admin HTTP Server (Port 8080)

| Endpoint | Purpose |
|----------|---------|
| `GET /health` | K8s liveness probe |
| `GET /metrics` | Internal counters (JSON) |
| `GET /prometheus` | Prometheus text exposition |
| `GET /book?symbolId=0` | L2 order book snapshot |
| `GET /otr?participantId=1` | Order-to-trade ratio |

### Docker Deployment
- Multi-stage `Dockerfile` for minimal production image
- `docker-compose.yml` for primary-backup topology
- Health checks and journal volume persistence

---

## 7. Performance Benchmarks

Measured locally using hardware timing on Apple Silicon M-series. All numbers are from `./bin/ManualBenchmark` and should be treated as machine-specific results, not a portable latency guarantee.

> **Note**: These are **per-order processing latencies** — how long each operation takes once it is dequeued and executing on the matching thread. They do **not** include queuing delay.

### Lean Mode (`OB_LEAN_MODE`)
Disables risk checks, OTR tracking, and event notifications for raw matching speed.

| Operation | Avg | P50 | P99 | Throughput |
| :--- | :--- | :--- | :--- | :--- |
| **Add** | 39.8 ns | 42 ns | 42 ns | 25.2M ops/s |
| **Match** | 70.1 ns | 83 ns | 125 ns | 14.3M ops/s |
| **Cancel** | 32.4 ns | 0 ns | 208 ns | 30.8M ops/s |

### Full Mode (Enterprise Default)
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

### End-to-End Latency (ingress → completion, including queue delay)

| Scenario | Threads | P50 | P99 | P99.9 | Throughput |
| :--- | :--- | :--- | :--- | :--- | :--- |
| Single order (drain between submits) | 2 | 167 ns | 292 ns | 3.58 μs | 3.30M accepted ops/s |
| 10K burst (no rate limit) | 2 | 208 ns | 30.21 μs | 32.13 μs | 7.78M accepted ops/s |
| Rate-limited (5K/s, 10 producers) | 2 | 3.20 μs | 57.86 μs | 109.06 μs | 0.04M accepted ops/s |
| Rate-limited (10K/s, 20 producers) | 4 | 5.82 μs | 52.74 μs | 74.24 μs | 0.17M accepted ops/s |

**Tail latency bounding**: Per-participant token-bucket rate limiting caps aggregate inbound rate. Queue-depth backpressure (configurable threshold, default 80%) rejects orders before the queue saturates. Together: `max_queue_depth × avg_processing_time` gives a hard ceiling on queuing delay.

---

## 8. Verification & Testing

### Test Suite — 28 Executables, 220+ Test Cases

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

| Specification | States Explored | Invariants |
|--------------|-----------------|------------|
| `MpscQueue.tla` | ~250K | Lock-free ring buffer linearizability |
| `EngineConsumer.tla` | ~200K | Worker loop shutdown safety |
| `Snapshot.tla` | ~150K | Read/write mutex prevents torn snapshots |
| `SnapshotLocked.tla` | ~150K | Locked variant correctness |
| **Total** | **~750K** | **All invariants hold** |

### Chaos Engineering & Fault Injection
`FaultInjector` singleton with 10+ injection points:
- Journal: short-writes, bit-flips, fsync failures
- Memory: pool exhaustion, allocation failures
- Network: gateway fragmentation, EAGAIN injection
- Queue: spurious empty returns, backpressure simulation
- **Zero-cost in production** (`OB_ENABLE_FAULT_INJECTION` compiled out)

### Coverage-Guided Fuzzing
- libFuzzer harness for protocol parsing and order flow
- `fuzz/` directory with dedicated build configuration

### CI/CD Pipeline
GitHub Actions workflow runs on every push/PR:
- **macOS Release**: Full test suite
- **Ubuntu Debug + ASan/UBSan**: Memory and undefined behavior sanitizers
- **Ubuntu ThreadSanitizer**: Data race detection
- E2E benchmark smoke test with artifact upload

---

## 9. Capacity Planning

### Memory Sizing

| Scenario | Live Orders | Symbols | Memory |
|----------|-------------|---------|--------|
| Small (dev/test) | 10K | 4 | ~256 MB |
| Medium (prop desk) | 100K | 50 | ~512 MB |
| Large (exchange) | 1M | 500 | ~2 GB |
| Ultra (HFT venue) | 5M | 2000 | ~8 GB |

### CPU Scaling

| Cores | Workers | Throughput (lean) | Throughput (full) |
|-------|---------|-------------------|-------------------|
| 4 | 1 | 25M ops/s | 1.9M ops/s |
| 8 | 4 | 80M ops/s | 7M ops/s |
| 16 | 8 | 150M ops/s | 14M ops/s |
| 32 | 16 | 280M ops/s | 25M ops/s |

### Disk & Network

- **Journal**: ~80-100 bytes/entry. 1M orders/day ≈ 100MB/day
- **Recommended**: NVMe SSD with ≥100K IOPS (production)
- **Network**: 10 GbE for production; 25 GbE + kernel bypass for HFT

---

## 10. Deployment

### Build & Run
```bash
# Build with all enterprise features
./build.sh

# Build for sub-100ns lean mode
./build.sh --lean

# Run the engine
./bin/OrderEngine --threads 4 --port 8080 --symbols 4

# Run full test suite
ctest --test-dir build --output-on-failure -L project
```

### Docker
```bash
docker-compose up -d engine-primary
curl -sf http://localhost:8080/health
```

### Configuration Precedence
1. Environment variable `OB_<KEY>` (highest priority)
2. Config file value (`config/engine.conf.example`)
3. Compiled default (lowest priority)

---

## 11. File Structure

```
include/              30 header files — all core logic is header-only
  ├── OrderBook.h         Core matching engine (373 lines)
  ├── FlatPriceMap.h      O(1) price-level lookup (401 lines)
  ├── FlatHashMap.h       Robin-Hood hash map (285 lines)
  ├── MatchingEngine.h    Thread-per-symbol router (11K bytes)
  ├── MpscQueue.h         Lock-free multi-producer queue
  ├── Journal.h           CRC-32 WAL + crash recovery
  ├── TcpGateway.h        Binary protocol gateway
  ├── FIXParser.h         FIX 4.4 tag-value parser
  ├── FixSession.h        FIX session state machine
  ├── FixTcpGateway.h     FIX TCP acceptor
  ├── ReplicationProtocol.h  Primary-backup HA (20K bytes)
  ├── Config.h            Key-value config with env override
  ├── AlertDispatcher.h   Webhook alerting
  ├── Metrics.h           Prometheus metrics registry
  ├── StructuredLog.h     Pluggable structured logging
  ├── MarketDataPublisher.h  Shared memory market data
  ├── GatewayProtocol.h   Binary wire protocol versioning
  └── ...                 Types, Utils, LatencyTracker, etc.

src/                  8 source files — thin compilation units
tests/                30 test files — 28 executables
spec/                 TLA+ formal specifications (4 specs)
config/               Example configuration files
docs/                 Runbook, CapacityPlanning, ProductionReadiness
fuzz/                 Coverage-guided fuzzing harness
scripts/              Benchmark and build automation
```

---

## 12. Documentation

| Document | Purpose |
|----------|---------|
| `README.md` | Feature overview and getting started |
| `Architecture.md` | Technical architecture deep-dive |
| `PerformanceWhitepaper.md` | Benchmark methodology and analysis |
| `docs/Runbook.md` | Operational procedures and incident response |
| `docs/CapacityPlanning.md` | Memory, CPU, disk, and network sizing |
| `docs/ProductionReadiness.md` | Remaining production checklist |
| `config/engine.conf.example` | All configuration keys with defaults |
| `PRODUCTION_ROADMAP.md` | Development roadmap and status |

---

## 13. Remaining Work

The system is architecturally complete. The only remaining gaps require specialized hardware not available in a standard development environment:

| Item | Status | Blocker |
|------|--------|---------|
| io_uring zero-copy data path | Seams exist (`FixSession`/`FixFramer`) | Linux + io_uring kernel |
| DPDK kernel bypass | Architecture ready | Linux + supported NIC |
| Solarflare/Onload | Architecture ready | Solarflare hardware |
| Wire-to-wire latency measurement | E2E bench exists | Multi-host test rig |

---

*Developed for professional quantitative trading systems.*
*C++20 · 21,600+ LOC · 28 test executables · 750K+ TLA+ states verified*
