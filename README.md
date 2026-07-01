# High-Performance Order Matching Engine

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B20)
[![Latency](https://img.shields.io/badge/Matching_P50-125ns-green.svg)](#performance)
[![Throughput](https://img.shields.io/badge/Throughput-6.6M_ops/s-blue.svg)](#performance)
[![Codec](https://img.shields.io/badge/SBE_encode-1ns/op-orange.svg)](#binary-codec-performance)
[![Tests](https://img.shields.io/badge/Tests-426_CTest_targets-brightgreen.svg)](#verification)
[![Chaos](https://img.shields.io/badge/Chaos_Scenarios-19_live-orange.svg)](#chaos-suite)
[![TLA+](https://img.shields.io/badge/TLA%2B-454M_states_verified-blueviolet.svg)](#formal-verification)
[![Protocols](https://img.shields.io/badge/Wire_Protocols-FIX_OUCH_ITCH_SBE-blueviolet.svg)](#multi-protocol-order-entry)

A C++20 low-latency matching engine with institutional-grade architecture drawing on exchange design principles: O(1) price-level lookup via `FlatPriceMap`, lock-free MPSC queues, thread-per-symbol horizontal scaling, CRC-32 journaling with deterministic replay, four wire protocols (FIX 4.2/4.4, OUCH 4.2, ITCH 5.0, SBE) over both real TCP and UDP transports, MoldUDP64 multicast with gap-recovery retransmission service, TLA+-verified safety invariants (454M states on `MatchingEngine.tla` + lease-propagation model on `Replication.tla`), **live multi-container chaos suite (19 scenarios) empirically verifying NoCommittedLoss, no-split-brain under partition/loss/clock-skew, snapshot catchup, and rolling restart**, end-to-end wired primary-backup replication with token-authenticated chaos injection endpoint, and a complete operational stack (config management, webhook alerting, Prometheus metrics with replication counters, `/version` build-metadata endpoint, Docker deployment). **46.5K LOC, 79 test executables, 426 CTest targets, 19 chaos scenarios, 12 TLA+ specifications.**

## 🚀 Key Features

### Core Engine
- **Professional Order Types**: Limit, Market, IOC, FOK, Stop, StopLimit, TrailingStop, Pegged, Iceberg, Hidden, PostOnly, MIT, MOC, LOC
- **O(1) Price Lookup**: `FlatPriceMap` — flat array indexed by price tick, replacing `std::map` red-black trees
- **Intrusive Data Structures**: Zero-heap matching via `ObjectPool` + intrusive doubly-linked order lists
- **Dual Match Algorithms**: Price-Time FIFO and Pro-Rata allocation with LMM/DMM floor guarantee (40% of available qty) and rounding-remainder priority

### Concurrency & Networking
- **Thread-Per-Symbol Partitioning**: N worker threads with independent lock-free `MpscQueue` ring buffers, routed by `hash(symbolId) % numThreads`
- **TCP Gateway**: FIX 4.4 session management with non-blocking I/O via `epoll`/`kqueue`, version negotiation, numeric `OrdRejReason` mapping, and `TransactTime` enforcement
- **Shared Memory IPC**: `MarketDataPublisher` using POSIX `shm_open` with versioned `ShmHeader` (magic/version/entrySize) for prefix-compatible rolling upgrades
- **Binary Protocol Versioning**: `GatewayProtocol.h` — 16-byte fixed header with V1/V2 payload evolution, forward+backward compatibility, and `GatewayResponse` ack frames

### Multi-Protocol Order Entry
All four protocols dispatch into the same `MatchingEngine`. Drop a different session class in front of the gateway loop, and you've changed the wire format without touching the engine.

- **FIX 4.2 / 4.4** (`FixSession.h`): ASCII text + checksum, full session-layer state machine (Logon/Logout/Heartbeat/TestRequest/ResendRequest with `SequenceReset-GapFill`), numeric `OrdRejReason` mapped from internal `RejectReason`, `TransactTime` validation on 4.4 inbound. Version negotiation per accepted `BeginString`.
- **OUCH 4.2** (`OuchSession.h`, `OuchProtocol.h`): NASDAQ binary order entry. Inbound `O`/`X`/`U` (Enter/Cancel/Replace) → outbound `A`/`J`/`C`/`E`/`U` (Accepted/Rejected/Canceled/Executed/Replaced). Token ↔ engine-orderId reverse-map preserves identity across `submitCancelReplace`. Big-endian fixed-width.
- **SBE** (`SbeSession.h`, `SbeProtocol.h`): FIX-TG schema-driven binary (CME / ICE style). NewOrder v1/v2 + OrderAck. Forward-compatibility proven by test: **the first 24 bytes of any v2 block encoding match the v1 encoding byte-for-byte** — a v1 reader on a v2 message reads the original fields unchanged, no codec branch.
- **SoupBinTCP** (`SoupBinTcpSession.h`): the Nasdaq TCP envelope that wraps OUCH on the wire. 3-byte length+type framing, login negotiation, configurable heartbeat / peer-idle (spec defaults 1s/15s), logout handshake. Tested end-to-end with OUCH payload through real TCP loopback (`OuchTcpGatewayTest`).

### Market Data Stack
- **ITCH 5.0 publisher** (`ItchPublisher.h`): 10 message types — `S` SystemEvent, `R` Stock Directory, `H` Trading Action (engine `TradingState` → wire halt/resume), `A` AddOrder, `E`/`C` Executed, `X` Cancel, `D` Delete, `P` Trade, `Q` Cross Trade. Per-`OrderBook` `EventListener` that looks up resting orders via `book.getOrder()` to populate side/price fields the listener path doesn't carry.
- **MoldUDP64 multicast** (`MoldUDP64.h`): batched publisher with MTU auto-flush + gap-detecting subscriber. Heartbeat with `nextExpectedSeq` lets subscribers detect silent gaps even during quiet markets. Session-ID mismatch reported separately from gap.
- **Real UDP transport** (`ItchUdpTransport.h`): `ItchUdpPublisher` writes via `sendto`, `ItchUdpSubscriber` joins multicast group via `IP_ADD_MEMBERSHIP` and runs a recv thread feeding `MoldUDP64Subscriber`. Tested over real 127.0.0.1 sockets (`ItchUdpTransportTest`).
- **Integrated publish pipeline** (`ItchMarketDataFeed.h`): engine event → ITCH frame → [UDP datagram + journal record]. One per-frame `flush()` so each event lands as its own datagram with a deterministic sequence number.
- **Gap recovery** (`MoldPacketJournal.h` + `ItchRetransmissionService.h`): bounded ring of journaled MoldUDP64 messages backs a SoupBinTCP-over-TCP retransmission service. End-to-end test exercises: publisher records → subscriber gap → SoupBinTCP login + re-request → byte-exact replay.
- **Fan-out adapter** (`MultiplexListener.h`): `OrderBook` exposes a single `EventListener` slot; this composes N of them so OUCH + ITCH + structured-log can coexist on the same book.

### Regulatory & Risk
- **Self-Match Prevention (SMP)**: Cancel-Taker strategy
- **Volatility Circuit Breakers**: Configurable % price band halts
- **OTR Monitoring**: Order-to-Trade ratio tracking per participant
- **Kill Switch**: Instant cancellation of all orders for a participant across all symbols
- **LMM/DMM Market Maker Privileges**: `ParticipantRole` enum (Regular/LMM/DMM); ProRata matching guarantees 40% floor allocation to LMM/DMM orders; remainder distributed to privileged roles first
- **Pre-Trade Risk Limits**: Max order size, notional, and position limits per participant
- **Per-Participant Rate Limiting**: Token-bucket throttling at ingress (configurable rate + burst) to bound queue depth and tail latency
- **Queue-Depth Backpressure**: Rejects orders when queue exceeds configurable threshold, hard ceiling on queuing delay
- **Admin HTTP Server**: Real-time `GET /metrics`, `GET /otr`, `GET /book` JSON endpoints on port 8080; `/health` liveness probe; `/readyz` k8s readiness probe (HTTP 503 until warmup completes, then 200)

### Microstructure Research Infrastructure
- **Simulation Engine**: Multi-agent market simulator with `NoiseTrader`, `MarketMaker`, and `InformedTrader` agents; fully event-driven against the live `OrderBook`
- **Market Impact Models**: Almgren-Chriss optimal execution model + square-root market impact law for pre-trade analytics
- **Order Flow Analytics**: VPIN (Volume-Synchronized Probability of Informed Trading), Roll spread decomposition, queue imbalance signal, logistic fill-probability model
- **Spread Decomposition**: Huang-Stoll and Glosten-Harris decomposition — separates order processing, inventory holding, and adverse selection components
- **Execution Analytics & Optimal Scheduling**: Almgren-Chriss execution schedule with arrival-price and VWAP slippage tracking
- **Signal Generator & PaperTrader**: Multi-factor signal composition (momentum, spread, VPIN, imbalance); `PaperTrader` submits IOC orders on composite signal, tracks position and realized/unrealized P&L analytically
- **Calibration & Backtesting**: `CalibrationPipeline` (Nelder-Mead minimization of spread decomposition residuals), `BacktestEngine` (replay historical tape against research models), `ResearchDashboard` and `ResearchSerializer` for result persistence

### Reliability & Observability
- **CRC-32 Journaling**: Write-ahead log with crash recovery, atomic checkpoint, and deterministic replay via virtual clock; monotonic `steady_clock` timestamps (NTP-skew-safe); stale `.tmp` cleanup on startup
- **End-to-End Wired Primary-Backup Replication**: `ReplicationCoordinator` instantiated in `src/main.cpp` driven by `OB_NODE_ROLE` / `OB_NODE_ID` / `OB_PRIMARY_HOST` env vars; primary's `Journal::onCommit` hook ships each fsync-durable batch to backup, backup's `applyReplicatedEntry` writes to its own journal — empirically verified end-to-end via the chaos suite
- **Lease Propagation + Fencing**: Primary broadcasts `LeaseGrant` (durationMs) every heartbeat tick; backup's local lease state is refreshed via same-epoch-from-same-holder path; `BackupPromote` requires both heartbeat miss AND local lease expiry — prevents split brain under packet loss, asymmetric partition, and clock skew
- **TCP Auto-Reconnect**: `ReplicationTransport::receiveLoop` retries `connectTo()` against saved host/port after socket loss — backup re-establishes within milliseconds of primary recovery without external orchestration
- **Snapshot Catchup on Join**: When a backup connects, primary streams all currently-resting orders via `streamSnapshot` → `JournalEntry::Snapshot` messages; idempotent on receiver — closes the rolling-restart gap
- **Crash Recovery & Warm Standby**: `JournalFollower` — single-host automated recovery with `promote()` API and documented invariants
- **Deterministic Sequencing**: Monotonic `sequenceNumber` on all `Trade`, `OrderUpdate`, and `MarketDataUpdate` events for gap detection
- **GTD Virtual Clock**: `setExpiryClock(ClockFn)` seam for cross-day replay — DAY/GTD expirations are journaled and replayed identically
- **Structured Logging**: Pluggable `StructuredSink` API with `NullSink` (zero-cost default), `JsonStderrSink` (dev), and `CapturingSink` (tests)
- **Prometheus Metrics**: `MetricsRegistry` with atomic Counters, Gauges, Histograms; `/prometheus` text-exposition endpoint exposes `journal_entries_committed_total`, `replication_entries_shipped_total`, `replication_bytes_sent_total`, `replication_snapshot_streams_total`, `replication_snapshot_entries_total`
- **Webhook Alerting**: `AlertDispatcher` — background thread delivery to Slack, PagerDuty, or generic HTTP webhooks with configurable severity filtering
- **Config Management**: `Config` key-value loader with env var override (`OB_` prefix), type-safe getters, registered-listener callbacks on set/loadFile/loadMap. Hot-reload via SIGHUP wired — `--config PATH` flag, async-signal-safe `g_reload_config` atomic flag, config reloaded on signal receipt
- **End-to-End Latency Tracking**: Ingress timestamps on every order; per-thread `LatencyTracker` histograms for real P50/P99/P99.9 including queue delay
- **Hardware Timing**: `ManualBenchmark` uses raw platform timers — `mach_absolute_time` (Apple Silicon) / `rdtsc` (x86) — for sub-clock-quantum micro-measurement. The headline `HonestBenchmark` numbers below use `nowNs()`, which is `std::chrono::high_resolution_clock`
- **Docker Deployment**: Multi-stage `Dockerfile` + `docker-compose.yml` for primary-backup topology with health checks and journal volumes; `.dockerignore` excludes host build artifacts; entrypoint shim conditionally LD_PRELOADs `libfaketime` for chaos clock-skew scenarios

### Formal Verification & Chaos Engineering
- **TLA+ Specifications**: 12 specs total. `MatchingEngine.tla` — 454M states, 181M distinct, 0 violations. `Replication.tla` — realistic lease-propagation model (heartbeat timeout AND lease expiry required for `BackupPromote`, no god-mode `~primaryAlive` guard) verified at `MaxEntries=10` / `HeartbeatTimeout=3` / `LeaseTimeout=7`. Plus `MpscQueue`, `EngineConsumer`, `Snapshot` / `SnapshotLocked`, `Refinement`, `Auction`, `EpochDurability`, `FixSession`, `Oco`, `Risk`.
- **Live Multi-Container Chaos Suite**: 19 scenarios in `deploy/chaos/` running against real running binaries in Docker Compose. Empirically verifies `NoCommittedLoss` (every primary-committed entry survives `SIGKILL`), no-split-brain under partition / packet loss / asymmetric partition / clock skew, snapshot catchup on backup join, rolling restart, transport auto-reconnect, lease-fenced promotion, token-authenticated chaos injection, Prometheus replication counters. See [deploy/chaos/README.md](./deploy/chaos/README.md).
- **TSan**: `ReplicationProtocolTest` is TSan-clean (was previously excluded due to teardown races on non-atomic fds + non-atomic sendSeq_; closed by atomic fds + `shutdown(2)` wakeup + atomic sequence)
- **Shadow Mode**: Dual-book divergence detection — validated against deliberate FIFO violations with trade-level and snapshot-level comparison
- **Fault Injection**: `FaultInjector` singleton with 10+ injection points — journal short-writes, bit-flips, fsync failures, pool exhaustion, gateway fragmentation, spurious queue failures; zero-cost in production (`OB_ENABLE_FAULT_INJECTION` off)
- **Coverage-Guided Fuzzing**: libFuzzer harness for protocol parsing and order flow

## 📊 Performance Benchmarks

Measured using `HonestBenchmark` — a single deterministic order flow (50K orders, seed=42) fed through three paths on Apple Silicon ARM64, Clang C++20 -O3 -march=native (Release). Each order individually timed with `nowNs()` (`std::chrono::high_resolution_clock`).

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

### Binary Codec Performance

`./bin/BinaryCodecBenchmark` — pure encode/decode microbench, no engine, no transport. 2M iterations per row, Apple M3 Pro, Clang `-O3 -march=native`.

| Codec | Message | ns/op | M ops/s |
| :--- | :--- | --: | --: |
| OUCH 4.2 | EnterOrder encode (49B) | 112.0 | 8.9 |
| OUCH 4.2 | EnterOrder decode (49B) | 10.2 | 98.5 |
| ITCH 5.0 | AddOrder encode (36B) | 34.1 | 29.4 |
| **SBE** | **NewOrderV1 encode (32B)** | **1.0** | **1015** |
| **SBE** | **NewOrderV1 decode (32B)** | **0.5** | **2128** |

The 110× gap between SBE encode and OUCH encode is the cost of OUCH's ASCII-decimal field formatting (`snprintf`-equivalent per integer). SBE's native-endian binary writes are the limit of what's measurable.

## 🏗️ Architecture

```
        ORDER ENTRY                                MARKET DATA
        ───────────                                ───────────

  ┌──────────────┐   ┌──────────────┐         ┌──────────────────┐
  │ FIX 4.2/4.4  │   │   OUCH 4.2   │         │   ITCH 5.0       │
  │   (text)     │   │   (binary)   │         │   (binary)       │
  └──────┬───────┘   └──────┬───────┘         └────────▲─────────┘
         │                  │                          │
         │            ┌─────▼──────┐                   │
         │            │ SoupBinTCP │             ┌─────┴──────┐
         │            │  session   │             │ MoldUDP64  │
         │            └─────┬──────┘             │ multicast  │
         │                  │                    └─────▲──────┘
   ┌─────▼──────┐    ┌──────▼─────┐    ┌────────────┐  │
   │ FixSession │    │ OuchSession│    │ SbeSession │  │ ItchPublisher
   └─────┬──────┘    └──────┬─────┘    └─────┬──────┘  │  (per book)
         │                  │                │         │
         └──────────────────┼────────────────┘         │
                            ▼                          │
                  ┌───────────────────┐                │
   Admin :8080 ──▶│   MatchingEngine  │────events─────┘
                  │   (order router)  │
                  └─┬───┬───┬───┬─────┘
                    │   │   │   │
                ┌───▼─┐ │ ┌─▼───▼──┐
                │ Q[0]│ │ │ Q[1..N]│   MpscQueue per thread
                └──┬──┘ │ └──┬─────┘
                   │    │    │
              ┌────▼──┐ │ ┌──▼────┐
              │Thread0│ │ │Thread1│    Worker threads
              │ BTC,  │ │ │ SOL,  │    (symbol affinity)
              │ ETH   │ │ │ AVAX  │
              └───────┘ │ └───────┘
                        │
                 ┌──────▼──────┐    ┌──────────────┐    ┌─────────────────┐
                 │  OrderBook  │───▶│ MoldPacket   │◀───│ ItchRetransmit  │
                 │  ObjectPool │    │   Journal    │    │  Service (TCP)  │
                 │  Journal    │    └──────────────┘    └─────────────────┘
                 └─────────────┘     gap recovery        re-request replay
```

**Four protocols, one engine.** Drop in a different session class to change the wire format; the engine doesn't know which codec it's driving. The market data side runs in parallel: ITCH frames flow out via MoldUDP64 over UDP multicast, with a SoupBinTCP-based retransmission service backing gap recovery from a bounded packet journal.

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
./bin/GatewayProtocolTest     # Internal binary protocol schema evolution
./bin/GTDReplayTest           # Deterministic expiry replay
./bin/MdFeedSchemaCompatTest  # Market data feed compatibility

# Wire protocols
./bin/OuchSessionTest         # OUCH 4.2 codec + session (25 cases)
./bin/SoupBinTcpTest          # SoupBinTCP envelope, framer, session (19 cases)
./bin/OuchTcpGatewayTest      # OUCH + SoupBinTCP over real TCP loopback
./bin/ItchPublisherTest       # ITCH 5.0 codec + publisher (17 cases)
./bin/MoldUDP64Test           # MoldUDP64 multicast wrapper + gap detection
./bin/ItchUdpTransportTest    # ITCH over real UDP loopback
./bin/ItchRetransmissionTest  # Journal + SoupBinTCP-based gap recovery
./bin/ItchMarketDataFeedTest  # End-to-end engine → ITCH → UDP + retransmit
./bin/SbeProtocolTest         # SBE codec, schema versioning, forward-compat
./bin/SbeSessionTest          # SBE NewOrder → engine + OrderAck

# Benchmarks
./bin/ManualBenchmark         # Per-order processing latency
./bin/E2EBenchmark            # End-to-end latency (includes queue delay)
./bin/BinaryCodecBenchmark    # OUCH / ITCH / SBE encode-rate comparison
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
# Liveness / build / replication state
curl localhost:8080/health                # k8s liveness probe
curl localhost:8080/version               # gitSha + buildTime + engineVersion
curl localhost:8080/role                  # primary | backup | standalone, epoch, isLeader
curl localhost:8080/replication           # peerAlive, running, epoch
curl localhost:8080/journal/head          # last committed journal sequence

# Operational state
curl localhost:8080/metrics               # throughput & queue depth (JSON)
curl localhost:8080/prometheus            # Prometheus text exposition (incl. replication counters)
curl "localhost:8080/book?symbolId=0"     # L2 order book snapshot
curl "localhost:8080/audit?symbolId=0"    # spread / depth / level counts
curl "localhost:8080/otr?participantId=1" # order-to-trade ratio

# Chaos-only (gated by OB_CHAOS_INJECT=1; X-Chaos-Token required when OB_CHAOS_TOKEN is set)
curl -H "X-Chaos-Token: $TOK" \
     "localhost:8080/chaos/order?orderId=1&participantId=1&price=100000&qty=1&side=0"
```

### Replication (live binary)
The OrderEngine binary instantiates `ReplicationCoordinator` when `OB_NODE_ROLE` is set. Primary listens on `OB_REPLICATION_PORT` (default 9002); backup connects to `OB_PRIMARY_HOST`:`OB_PRIMARY_REPLICATION_PORT` and runs in replay mode until promotion. Set `OB_JOURNAL_PATH` to enable journal commit → backup shipping. The chaos suite (`docker compose -f deploy/chaos/docker-compose.chaos.yml up -d --build`) provides a fully-wired 1+1 topology.

## 🧪 Verification

**79 test executables** covering **426 CTest targets**[^1] across 11 categories:

| Category | Tests | Description |
|----------|-------|-------------|
| **Functional** | ManualTest, UnitTests (GTest) | All order types, matching, cancellation, edge cases |
| **Stress** | StressTest, ShardedBookStressTest, SustainedLoadTest | 5 multi-threaded scenarios, 4 concurrent producers, sustained load degradation detection |
| **Property** | PropertyTest, JournalReplayPropertyTest, SnapshotConsistencyTest | Randomized invariant checks, replay determinism |
| **Chaos** | FaultInjectorTest, PoolExhaustionTest, JournalChaosTest, JournalCorruptionTest, GatewayChaosTest, QueueChaosTest, CheckpointChaosTest, CombinedChaosTest | 10+ fault-injection points, short-writes, bit-flips, EAGAIN |
| **Integration** | GatewayIntegrationTest, FixTcpGatewayTest, AdminServerEndpointsTest, OuchTcpGatewayTest, ItchUdpTransportTest, ItchMarketDataFeedTest | Real-loopback TCP/UDP round-trip, FIX/OUCH/ITCH session, HTTP endpoints |
| **Protocol — text** | GatewayProtocolTest, MarketDataSchemaTest, MdFeedSchemaCompatTest, RejectReasonContractTest, SubmitResultTest | Binary wire format evolution, ShmHeader compat |
| **Protocol — binary** | OuchSessionTest, ItchPublisherTest, SoupBinTcpTest, MoldUDP64Test, SbeProtocolTest, SbeSessionTest | OUCH/ITCH/SBE codec + session layers, SoupBinTCP login/heartbeat, MoldUDP64 gap detection, SBE forward-compat |
| **Recovery** | JournalCrashTest, JournalFollowerTest, GTDReplayTest, ItchRetransmissionTest | Crash recovery, log-ship convergence, virtual-clock replay, **gap-recovery via SoupBinTCP re-request** |
| **HA & Ops** | ReplicationProtocolTest, ConfigTest, AlertDispatcherTest | Primary-backup replication, config loader, webhook alerts |
| **Shadow** | ShadowModeTest | Dual-book divergence detection, FIFO violation catching |
| **Benchmark** | BenchmarkRegression (GTest), BinaryCodecBenchmark | P99 latency regression gates, OUCH/ITCH/SBE encode-rate comparison |

[^1]: *CTest targets map to individual executables and GTest cases. The 426 targets encompass several hundred underlying assertions and scenarios; e.g. `OuchSessionTest` alone runs 25 internal cases.*

### Formal Verification (TLA+)

**12 TLA+ specifications**, model-checked with TLC:

| Specification | States | Invariants |
|--------------|--------|------------|
| `MatchingEngine.tla` | **454M generated, 181M distinct** | NoNegativeQuantity, FIFO_Preservation, GTD_Expiry_Correctness *(Model Scope: 2 participants, 2 price levels, 4 orders, qty 1-3)* |
| `Replication.tla` (lease-propagation model) | **1373 → 4192 states at MaxEntries=6 / 10**, 0 violations | NoCommittedLoss, NoDuplicateExecution, NoSplitBrain. Realistic promotion rule: backup must observe heartbeat-miss AND local-lease-expiry; no god-mode `~primaryAlive` guard. Bug-injected variant (lease check stripped) reproduces split brain in 188 states — confirms the verification is genuine. |
| `Refinement.tla` | written | Refinement mapping from spec to implementation behavior |
| `MpscQueue.tla` | ~250K | Lock-free ring buffer linearizability |
| `EngineConsumer.tla` | ~200K | Worker loop shutdown safety |
| `Snapshot.tla` / `SnapshotLocked.tla` | ~300K | Read/write mutex prevents torn snapshots (lock spec verifies the fix found via the unlocked spec) |
| `Auction.tla` | verified | Opening/closing auction uncross correctness, price collar admission |
| `EpochDurability.tla` | verified | Epoch-store durability invariant under crash |
| `FixSession.tla` | verified | FIX session state machine safety (logon/heartbeat/gap-fill) |
| `Oco.tla` | verified | OCO one-cancels-other atomicity |
| `Risk.tla` | verified | Pre-trade risk cap enforcement + tier aggregation, modelled as a reduced two-tier (Firm/Trader) abstraction. The C++ `HierarchicalRiskManager` is four-tier (Trader/Strategy/Account/Firm); the extra tiers are additional instances of the same per-tier check. |

### <a name="chaos-suite"></a>Live Chaos Suite

**19 scenarios** in `deploy/chaos/` run against the actual `OrderEngine` binary inside Docker Compose. They empirically verify properties the spec proves, plus operational properties the spec doesn't model. See [deploy/chaos/README.md](./deploy/chaos/README.md) for the full table and how to run them.

| # | Scenario | Measured behavior |
|---|---|---|
| 1 | Primary kill → backup detects | ~400 ms RTO (heartbeat-timeout bounded) |
| 2 | Bidirectional partition + heal | detect ~500 ms / ~3 ms, recover ~110 ms |
| 3 | Asymmetric partition (primary→backup only) | backup sees peer death, primary doesn't, no split brain |
| 4 | **Backup clock skew (+30s via libfaketime)** | no split brain across 4s of sampling |
| 5 | 30% packet loss on replication link | heartbeats survive, no split brain |
| 6 | Slow link (200 ms one-way latency) | bilateral peerAlive stable for 3s |
| 7 | Rolling backup restart | bilateral recover ~3 ms via auto-reconnect |
| 8 | **NoCommittedLoss under SIGKILL** | 200 orders → 192 primary-committed → 192 on backup, **0 lost** |
| 9 | **Snapshot catchup on late join** | 576 pre-existing entries replicated to joining backup |
| 10 | Pause / unpause primary | detect ~415 ms, recover ~2 ms |
| 11 | `/chaos/order` auth | rejects missing & wrong token; accepts correct |
| 12 | Prometheus replication counters | `_shipped_total`, `_bytes_sent_total`, `_snapshot_streams_total` all advance |
| 13–19 | Steady-state guards, no-split-brain checks, replication metrics | always-on regression pins |

### Shadow Mode Validation
- Dual `OrderBook` instances fed identical order streams
- Deliberately broken FIFO detected via trade-level comparison
- 200-order clean run verified zero false positives

## 📚 Documentation

| Document | Purpose |
|----------|---------|
| [Architecture.md](./Architecture.md) | Full technical deep-dive |
| [BENCHMARKS.md](./BENCHMARKS.md) | Three-path latency methodology + binary codec results |
| [PerformanceWhitepaper.md](./PerformanceWhitepaper.md) | Benchmark methodology & analysis |
| [docs/Verification.md](./docs/Verification.md) | TLA+ model checking results (MatchingEngine + Replication) |
| [deploy/chaos/README.md](./deploy/chaos/README.md) | Live chaos suite — 19 scenarios, how to run, scenario catalog |
| [docs/Compliance.md](./docs/Compliance.md) | Regulatory mapping (MiFID II / SEC Rule 15c3-5 / Reg NMS) |
| [docs/MemoryOrderingAudit.md](./docs/MemoryOrderingAudit.md) | Per-site memory-order analysis |
| [docs/Runbook.md](./docs/Runbook.md) | Operational procedures, incident response |
| [docs/CapacityPlanning.md](./docs/CapacityPlanning.md) | Memory, CPU, disk, network sizing |
| [docs/ProductionReadiness.md](./docs/ProductionReadiness.md) | Remaining production checklist |
| [config/engine.conf.example](./config/engine.conf.example) | All configuration keys with defaults |
| [PRODUCTION_ROADMAP.md](./PRODUCTION_ROADMAP.md) | Tier-1 production upgrade checklist — completed and planned work |

## 🔮 Remaining Work

Most of the original wire-protocol gap (FIX 4.4, OUCH, ITCH, SBE, SoupBinTCP, MoldUDP64, retransmission) is now closed. The honest list of what's still NOT done:

| Item | Status | Blocker |
|------|--------|---------|
| x86 Bare Metal Benchmarks | E2E bench exists | Multi-socket EC2 c5.metal instance |
| ~~`Replication.tla` TLC run~~ | ✅ **Realistic lease-propagation model verified at MaxEntries=10, 0 violations.** Bug-injected variant reproduces split brain — confirms verification is genuine | — |
| io_uring async journal writes | Implemented behind `#ifdef __linux__` (`fdatasync`/`F_FULLFSYNC` fallback elsewhere); pending x86 validation | Linux + `liburing` — **no special NIC** |
| DPDK kernel bypass | Architecture ready — the genuine hardware-blocked item | Linux + **dedicated NIC/ENI** (e.g. Solarflare/Onload) |
| Solarflare/Onload | Architecture ready | Solarflare hardware |
| Wire-to-wire latency measurement | E2E bench exists | Multi-host test rig |
| Auction uncross price discovery | ✅ Auction state machines implemented (PreOpen, AuctionOpen, AuctionClose, Halted, VolatilityAuction) and cross verified | Volume-maximization algorithm: max-qty uncross implemented |
| Schema-driven SBE codegen | Hand-coded v1/v2 + forward-compat proven | XML schema → codec generator (tooling) |
| ~~Cross-host failure-drill validation~~ | ✅ **19-scenario live chaos suite in `deploy/chaos/` — multi-container failover, partition, packet loss, clock skew, snapshot catchup, NoCommittedLoss all verified empirically** | True cross-physical-host still needs hardware |
| TSan coverage | `ReplicationProtocolTest` now TSan-clean (closed atomic-fd + sendSeq_ races); CI runs it under TSan. 24h+ soak still pending | Clock time |
| Real FIX path through chaos topology | `GatewayServer` runs its own engine — would need to merge with replicated `OrderEngine`. Same safety properties verified via `/chaos/order` + `FixTcpGatewayTest` | Architectural refactor of gateway/engine binary split |
| TLS + full auth on admin port | Token auth on `/chaos/order` only | Design decision: token-everywhere vs reverse-proxy vs mTLS |
| Regulatory submission (CAT / MiFID RTS 22) | Event pipeline + journal in place | Broker-dealer / venue registration |
| Clearing integration (DTCC / OCC / CME) | Trade event surface in place | Clearing membership |

---
*Developed for professional quantitative trading systems.*
*C++20 · 46.5K LOC · 79 test executables · 426 CTest targets · 19 chaos scenarios · 12 TLA+ specifications · 454M states verified on MatchingEngine.tla · Replication.tla verified under realistic lease-propagation model · TSan-clean replication transport*
