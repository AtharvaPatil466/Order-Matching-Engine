# High-Performance Order Matching Engine

> [!WARNING]
> **Single node. Not production-ready.** There is no replication or failover: a
> node recovers from its journal, and `--role` / `OB_NODE_ROLE` refuse to boot. <!-- claim:repl-refuses-role -->
> **One binary, one protocol.** `OrderEngine` is the only binary, and the native
> binary protocol over TCP (`TcpGateway`) is its only order-entry protocol.
> Started with `--journal`, it syncs each order to disk before acknowledging it. <!-- claim:gw-durable-ack -->
> An ack says the order is durable, not that the book took it: a reject decided
> while matching (post-only, FOK, a cancel of an unknown id) still answers Ack.
> ITCH and MoldUDP64 are library code it does not serve, and most risk and
> compliance controls below are library code it does not enable — see
> [docs/Compliance.md](./docs/Compliance.md).

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B20)
[![CI](https://github.com/AtharvaPatil466/Order-Matching-Engine/actions/workflows/ci.yml/badge.svg)](https://github.com/AtharvaPatil466/Order-Matching-Engine/actions/workflows/ci.yml)
[![Latency](https://img.shields.io/badge/Matching_P50_PGO-237ns-green.svg)](#performance)
[![Throughput](https://img.shields.io/badge/Throughput-3.10M_ops/s-blue.svg)](#performance)
[![Tests](https://img.shields.io/badge/Tests-CTest_suite-brightgreen.svg)](#verification)
[![TLA+](https://img.shields.io/badge/TLA%2B-8_specs-blueviolet.svg)](#formal-verification)
[![Order entry](https://img.shields.io/badge/Order_entry-native_binary_over_TCP-blueviolet.svg)](#order-entry)

A C++20 low-latency matching engine drawing on exchange design principles: O(1) price-level lookup via `FlatPriceMap`, lock-free MPSC queues, thread-per-symbol horizontal scaling, CRC-32 journaling with deterministic replay, one binary (`OrderEngine`) speaking one order-entry protocol (the native binary protocol over TCP), an ITCH 5.0 / MoldUDP64 market-data codec with gap recovery that is library code with tests, TLA+ model checking (`MatchingEngine.tla`: price-time matching with STP and modify/replace, 2.7M distinct states at small bounds <!-- claim:tla-matching-engine -->), a token-authenticated order injection endpoint for tests, and an operational stack (config management, Prometheus metrics, `/version` build-metadata endpoint, Docker deployment; `AlertDispatcher` is implemented but has no TLS client, so it cannot deliver to any `https://` webhook — see Project_Overview §11). **7 TLA+ specifications** (5 of them model code `OrderEngine` runs).

## 🔬 Research

This engine served as the experimental substrate for a published market microstructure paper testing the **Budish–Cramton–Shim (BCS) theory of the HFT arms race** — the argument that continuous limit-order-book markets structurally incentivize a socially-wasteful race for speed. Read it on SSRN: [**Experimental Evidence for the HFT Arms Race: Welfare Decomposition and Market Fragility on a Formally Verified Limit Order Book**](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=6994722).

The experiment code lives in [`bcs_research/`](./bcs_research/). The matching core's FIFO and conservation invariants are [model-checked](#formal-verification) at small bounds <!-- claim:tla-matching-engine -->, and the engine passes its CTest suite, including property and shadow-mode divergence checks. That is **not** a proof that the experiments are artifact-free, and an earlier version of this line claimed it was: the verified configuration is `MaxOrders=4`, two participants, two prices, with no STP, icebergs, pro-rata, auctions, pegs or stops — while the experiments drive millions of orders through market-maker and HFT agents, latency models and batch auctions, essentially none of which is in the model. What supports the results is the test suite and the finding that every qualitative result survives recalibration, not the state count.

## 🚀 Key Features

### Core Engine
- **Professional Order Types**: Limit, Market, IOC, FOK, Stop (a stop-market), StopLimit, TrailingStop, Iceberg, Hidden, PostOnly, MIT, MOC, LOC, Pegged (MidPeg / PrimaryPeg, referenced to non-pegged orders and never resting at or through the opposite touch). <!-- claim:peg-never-crosses --> See [docs/MatchingRules.md](./docs/MatchingRules.md) §5-6
- **O(1) Price Lookup**: `FlatPriceMap` — flat array indexed by price tick, replacing `std::map` red-black trees
- **Intrusive Data Structures**: Zero-heap matching via `ObjectPool` + intrusive doubly-linked order lists
- **Dual Match Algorithms**: Price-Time FIFO and Pro-Rata allocation with LMM/DMM floor guarantee (40% of available qty) and rounding-remainder priority <!-- claim:prorata-lmm-floor -->

### Concurrency & Networking
- **Thread-Per-Symbol Partitioning**: N worker threads with independent lock-free `MpscQueue` ring buffers, routed by `hash(symbolId) % numThreads`
- **TCP Gateway**: the native binary protocol (`TcpGateway.h`) with non-blocking I/O via `epoll`/`kqueue` — the only order-entry protocol, served by `OrderEngine` on `--gateway-port` (default 9001)
- **Shared Memory IPC**: `MarketDataPublisher` using POSIX `shm_open` with versioned `ShmHeader` (magic/version/entrySize) for prefix-compatible rolling upgrades. One writer at a time: `OrderEngine` publishes each book's updates from the worker that owns it, and a lock hands the ring between workers. It publishes no L2 snapshots, so a subscriber that gaps has no snapshot to resync from until one is added (roadmap 1.10). Each slot is a seqlock, so a lapped reader never takes a torn entry. <!-- claim:md-shm-no-torn-read --> `ShmHeader::epoch` names the publisher session, and `MarketDataSubscriber::poll()` returns `PollResult` — `Entry`, `Empty`, `Gap` (entries lost; resync from a snapshot) or `Reset` (publisher restarted or stopped; `connect()` again). <!-- claim:md-shm-gap-reset --> The segment is mode 0640 and owned by the publisher's user, so `MdSubscriber` runs as that user or in its group
- **Binary Protocol Versioning**: `TcpGateway.h` — a bare V1 `OrderRequest` frame or a V2 frame with a 24-byte header, answered in the same framing; a version the gateway does not know is refused <!-- claim:gw-proto-compat -->

### Order Entry
One protocol: the native binary protocol above, on `OrderEngine`'s order port. `MatchingEngine::submitOrder` range-checks side, order type, time in force and peg type for every ingress, the gateway and the admin port alike (`InvalidFieldValue`). <!-- claim:submit-field-validation --> FIX, OUCH, SBE, the DPDK path and the separate `GatewayServer` binary were deleted (roadmap 1.2): no binary served the protocols, and they carried open findings.

### Market Data Stack
**Library code.** `OrderEngine` does not publish ITCH or MoldUDP64 or run the retransmission service; it publishes to shared memory (`MarketDataPublisher`).
- **ITCH 5.0 publisher** (`ItchPublisher.h`): 10 message types — `S` SystemEvent, `R` Stock Directory, `H` Trading Action (engine `TradingState` → wire halt/resume), `A` AddOrder, `E`/`C` Executed, `X` Cancel, `D` Delete, `P` Trade, `Q` Cross Trade. An auction uncross goes out as non-printable `C` executions at the cross price plus one `Q` with the cross's price and volume, so its volume is counted once. A book with a publisher attached rejects at admission any price outside [0, 2^32) or quantity of 2^32 or more (`InvalidPrice` / `InvalidQuantity`), since ITCH carries both in 32 bits. <!-- claim:itch-32bit-admission --> In async mode an event that finds the publisher's ring full is dropped, and the feed then says so: an `S` with the non-standard code `R` (resync), a `D` for every order it had announced, and an `A` for every displayed order in priority order. <!-- claim:itch-ring-drop-resync --> Per-`OrderBook` `EventListener` that looks up resting orders via `book.getOrder()` to populate side/price fields the listener path doesn't carry.
- **MoldUDP64 multicast** (`MoldUDP64.h`): batched publisher with MTU auto-flush (default bound 1472 bytes, so a full packet fits one Ethernet frame unfragmented) + gap-detecting subscriber. The subscriber delivers messages in arrival order: after a gap, later messages arrive before the retransmitted ones, so a consumer that needs sequence order (an order book) buffers by sequence number and applies up to `nextExpectedSequence()`. Heartbeat with `nextExpectedSeq` lets subscribers detect silent gaps even during quiet markets. Session-ID mismatch reported separately from gap. <!-- claim:mold-gap-detection -->
- **Real UDP transport** (`ItchUdpTransport.h`): `ItchUdpPublisher` writes via `sendto`, `ItchUdpSubscriber` joins multicast group via `IP_ADD_MEMBERSHIP` and runs a recv thread feeding `MoldUDP64Subscriber`. Tested over real 127.0.0.1 sockets (`ItchUdpTransportTest`).
- **Integrated publish pipeline** (`ItchMarketDataFeed.h`): engine event → ITCH frame → [UDP datagram + journal record]. One per-frame `flush()` so each event lands as its own datagram with a deterministic sequence number.
- **Gap recovery** (`MoldPacketJournal.h` + `ItchRetransmissionService.h`): bounded ring of journaled MoldUDP64 messages backs a SoupBinTCP-over-TCP retransmission service. End-to-end test exercises: publisher records → subscriber gap → SoupBinTCP login + re-request → byte-exact replay. <!-- claim:itch-gap-recovery -->
- **Fan-out adapter** (`MultiplexListener.h`): `OrderBook` exposes a single `EventListener` slot; this composes N of them so ITCH + structured-log can coexist on the same book.

### Regulatory & Risk
In `OrderEngine`, self-match prevention (cancel-incoming), the 5% circuit breaker, the per-participant kill, OTR tracking and per-participant position limits (`participant.<id>.position_limit`, required for every participant the order port serves unless `--no-risk-limits`) are active. The other pre-trade risk limits (fat-finger, OTR throttle, hierarchical), LMM/DMM roles and pro-rata are library code that nothing enables; the rate limiter is off by default. See [docs/Compliance.md](./docs/Compliance.md).
- **Self-Match Prevention (SMP)**: Cancel-Taker strategy <!-- claim:stp-default-cancel-incoming -->
- **Volatility Circuit Breakers**: a fixed 5% threshold on the price of every continuous print, elected stops and replaces included, against a reference set by the first print; a breach puts the symbol into a volatility auction that reopens with a cross after 5 minutes (`volatility_auction_dwell_ms`). A passive order far from the market trips nothing <!-- claim:circuit-breaker -->
- **OTR Monitoring**: Order-to-Trade ratio tracking per participant <!-- claim:otr -->
- **Kill Switch**: Cancels every resting order of a participant on every symbol, each cancel journaled, before the call returns <!-- claim:kill-participant -->
- **LMM/DMM Market Maker Privileges**: `ParticipantRole` enum (Regular/LMM/DMM); ProRata matching guarantees 40% floor allocation to LMM/DMM orders; remainder distributed to privileged roles first <!-- claim:prorata-lmm-floor -->
- **Pre-Trade Risk Limits**: Max order size, notional, and position limits per participant <!-- claim:risk-limits -->
- **Per-Participant Rate Limiting**: Token-bucket throttling at ingress (configurable rate + burst), off by default <!-- claim:rate-limit -->
- **Queue-Depth Backpressure** (library only: `OrderEngine` sets no threshold): `setBackpressureThreshold` makes ingress refuse an order with `QueueBackpressure` once its worker's queue is past that fraction of capacity. No test drives a queue past the threshold, so nothing here bounds queuing delay
- **Admin HTTP Server**: Real-time `GET /metrics`, `GET /otr`, `GET /book` JSON endpoints on port 8080; `/health` liveness probe; `/readyz` k8s readiness probe (the admin port opens only after journal replay completes, so during replay both probes get connection refused; `/readyz` is 200 once the engine is up) <!-- claim:admin-readyz -->

### Microstructure Research Infrastructure
- **Simulation Engine**: Multi-agent market simulator with `NoiseTrader`, `MarketMaker`, and `InformedTrader` agents; fully event-driven against the live `OrderBook`
- **Market Impact Models**: Almgren-Chriss optimal execution model + square-root market impact law for pre-trade analytics
- **Order Flow Analytics**: VPIN (Volume-Synchronized Probability of Informed Trading), Roll spread decomposition, queue imbalance signal, logistic fill-probability model
- **Spread Decomposition**: Huang-Stoll and Glosten-Harris decomposition — separates order processing, inventory holding, and adverse selection components
- **Execution Analytics & Optimal Scheduling**: Almgren-Chriss execution schedule with arrival-price and VWAP slippage tracking
- **Signal Generator & PaperTrader**: Multi-factor signal composition (momentum, spread, VPIN, imbalance); `PaperTrader` submits IOC orders on composite signal, tracks position and realized/unrealized P&L analytically
- **Calibration & Backtesting**: `CalibrationPipeline` (Nelder-Mead minimization of spread decomposition residuals), `BacktestEngine` (replay historical tape against research models), `ResearchDashboard` and `ResearchSerializer` for result persistence

### Reliability & Observability
- **CRC-32 Journaling**: a journal replayed on boot <!-- claim:journal-replay-on-boot --> (one that does not decode is refused; a torn final write is cut off at boot and kept in `<journal>.torn`, never appended behind <!-- claim:journal-torn-tail -->), atomic checkpoint <!-- claim:checkpoint-atomic -->, and deterministic replay via virtual clock <!-- claim:gtd-replay -->. It is **not write-ahead**: the book applies an order before its entry is appended (`MatchingEngine.cpp`). `OrderEngine` syncs every entry and answers its order port, sends its execution reports and publishes its market data only after the worker has written it; the library's synchronous mode with durable client acks holds fills and order updates until the entry is synced. <!-- claim:gw-durable-events --> A write or sync error halts the journal, and from then on nothing changes a book: requests (the kill switch included) are refused `JournalHalted` and expiry and session sweeps are suspended until a restart (MatchingRules Rule 2.7). A checkpoint replaces the journal with a snapshot of resting orders, so it is not a retained history; monotonic `steady_clock` timestamps (NTP-skew-safe); stale `.tmp` cleanup on startup
- **Single Node, Journal Recovery**: one `OrderEngine` and no replication or failover; a restart replays the journal. Replication was deleted rather than finished, and `--role` / `OB_NODE_ROLE` refuse to boot, saying so. <!-- claim:repl-refuses-role -->
- **Deterministic Sequencing**: a book's `MarketDataUpdate` events carry strictly increasing `sequenceNumber`s, and a subscriber that applies only them holds the book's depth <!-- claim:md-l2-sequence -->. `Trade` and `OrderUpdate` carry one too; no test holds either to an order
- **GTD Virtual Clock**: `setExpiryClock(ClockFn)` seam for cross-day replay — DAY/GTD expirations are journaled and replayed identically <!-- claim:gtd-replay -->
- **Structured Logging**: Pluggable `StructuredSink` API with `NullSink` (zero-cost default), `JsonStderrSink` (dev), and `CapturingSink` (tests)
- **Prometheus Metrics**: `MetricsRegistry` with atomic Counters, Gauges, Histograms; `/prometheus` text-exposition endpoint exposes `journal_entries_committed_total` among others
- **Webhook Alerting**: `AlertDispatcher` — background thread delivery to Slack, PagerDuty, or generic HTTP webhooks with configurable severity filtering
- **Config Management**: `Config` key-value loader with env var override (`OB_` prefix), type-safe getters, registered-listener callbacks on set/loadFile/loadMap. Hot-reload via SIGHUP wired — `--config PATH` flag, async-signal-safe `g_reload_config` atomic flag, config reloaded on signal receipt; only `rate_limit.default_rate` / `rate_limit.default_burst` take effect (they also apply at startup), and many keys in `config/engine.conf.example` are read by nothing
- **End-to-End Latency Tracking**: Ingress timestamps on every order; per-thread `LatencyTracker` histograms for real P50/P99/P99.9 including queue delay
- **Hardware Timing**: `DeterministicBenchmark` reads the cycle counter — `rdtsc` (x86) / `cntvct_el0` (ARM) — for sub-clock-quantum micro-measurement. The headline `HonestBenchmark` numbers below use `nowNs()`, which is `std::chrono::high_resolution_clock`
- **Docker Deployment**: Multi-stage `Dockerfile` + `docker-compose.yml` for a single node with health checks and a journal volume; `.dockerignore` excludes host build artifacts

### Formal Verification & Fault Injection
- **TLA+ Specifications**: 7 specs total. `MatchingEngine.tla` — price-time matching with STP, modify and replace; 0 violations, 2,732,033 distinct states at `MatchingEngine.cfg` (MaxOrders=3) and 10,637,301 at `MatchingEngineDeep.cfg` (a fill reaches a level three deep), checked in CI with eight broken variants that must fail (see [docs/Verification.md](./docs/Verification.md)). <!-- claim:tla-matching-engine --> Plus `MpscQueue`, `EngineConsumer`, `SnapshotLocked`, `Auction`, and `Oco` / `Risk` (library code `OrderEngine` never calls).
- **Shadow Mode**: Dual-book divergence detection — validated against deliberate FIFO violations with trade-level and snapshot-level comparison <!-- claim:shadow-mode -->
- **Fault Injection**: `FaultInjector` singleton with 10+ injection points — journal short-writes, bit-flips, fsync failures, pool exhaustion, gateway fragmentation, spurious queue failures; compiled out unless built with `ENABLE_FAULT_INJECTION` (`OB_ENABLE_FAULT_INJECTION`)
- **Coverage-Guided Fuzzing**: a libFuzzer target, `fuzz_gateway_engine`, which feeds a client's bytes through `TcpGateway`'s framer into `MatchingEngine::submitOrder` and requires `validateIntegrity` on every book; CI's `gateway-engine-fuzz` job runs it for 10 minutes on every push and PR. See [fuzz/README.md](./fuzz/README.md)

## 📊 Performance Benchmarks

Measured using `HonestBenchmark` — a single deterministic order flow (50K orders, seed=42) fed through three paths on Apple Silicon ARM64, Clang C++20 -O3 -march=native (Release). Each order individually timed with `nowNs()` (`std::chrono::high_resolution_clock`).

> [!IMPORTANT]
> **Two machines:** the rows below are labelled x86 (AWS c6in.metal, commit `d2e688c`) or ARM (a single-socket Apple M-series dev machine), and must not be compared across that line. (This note used to say every number here was measured on Apple M-series, and quoted a 125 ns core-matching figure that is withdrawn.) Multi-thread scaling bounds do not factor in cross-NUMA cache coherence traffic.
> 
> These are **per-order processing latencies** on the matching thread. They do **not** include network I/O, async queue delay, or OS scheduling jitter. See [BENCHMARKS.md](./BENCHMARKS.md) for full methodology and caveats.

### Four benchmarks, four workload regimes

Four benchmarks characterize the engine and are complementary rather than competing. The two below cover the closed-loop latency floor and venue-shaped flow; `CoordinatedOmissionBenchmark` (open-loop, reports CO-naive *and* CO-corrected latency at a fixed offered rate) and `ColdCacheBenchmark` (working set evicted before each timed op) cover the two regimes a closed-loop steady-state benchmark structurally cannot see. Results for all four, with the ARM measurement-floor caveats, are in [BENCHMARKS.md](./BENCHMARKS.md).

| Benchmark | Platform | Workload | P50 | P99 | Ratio |
| :--- | :--- | :--- | ---: | ---: | ---: |
| **HonestBenchmark** | x86, PGO, commit `d2e688c` | 50K orders, seed=42, no cancels — dense resting book, predictable clustering (its "100% fill" counts accepted orders; the fill rate is unmeasured) | **237 ns** | 910 ns | 3.8× |
| **RealisticFlowBenchmark** | **ARM, indicative** | 500K events, 44% cancel / 46% new / 8% IOC / 2% modify, mean resting depth 2,418 | 208 ns combined | 709 ns | 3.4× |

> [!WARNING]
> **These two rows are measured on different machines and must not be compared to each other.** The 208 ns ARM figure is *not* evidence that realistic flow is faster than the 237 ns x86 floor — it is a different CPU with a coarser clock. `RealisticFlowBenchmark` has never been run on x86.

`HonestBenchmark` is the controlled reproducible baseline — the floor on matching latency under favourable conditions, and the flow every three-path figure below is measured on. It is **closed-loop with no cancels**, so it is a floor rather than an expected operating number. `RealisticFlowBenchmark` models venue-shaped flow against a sustaining resting book, with no empty-book cancels (0 of 217,256) — though cancels of ids an IOC already filled are timed as real cancels and not counted; its cancel P50 sits *at* the ~42 ns ARM clock tick and is therefore not a measurement on this box (the published run's recorder dropped the 26.4% of cancels that finished inside one tick; `6c93b11` now counts them), and x86 TSC resolves it. It was rewritten from a prior version whose cancel fraction exceeded its new-order fraction, draining the resting pool to empty and measuring an empty book at venue-shaped labels.

> [!NOTE]
> None of the four benchmarks represents a **cancel-heavy sparse-book regime** (cancel-to-trade ratios north of 20:1, price levels churn, book depth near zero). That regime stresses `FlatPriceMap` traversal over sparse slots, cancel-path hash lookups on recently-consumed IDs, and object pool churn. It is the planned next workload addition. See [BENCHMARKS.md](./BENCHMARKS.md).

### Three-Path Latency (identical order flow, x86 AWS c6in.metal, commit `d2e688c`)

| Path | What's Included | P50 | P90 | P99 | Throughput |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Core matching** | OrderBook + STP | **261 ns** | 620 ns | 1,010 ns | 2.80M ops/s |
| **Engine wrapper** | + sequence alloc, rate-limiter check (limiter disabled, its default) | 269 ns | 620 ns | 1,001 ns | 2.74M ops/s |
| **Full-stack journal** | + async io_uring ack (batch=64; journal on `/tmp`, backing not recorded; timing stops before durability) | 615 ns | 1,048 ns | 3,568 ns | 1.28M ops/s |

> [!NOTE]
> x86 figures (AWS c6in.metal, dual Xeon Platinum 8375C, 50K orders seed=42) measured at commit `d2e688c`, 340 commits before this revision; no raw log is committed and they have not been re-run since. The Apple M3 Pro reference of ~125 ns P50 that used to sit here did not reproduce and is withdrawn. See [BENCHMARKS.md](./BENCHMARKS.md).
>
> The full-stack journal path now uses the **async io_uring ack** (`onDurable` fires on the completion reaper, not on submit): Path C P99 dropped **32.8%** (5,312 → 3,568 ns) versus the synchronous `fdatasync` path, trading ~167 ns of added P50.
>
> **PGO:** Clang IR-based profile-guided optimization (profiled on the seed=42 HonestBenchmark workload) takes Path A core matching to **P50 237 ns / P99 910 ns / 3.10M ops/s** — the headline figure above. The three-path table is the standard (non-PGO) Release build.

### Multi-Threaded Stress Test (4 threads)
| Scenario | Orders | Trades | Result |
| :--- | :--- | :--- | :--- |
| Concurrent Producers | 20,000 | ~6,300 | ✅ |
| Mixed Workload | 20,000 | ~5,800 | ✅ |
| Kill Switch Under Load | 11,000 | — | ✅ |
| Multi-Symbol (4 symbols) | 12,000 | ~5,900 | ✅ |
| High-Throughput Burst | 50,000 | ~19,500 | ✅ 5M ops/s |

### Binary Codec Performance

`./bin/BinaryCodecBenchmark` — pure encode microbench, no engine, no transport. 2M iterations, Apple M3 Pro, Clang `-O3 -march=native`.

| Codec | Message | ns/op | M ops/s |
| :--- | :--- | --: | --: |
| ITCH 5.0 | AddOrder encode (36B) | 34.1 | 29.4 |

## 🏗️ Architecture

```
   clients ──TCP──▶ TcpGateway (login, then orders)          OrderEngine (one process)
                        │
   Admin :8080 ──▶ MatchingEngine (order router) ──▶ Q[0] .. Q[N]   MpscQueue per worker
                                                        │
                                       Worker threads (symbol affinity)
                                                        │
                                OrderBook ─ Journal (replayed at boot, synced per ack)
                                    │
                                    └─▶ market data: /orderbook_md (shared memory)

   Library only, not served: ItchPublisher ─▶ MoldUDP64 multicast,
                             MoldPacketJournal ◀─ ItchRetransmissionService (SoupBinTCP)
```

**One binary, one protocol.** `OrderEngine` takes orders on its TCP port, matches them on per-symbol workers and journals them; nothing else takes orders. The ITCH side is library code: frames can flow out via MoldUDP64 over UDP multicast, with a SoupBinTCP-based retransmission service backing gap recovery from a bounded packet journal, but `OrderEngine` wires none of it.

See [Architecture.md](./Architecture.md) for the full technical deep-dive.
See [docs/ProductionReadiness.md](./docs/ProductionReadiness.md) for the remaining work required before treating this as a production exchange component.

## 🚦 Getting Started

### Prerequisites
- Clang 14+ or GCC 11+ (C++20 support)

### Build & Run
```bash
# Build with all enterprise features
./build.sh

# Build lean mode (risk/stats compiled out; no lean latency has been published)
./build.sh --lean

# Run the engine (4 threads, admin on port 8080, orders on 9001). It refuses to
# start without each of these, unless that one is waived by name:
#   admin token (OB_ADMIN_TOKEN / --admin-token)        or --admin-no-auth
#   participant credentials (--participant-credentials) or --no-participant-auth
#   a journal (--journal)                               or --no-journal
#   a price band per symbol (instrument.<s>.min_price/max_price in --config)
#                                                       or --no-price-band
#   a position limit per participant the port serves (participants = ...,
#   participant.<id>.position_limit in --config)        or --no-risk-limits
OB_ADMIN_TOKEN=... ./bin/OrderEngine --threads 4 --port 8080 --symbols 4 \
    --config /etc/orderbook/engine.conf \
    --participant-credentials /etc/orderbook/participants.conf --journal /var/lib/orderbook/engine.wal

# Run tests
./bin/ManualTest              # Functional tests
./bin/StressTest              # Multi-threaded stress tests
./bin/PropertyTest            # Randomized invariant checks
./bin/JournalCrashTest        # Crash recovery validation
./bin/GTDReplayTest           # Deterministic expiry replay
./bin/MdFeedSchemaCompatTest  # Market data feed compatibility

# Market data (library)
./bin/SoupBinTcpTest          # SoupBinTCP envelope, framer, session
./bin/ItchPublisherTest       # ITCH 5.0 codec + publisher (17 cases)
./bin/MoldUDP64Test           # MoldUDP64 multicast wrapper + gap detection
./bin/ItchUdpTransportTest    # ITCH over real UDP loopback
./bin/ItchRetransmissionTest  # Journal + SoupBinTCP-based gap recovery
./bin/ItchMarketDataFeedTest  # End-to-end engine → ITCH → UDP + retransmit

# Benchmarks
./bin/HonestBenchmark         # Per-order latency: book, engine, engine + journal
./bin/E2EBenchmark            # End-to-end latency (includes queue delay)
./bin/BinaryCodecBenchmark    # ITCH AddOrder encode rate
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

When `OB_ADMIN_TOKEN` (or `--admin-token`) is set, every endpoint except the
k8s probes (`/health`, `/readyz`) requires `Authorization: Bearer <token>`
(constant-time compare, 401 otherwise). <!-- claim:admin-auth --> The production compose fails closed
if the token is unset.

The port listens on `127.0.0.1` unless `--admin-bind` / `OB_ADMIN_BIND` widens
it. The Docker image sets `OB_ADMIN_BIND=0.0.0.0`, because a published port or
a k8s probe arrives on the container's interface, not its loopback. Running the
binary directly on a host that others need to reach? Pass `--admin-bind`. <!-- claim:admin-loopback -->

```bash
AUTH="Authorization: Bearer $OB_ADMIN_TOKEN"

# Probes — always credential-free (k8s liveness/readiness)
curl localhost:8080/health                # k8s liveness probe
curl localhost:8080/readyz                # refused while the journal replays, 503 until the engine is up, then 200

# Liveness / build state
curl -H "$AUTH" localhost:8080/version               # gitSha + buildTime + engineVersion
curl -H "$AUTH" localhost:8080/journal/head          # last committed journal sequence

# Operational state
curl -H "$AUTH" localhost:8080/metrics               # throughput & queue depth (JSON)
curl -H "$AUTH" localhost:8080/prometheus            # Prometheus text exposition
curl -H "$AUTH" "localhost:8080/book?symbolId=0"     # L2 order book snapshot
curl -H "$AUTH" "localhost:8080/audit?symbolId=0"    # spread / depth / level counts
curl -H "$AUTH" "localhost:8080/otr?participantId=1" # order-to-trade ratio

# Test-only order injection (off unless OB_CHAOS_INJECT=1 and OB_CHAOS_TOKEN are set; needs a matching X-Chaos-Token)
curl -H "X-Chaos-Token: $TOK" \
     "localhost:8080/chaos/order?orderId=1&participantId=1&price=100000&qty=1&side=0"
```

### Order Entry Journal

`OrderEngine` journals every order it accepts when given `--journal PATH` (or
`OB_JOURNAL_PATH`), and replays that journal before its order port opens,
refusing unreadable and legacy journals. Without one it refuses to start unless
given `--no-journal` (`OB_NO_JOURNAL=1`), under which every acknowledged order
lives only in memory and is lost on any restart. Likewise it refuses to start without a
price band for every symbol (`--no-price-band`) or a position limit for every
participant its credentials name (`--no-risk-limits`), and on a config file it
cannot read. <!-- claim:boot-requires-controls -->

```bash
OrderEngine --participant-credentials /etc/orderbook/participants.conf \
            --journal /var/lib/orderbook/engine.wal
```

With a journal, **acks are durable**: the journal syncs every entry as it is
written, and the order port answers only after the worker has run the request,
so an order the client was told it has survives `kill -9` sent the moment the ack
arrives — the scenario `GatewayJournalRestartTest`, `CrashRecoveryE2ETest` and
`OrderEngineGatewayTest` run against the real binary. <!-- claim:gw-durable-ack -->
In the library's synchronous mode with durable client acks on, fills and order
updates are held until durable too. <!-- claim:gw-durable-events --> `OrderEngine`
holds its execution reports and its market-data feed to the same rule: what a
request produced leaves only once that request's journal entries are synced, and
never if the journal refused them. <!-- claim:gw-exec-reports -->

The cost is one fsync per order, and the order port's single event thread waits
it out, so order-entry throughput is bounded by your disk's fsync latency.
Measure it on the hardware you deploy to; this repository's published
throughput figures do not apply to this configuration.

### Order Entry Authentication

`OrderEngine` refuses to start unless participant credentials are configured
or authentication is explicitly turned off — the same bar the admin port has
held for a while, applied to the port that moves money. <!-- claim:gw-auth-required -->

```bash
# One credential per line: user:secret:participantId[,participantId...]
# Participant ids must be below 1024; a larger one is a config error.
cat > /etc/orderbook/participants.conf <<'EOF'
firm-a:s3cret:100,101
firm-b:0th3r:200
EOF
chmod 600 /etc/orderbook/participants.conf    # warned about if group/other-readable

OrderEngine --participant-credentials /etc/orderbook/participants.conf
# or: OB_PARTICIPANT_CREDENTIALS=/etc/orderbook/participants.conf OrderEngine

# Deliberately unauthenticated and uncontrolled (dev only)
OrderEngine --no-participant-auth --no-journal --no-price-band --no-risk-limits
```

The order port listens on `--gateway-port` (default 9001; `OB_GATEWAY_PORT`, or
`gateway_port` in the config file) at `--gateway-bind` (default `0.0.0.0`).

A malformed line fails the whole load rather than being skipped: a typo that
silently dropped a firm's credential would surface as that firm being unable to
trade, at the worst possible moment. <!-- claim:gw-credentials-malformed -->

Clients authenticate per connection: a login frame — the V2 header's `flags`
word with `GATEWAY_FLAG_LOGIN` set, carrying `user`/`secret` — sent before the
first order. A session may then only submit as a participant its credential
covers, which includes the kill switch. A failed login closes the connection. <!-- claim:gw-login -->

### Order Entry Protocol

Order ids are the client's own and are scoped by participant: the book holds
`(participantId << 54) | orderId`, so two participants may use the same id on
one symbol, neither can see nor touch the other's order by id, and a cancel,
modify or replace names its order by the participant and id it was entered
with. Every id the port sends back is the client's. A request whose participant
id is 1024 or more, or whose order id is 2^54 or more, is refused with
`InvalidFieldValue` whatever its type, the kill switch included.
<!-- claim:gw-ids-per-participant -->

Besides the Ack or Error answering each request, the port sends execution
reports unasked (V2 `clientRequestId` 0), so a client switches on the response
`type` first. A `TradeNotification` goes to the aggressor and to each maker for
every fill, carrying the trade id, price and quantity and the order's cumulative
and leaves; the contra side stays anonymous. An `OrderUpdateNotification` goes
out for every status change: `Accepted`, the cancels the engine makes (IOC, FOK
and Market remainders, expiry, kill switch, session end, self-trade prevention)
and a new order refused after its Ack. They reach every session that may act
for the order's participant; one with no session is not told later. A session
that stops reading is disconnected once its queued output reaches the
per-session ceiling (`TcpGateway::kMaxWriteBufBytes`). A cancel, modify or
replace is answered Ack once it has run; whether it took effect follows as an
`OrderUpdateNotification`: the order restated with its new leaves, or the
refusal's reason with the order unchanged (`Rejected`/`OrderNotFound` when there
is no such order). Not yet reported: a stop's election before it trades (see
MatchingRules Rule 2.6).
<!-- claim:gw-exec-reports -->

### Replication: removed
`OrderEngine` runs as a single node and recovers from its journal. Primary/backup replication had no fencing, a promoted backup kept applying the old primary's stream, its send blocked the journal commit path and resync only added orders, so it was deleted rather than finished. `--role` / `OB_NODE_ROLE` still refuse to boot, so a deployment that sets one finds out at startup. <!-- claim:repl-refuses-role -->

## 🧪 Verification

The CTest suite[^1] (`ctest -N` lists every target), by category:

| Category | Tests | Description |
|----------|-------|-------------|
| **Functional** | ManualTest, UnitTests (GTest) | All order types, matching, cancellation, edge cases |
| **Stress** | StressTest, SustainedLoadTest | 5 multi-threaded scenarios, 4 concurrent producers, sustained load degradation detection |
| **Property** | PropertyTest, JournalReplayPropertyTest, SnapshotConsistencyTest | Randomized invariant checks, replay determinism |
| **Chaos** | FaultInjectorTest, PoolExhaustionTest, JournalChaosTest, JournalCorruptionTest, GatewayChaosTest, QueueChaosTest, CheckpointChaosTest, CombinedChaosTest | 10+ fault-injection points, short-writes, bit-flips, EAGAIN |
| **Integration** | OrderEngineGatewayTest, GatewayIntegrationTest, AdminServerEndpointsTest, ItchUdpTransportTest, ItchMarketDataFeedTest | The real binary over its order port, real-loopback TCP/UDP round-trip, HTTP endpoints |
| **Protocol** | GatewayIntegrationTest, MarketDataSchemaTest, MdFeedSchemaCompatTest, RejectReasonContractTest, SubmitResultTest | V1/V2 gateway frames, ShmHeader compat, reject strings |
| **Market data (library)** | ItchPublisherTest, ItchByteLayoutTest, SoupBinTcpTest, MoldUDP64Test | ITCH codec + publisher, SoupBinTCP login/heartbeat, MoldUDP64 gap detection |
| **Recovery** | JournalCrashTest, GTDReplayTest, ItchRetransmissionTest | Crash recovery, virtual-clock replay, **gap-recovery via SoupBinTCP re-request** |
| **Ops** | ConfigTest, AlertDispatcherTest | Config loader, webhook alerts |
| **Shadow** | ShadowModeTest | Dual-book divergence detection, FIFO violation catching |
| **Benchmark** | BenchmarkRegression (GTest), BinaryCodecBenchmark | P99 latency regression gates, ITCH encode rate |

[^1]: *A CTest target is a test executable or a single GTest case, and one executable can run many internal cases (`ItchPublisherTest` alone runs seventeen).*

### Formal Verification (TLA+)

**7 TLA+ specifications**, model-checked with TLC (scope, and which run in `OrderEngine`: [docs/Verification.md](./docs/Verification.md)):

| Specification | States | Invariants |
|--------------|--------|------------|
| `MatchingEngine.tla` | **2,732,033 distinct, 0 violations** (`MatchingEngine.cfg`: 3 orders, 1 replace, 2 participants, 2 prices, qty 1-3) and **10,637,301** (`MatchingEngineDeep.cfg`: 4 orders at one price, so a fill reaches a level three deep); both checked in CI | PricePriority, FIFOExecution, ExecPriceIsPassive, NoCrossedBookAfterStep, NoSelfTrade, NoEmptyRestingOrder, FIFO_Preservation, MatchingConservation, NoNegativeQuantity. Limit orders, default STP, modify and replace only; each of eight broken cfgs must fail. See [docs/Verification.md](./docs/Verification.md) <!-- claim:tla-matching-engine --> |
| `MpscQueue.tla` | 3,672 distinct | No item invented or duplicated, per-producer FIFO, every item eventually consumed (not linearizability) <!-- claim:tla-concurrency-specs --> |
| `EngineConsumer.tla` | 323 distinct | Worker loop: no item invented or duplicated, every item eventually consumed, no lost wake-up (a broken cfg that samples the wake counter too late must deadlock) <!-- claim:tla-concurrency-specs --> |
| `SnapshotLocked.tla` | 221,140 distinct | Holding `bookLock_` across the whole 2-step snapshot read prevents torn snapshots; a broken cfg without the lock must produce a torn one <!-- claim:tla-concurrency-specs --> |
| `Auction.tla` | **199,348 distinct, 0 violations** (`Auction.cfg`) | Every session transition goes through the table `OrderBook::transitionAllowed` enforces (Rule 7.7); no match outside Continuous except the uncross; no trade while halted or closed; volatility auction ends; once a session reaches its closing auction or close, nothing trades or auctions until the next PreOpen (a halt resumes only to the state it paused). Three broken cfgs with an illegal transition or a halt detour must fail, and one that never reopens a volatility auction; see [docs/Verification.md](./docs/Verification.md) <!-- claim:tla-auction --> |
| `Oco.tla` | 198 distinct | OCO one-cancels-other atomicity. Library only: `OrderEngine` never registers an OCO pair <!-- claim:tla-small-specs --> |
| `Risk.tla` | 800 distinct | Library only (`OrderEngine` sets no tier or position limit). Pre-trade risk cap enforcement + tier aggregation, modelled as a reduced two-tier (Firm/Trader) abstraction. The C++ `HierarchicalRiskManager` is four-tier (Trader/Strategy/Account/Firm); the extra tiers are additional instances of the same per-tier check. <!-- claim:tla-small-specs --> |

### Shadow Mode Validation
- Dual `OrderBook` instances fed identical order streams
- Deliberately broken FIFO detected via trade-level comparison
- 200-order clean run verified zero false positives <!-- claim:shadow-mode -->

## 📚 Documentation

| Document | Purpose |
|----------|---------|
| [Architecture.md](./Architecture.md) | Full technical deep-dive |
| [docs/MatchingRules.md](./docs/MatchingRules.md) | **The rulebook** — iceberg/pro-rata allocation, STP, stop-election bounds, pegs, auction uncross, expiry, and the suspected defects |
| [BENCHMARKS.md](./BENCHMARKS.md) | Three-path latency methodology + binary codec results |
| [PerformanceWhitepaper.md](./PerformanceWhitepaper.md) | Benchmark methodology & analysis |
| [docs/Verification.md](./docs/Verification.md) | TLA+ model checking results |
| [docs/Compliance.md](./docs/Compliance.md) | Regulatory mapping (MiFID II / SEC Rule 15c3-5 / Reg NMS) |
| [docs/Runbook.md](./docs/Runbook.md) | Operational procedures, incident response |
| [docs/CapacityPlanning.md](./docs/CapacityPlanning.md) | Memory, CPU, disk, network sizing |
| [docs/ProductionReadiness.md](./docs/ProductionReadiness.md) | Remaining production checklist |
| [config/engine.conf.example](./config/engine.conf.example) | All configuration keys with defaults |
| [PRODUCTION_ROADMAP.md](./PRODUCTION_ROADMAP.md) | Tier-1 production upgrade checklist — completed and planned work |

## 🔮 Remaining Work

The market-data codecs (ITCH, SoupBinTCP, MoldUDP64, retransmission) exist as library code with tests, but `OrderEngine` serves none of them. The honest list of what's still NOT done:

| Item | Status | Blocker |
|------|--------|---------|
| x86 Bare Metal Benchmarks | E2E bench exists | Multi-socket EC2 c5.metal instance |
| io_uring async journal writes | Implemented behind `#ifdef __linux__` (`fdatasync`/`F_FULLFSYNC` fallback elsewhere); the `Ubuntu io_uring Journal` CI lane builds it and runs the suite on it (with a runtime probe, so a silent fallback cannot pass); latency not yet measured on x86 | Linux + `liburing` — **no special NIC** |
| Solarflare/Onload | Architecture ready | Solarflare hardware |
| Wire-to-wire latency measurement | E2E bench exists | Multi-host test rig |
| Auction uncross price discovery | ✅ Auction state machines implemented (PreOpen, AuctionOpen, AuctionClose, Halted, VolatilityAuction) and cross verified <!-- claim:auction-uncross --> | Volume-maximization algorithm: max-qty uncross implemented |
| TSan coverage | CI runs the project tests under TSan. 24h+ soak still pending | Clock time |
| TLS + full auth on admin port | Token auth on `/chaos/order` only | Design decision: token-everywhere vs reverse-proxy vs mTLS |
| Regulatory submission (CAT / MiFID RTS 22) | Event pipeline + journal in place | Broker-dealer / venue registration |
| Clearing integration (DTCC / OCC / CME) | Trade event surface in place | Clearing membership |

---
*Developed for professional quantitative trading systems.*
*C++20 · 39.4K LOC engine (73.5K with tests) · 7 TLA+ specifications · MatchingEngine.tla checked in CI*
