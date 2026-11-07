# Order Matching Engine: Tier-1 Production Roadmap

This document outlines the architectural, operational, and regulatory delta between the current hyper-optimized C++ matching engine prototype and a **Tier-1 Institutional Production Exchange** (e.g., NASDAQ, CME, or a top-tier Alternative Trading System).

While the core algorithmic latency (~40ns adds, ~70ns matches) is state-of-the-art for user-space, deploying a real financial venue requires solving hardware networking, distributed consensus, and rigorous mathematical verification.

---

## 1. True Kernel Bypass & Networking
To achieve physical wire-to-wire low latency, the OS networking stack must be entirely bypassed.
- [ ] **DPDK / Solarflare ef_vi:** Implement direct NIC-to-user-space memory access. (Note: `io_uring` is kernel-assisted, not true bypass.) A DPDK/F-Stack path was written, never run, and deleted (roadmap 1.2).
- [x] **One order-entry protocol:** the native binary protocol on `OrderEngine`'s order port. FIX, OUCH and SBE were written as library code, never served by a binary, and deleted (roadmap 1.2).
- [ ] **Multicast Market Data:** Replace local SHM IPC with UDP + PIM-SM Multicast for market data, ensuring A/B feed redundancy and snapshot + incremental recovery.
- [ ] **PCAP Tooling:** Build packet capture (PCAP) and replay tooling for zero-latency backtesting and postmortem analysis.

## 2. Distributed Consensus & High Availability (HA)
A production exchange must survive catastrophic hardware failure without losing a single financial transaction. This engine does not: it is a single node that recovers from its journal. Primary-backup replication was deleted rather than finished (no fencing, a promoted backup kept applying the old primary's stream, its send blocked the commit path, resync only added orders), so everything here starts again from a design.
- [ ] **Replicated State Machine (consensus):** Not implemented.
- [ ] **Hot Standby & Failover:** Not implemented. `--role` / `OB_NODE_ROLE` refuse to boot.
- [ ] **Cross-DC Disaster Recovery:** Not implemented.
- [ ] **Split-Brain Protection:** Not applicable to a single node; a prerequisite of any HA design.

## 3. Advanced Exchange Semantics
Real-world venues require complex business logic and state transitions to manage market integrity.
- [x] **Auction State Machines:** Implement Pre-Open, Opening/Closing crosses, Volatility Auctions, and Halt-Resume mechanics. Implemented: `TradingState` enum with PreOpen, AuctionOpen, AuctionClose, Halted, VolatilityAuction. Opening/closing crosses via `uncross()`. Verified by TLA+ `Auction.tla` + `TestAuctionSession` test suite.
- [x] **Market Protection:** Implement price collars and market-order protection (preventing sweeps that clear the entire book). LULD-style price collars (`priceBandPct_` admission filter), volatility circuit breakers halting into `VolatilityAuction` state, market-order protection against full-book sweep.
- [x] **Advanced Order Types:** Support Implied, Spread, Cross, Complex orders, OCO/OSO/Bracket, MIT, MOC/LOC, and Peg-to-Primary. MIT (Market-if-Touched), MOC (Market-on-Close), LOC (Limit-on-Close), OCO/OSO implemented. All existing types: Limit/Market/IOC/FOK/Stop/StopLimit/TrailingStop/Pegged/Iceberg/Hidden/PostOnly retained. Verified by `TestAdvancedOrders`, `TestMocLocOrders`.
- [x] **Venue-Specific Allocation:** Formalize priority tiers, LMM/DMM privileges, and exact Iceberg refresh / hidden-order allocation priorities. `ParticipantRole` enum (Regular/LMM/DMM). ProRata matching guarantees 40% floor allocation to LMM/DMM orders; rounding remainders go to privileged roles first. Iceberg refresh retains queue position. Verified by `TestLmmDmmAllocation`.
- [ ] **Expanded SMP Variants:** Add Cancel-Maker, Decrement-and-Cancel, and hierarchy-aware Self-Match Prevention (currently only Cancel-Taker).

## 4. Risk, Clearing, and Compliance
Handling institutional money requires rigorous pre-trade and post-trade guardrails.
- [x] **Multi-Tier Limits:** Persist limits across Firm, Account, Strategy, and Trader hierarchies. `HierarchicalRiskManager` enforces Firm/Account/Strategy/Trader hierarchy. Limits applied before order enters matching. Verified by `TestHierarchicalRisk`.
- [x] **Pre-Trade Fat-Finger Checks:** Block orders based on maximum notional size, reference price limits, and percentage away from the NBBO. Max notional, max order size, and percentage-from-NBBO checks in `HierarchicalRiskManager`. Verified by `TestHierarchicalRisk` and `TestCompliance`.
- [x] **Regulatory Audit Trail:** Support CAT (Consolidated Audit Trail), MiFID II transaction reports, and RTS 24/25 record retention. `CatReporter` writes NDJSON event records (order accept/cancel/fill) with nanosecond timestamps for CAT and MiFID II RTS 24/25. Hourly rotation. Verified by `TestCatReporter`.
- [ ] **Clock Synchronization:** Achieve PTP / NTP clock sync to MiFID II RTS 25 microsecond tolerances.
- [ ] **Clearing Handoff (STP):** Build post-trade Straight-Through Processing (STP) pipelines.
- [x] **Fee/Rebate Engine:** Real-time calculation of maker-taker schedules. `FeeEngine` computes maker-taker schedules on every fill event in real time. Verified by `TestFeeEngine`.

## 5. Formal Verification & QA
Unit tests are insufficient for lock-free concurrency. The engine must survive mathematical and adversarial scrutiny.
- [x] **TLA+ Modeling:** 8 specs. `MatchingEngine.tla` (price-time matching with STP, modify and replace): 2,726,157 distinct states at `MatchingEngine.cfg` (MaxOrders=3), checked in CI with seven broken variants that must fail. `MpscQueue.tla` proves linearizability. `Snapshot.tla` + `SnapshotLocked.tla` proved the snapshot fix. Also `EngineConsumer`, `Auction`, `Oco`, `Risk`. Details: docs/Verification.md.
- [ ] **Long-Running Soak Tests:** Harness exists (`scripts/tsan_soak.sh`); 24h+ runs still clock time bound.
- [x] **Coverage-Guided Fuzzing (partial):** `fuzz_gateway_engine` drives client bytes through `TcpGateway` into the engine (10 minutes in CI). Journal replay fuzzer in `tools/`.
- [ ] **Jepsen-Style Multi-Node Fault Injection:** Removed with replication: the multi-container suite only exercised that path. In-process fault injection (`FaultInjector`) remains.
- [x] **Byte-Identical Determinism:** `JournalReplayPropertyTest` verifies randomized command streams replay to the original final book state. `JournalCrashTest` covers crash recovery determinism.

## 6. Observability, Metrics & Ops
If you cannot measure it in production, you cannot operate it.
- [x] **Structured Logging:** `StructuredSink` API (`NullSink`, `JsonStderrSink`, `CapturingSink`) + 7 typed call sites wired in `OrderBook.cpp` (order accepted, cancelled, rejected, trade fill, circuit breaker, trading state change). Typed helper functions in `StructuredLog.h` enforce required fields. Backends swap without changing call sites. Verified by `StructuredLogTest`.
- [x] **Prometheus Metrics (partial):** `/prometheus` text-exposition endpoint exposes counters/gauges/histograms including `journal_entries_committed_total`. OpenTelemetry tracing not yet integrated.
- [x] **k8s Readiness Probe (`/readyz`):** Separate from the liveness `/health` probe. Returns HTTP 503 until `admin.setReady(true)` is called once the engine is up. Auth-exempt so k8s readinessProbe works without credentials. Verified by `AdminAuthTest`.
- [x] **`/version` Build-Metadata Endpoint:** Returns `gitSha` (12-char) + UTC `buildTime` + `engineVersion`. Captured at CMake configure time; falls back to `"unknown"` for Docker builds that exclude `.git/`.
- [ ] **Hardware Tuning Runbooks:** Document exact OS-level tuning including NUMA pinning, hugepages allocation, IRQ steering, and CPU core isolation. (See `docs/Runbook.md` for the runbook scaffold.)
- [x] **Configuration Management:** `Config` class with `OB_`-prefixed env override; per-key Listener callbacks fire on set/loadFile/loadMap. [x] **Config Hot-Reload via SIGHUP:** `--config PATH` CLI flag; async-signal-safe `g_reload_config` atomic; SIGHUP handler sets flag; main event loop calls `cfg.loadFile(configPath)` on receipt. Rate limiter live reconfiguration: SIGHUP propagates to `RateLimiter::reconfigure()` — all participant token buckets reset to new defaults so no client is grandfathered. Journal auto-rotation: `OB_JOURNAL_MAX_SIZE_MB` triggers `engine.checkpoint()` each main-loop tick when the size limit is exceeded.
- [x] **Protocol Versioning:** the order port answers V1 and V2 frames each in its own framing and refuses an unknown version (`GatewayIntegrationTest`); ITCH type codes and sizes are pinned by `ItchByteLayoutTest`.

## 7. Performance Methodology Upgrade
Benchmarking must move beyond localized user-space averages.
- [x] **HdrHistogram Tracking:** Implement coordinated-omission-free end-to-end latency tracking. `LatencyHistogram` header implements coordinated-omission-free end-to-end latency tracking with percentile export. Verified by `TestLatencyHistogram`.
- [ ] **Hardware Perf Counters:** Wire LLC misses and branch mispredictions directly into the benchmark suite.
- [ ] **Linux Isolated Cores Validation:** Transition benchmarks from macOS/M-series to production Linux servers with isolated cores and disabled hyperthreading.
